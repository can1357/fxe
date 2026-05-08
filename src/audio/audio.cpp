// Engine-level audio. Wraps miniaudio's high-level ma_engine + ma_sound API.
//
// Design notes:
//   * A single process-wide ma_engine drives playback. miniaudio is happy
//     to operate without a window/context; on macOS it goes through CoreAudio.
//   * `sound_handle` is an int slot into an internal vector. We never hand
//     raw ma_sound* pointers across the engine boundary so loaded sounds can
//     migrate slots if we ever compact (today: stable, free-list style).
//   * Loading from bytes uses a custom decoder that reads from an owned
//     std::vector<u8>. The vector outlives the ma_sound.

#include "audio.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <cstring>
#include <fxe/types.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fxe::audio {
  namespace {

    struct sound_slot {
      bool in_use = false;
      bool sound_initialised = false;
      ma_sound sound{};
      // For load_from_bytes: backing buffer + decoder owned by the slot.
      std::vector<u8> bytes;
      bool decoder_initialised = false;
      ma_decoder decoder{};
    };

    struct capture_slot {
      bool in_use = false;
      bool context_initialised = false;
      bool device_initialised = false;
      ma_context context{};
      ma_device device{};
      ma_device_id selected_id{};
      captured_audio_callback callback;
      u32 channels = 0;
      u32 sample_rate = 0;
    };

    struct engine_state {
      std::mutex mu;
      bool initialised = false;
      ma_engine engine{};
      std::vector<sound_slot> slots;
      std::vector<int> free_list;
      std::vector<std::unique_ptr<capture_slot>> capture_slots;
      std::vector<int> capture_free_list;
    };

    engine_state& state() {
      static engine_state s;
      return s;
    }

    thread_local audio_error g_last_error = audio_error::ok;

    audio_error set_last_error(audio_error error) {
      g_last_error = error;
      return error;
    }

    sound_handle fail_load(audio_error error) {
      set_last_error(error);
      return {};
    }

    capture_handle fail_capture(audio_error error) {
      set_last_error(error);
      return {};
    }

    audio_error map_resource_error(ma_result r, audio_error fallback) {
      if (r == MA_OUT_OF_MEMORY || r == MA_NO_SPACE)
        return audio_error::out_of_slots;
      return fallback;
    }

    audio_error map_path_error(ma_result r) {
      if (r == MA_OUT_OF_MEMORY || r == MA_NO_SPACE)
        return audio_error::out_of_slots;
      switch (r) {
      case MA_DOES_NOT_EXIST:
      case MA_ACCESS_DENIED:
      case MA_TOO_MANY_OPEN_FILES:
      case MA_PATH_TOO_LONG:
      case MA_IO_ERROR:
      case MA_BAD_SEEK:
        return audio_error::io_failed;
      default:
        return audio_error::decode_failed;
      }
    }

    std::string encode_device_id(const ma_device_id& id) {
      static constexpr char k_hex[] = "0123456789abcdef";
      const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
      std::string out;
      out.resize(sizeof(ma_device_id) * 2);
      for (usize i = 0; i < sizeof(ma_device_id); ++i) {
        out[i * 2] = k_hex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = k_hex[bytes[i] & 0x0f];
      }
      return out;
    }

    void capture_data_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count) {
      if (device == nullptr || input == nullptr || frame_count == 0)
        return;
      auto* slot = static_cast<capture_slot*>(device->pUserData);
      if (slot == nullptr || !slot->callback)
        return;
      slot->callback(static_cast<const float*>(input), static_cast<usize>(frame_count),
                     slot->channels, slot->sample_rate);
    }

    sound_slot* slot_for_id(engine_state& s, int id) {
      if (id < 0)
        return nullptr;
      const auto index = static_cast<usize>(id);
      if (index >= s.slots.size())
        return nullptr;
      return &s.slots[index];
    }

    std::optional<int> acquire_slot_locked(engine_state& s) {
      while (!s.free_list.empty()) {
        int id = s.free_list.back();
        s.free_list.pop_back();
        if (auto* slot = slot_for_id(s, id)) {
          *slot = sound_slot{};
          slot->in_use = true;
          return id;
        }
      }
      if (s.slots.size() >= static_cast<usize>(std::numeric_limits<int>::max()))
        return std::nullopt;
      try {
        s.slots.emplace_back();
      } catch (const std::bad_alloc&) {
        return std::nullopt;
      }
      s.slots.back().in_use = true;
      return static_cast<int>(s.slots.size()) - 1;
    }

    void release_slot_locked(engine_state& s, int id) {
      auto* slot = slot_for_id(s, id);
      if (slot == nullptr)
        return;
      if (slot->sound_initialised) {
        ma_sound_uninit(&slot->sound);
        slot->sound_initialised = false;
      }
      if (slot->decoder_initialised) {
        ma_decoder_uninit(&slot->decoder);
        slot->decoder_initialised = false;
      }
      slot->bytes.clear();
      slot->bytes.shrink_to_fit();
      slot->in_use = false;
      s.free_list.push_back(id);
    }

    sound_slot* slot_for(engine_state& s, sound_handle h) {
      auto* slot = slot_for_id(s, h.id);
      if (slot == nullptr)
        return nullptr;
      if (!slot->in_use)
        return nullptr;
      return slot;
    }

    std::optional<int> acquire_capture_slot_locked(engine_state& s) {
      while (!s.capture_free_list.empty()) {
        int id = s.capture_free_list.back();
        s.capture_free_list.pop_back();
        if (id >= 0 && static_cast<usize>(id) < s.capture_slots.size() &&
            s.capture_slots[static_cast<usize>(id)] == nullptr)
          return id;
      }
      if (s.capture_slots.size() >= static_cast<usize>(std::numeric_limits<int>::max()))
        return std::nullopt;
      try {
        s.capture_slots.emplace_back(nullptr);
      } catch (const std::bad_alloc&) {
        return std::nullopt;
      }
      return static_cast<int>(s.capture_slots.size()) - 1;
    }

    capture_slot* capture_slot_for_id(engine_state& s, int id) {
      if (id < 0)
        return nullptr;
      const auto index = static_cast<usize>(id);
      if (index >= s.capture_slots.size())
        return nullptr;
      return s.capture_slots[index].get();
    }

    void uninit_capture_slot(capture_slot& slot) {
      if (slot.device_initialised) {
        ma_device_uninit(&slot.device);
        slot.device_initialised = false;
      }
      if (slot.context_initialised) {
        ma_context_uninit(&slot.context);
        slot.context_initialised = false;
      }
      slot.in_use = false;
    }

  } // namespace

  audio_error last_error() {
    return g_last_error;
  }

  audio_error initialize() {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (s.initialised)
      return set_last_error(audio_error::ok);
    if (ma_engine_init(nullptr, &s.engine) != MA_SUCCESS)
      return set_last_error(audio_error::engine_init_failed);
    s.initialised = true;
    return set_last_error(audio_error::ok);
  }

  void shutdown() {
    std::vector<std::unique_ptr<capture_slot>> captures;
    auto& s = state();
    {
      std::lock_guard<std::mutex> g(s.mu);
      captures.swap(s.capture_slots);
      s.capture_free_list.clear();
      if (!s.initialised) {
        // Capture devices are independent of the playback engine.
      } else {
        for (usize i = 0; i < s.slots.size(); ++i) {
          if (s.slots[i].in_use)
            release_slot_locked(s, static_cast<int>(i));
        }
        s.slots.clear();
        s.free_list.clear();
        ma_engine_uninit(&s.engine);
        s.initialised = false;
      }
    }
    for (auto& slot : captures) {
      if (slot)
        uninit_capture_slot(*slot);
    }
  }

  sound_handle load_from_path(std::string_view path) {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialised)
      return fail_load(audio_error::not_initialized);
    auto id = acquire_slot_locked(s);
    if (!id)
      return fail_load(audio_error::out_of_slots);
    auto* slot = slot_for_id(s, *id);
    if (slot == nullptr)
      return fail_load(audio_error::out_of_slots);
    std::string p(path);
    ma_result r = ma_sound_init_from_file(&s.engine, p.c_str(), MA_SOUND_FLAG_DECODE, nullptr,
                                          nullptr, &slot->sound);
    if (r != MA_SUCCESS) {
      release_slot_locked(s, *id);
      return fail_load(map_path_error(r));
    }
    slot->sound_initialised = true;
    set_last_error(audio_error::ok);
    return sound_handle{*id};
  }

  sound_handle load_from_bytes(const u8* data, usize size) {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialised)
      return fail_load(audio_error::not_initialized);
    if (data == nullptr || size == 0)
      return fail_load(audio_error::decode_failed);
    auto id = acquire_slot_locked(s);
    if (!id)
      return fail_load(audio_error::out_of_slots);
    auto* slot = slot_for_id(s, *id);
    if (slot == nullptr)
      return fail_load(audio_error::out_of_slots);
    try {
      slot->bytes.assign(data, data + size);
    } catch (const std::bad_alloc&) {
      release_slot_locked(s, *id);
      return fail_load(audio_error::out_of_slots);
    }
    ma_result r =
        ma_decoder_init_memory(slot->bytes.data(), slot->bytes.size(), nullptr, &slot->decoder);
    if (r != MA_SUCCESS) {
      release_slot_locked(s, *id);
      return fail_load(map_resource_error(r, audio_error::decode_failed));
    }
    slot->decoder_initialised = true;
    r = ma_sound_init_from_data_source(&s.engine, &slot->decoder, MA_SOUND_FLAG_DECODE, nullptr,
                                       &slot->sound);
    if (r != MA_SUCCESS) {
      release_slot_locked(s, *id);
      return fail_load(map_resource_error(r, audio_error::decode_failed));
    }
    slot->sound_initialised = true;
    set_last_error(audio_error::ok);
    return sound_handle{*id};
  }

  audio_error play(sound_handle h, float volume, bool loop, float rate) {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialised)
      return set_last_error(audio_error::not_initialized);
    auto* slot = slot_for(s, h);
    if (!slot || !slot->sound_initialised)
      return set_last_error(audio_error::invalid_handle);
    ma_sound_set_volume(&slot->sound, volume);
    ma_sound_set_looping(&slot->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_pitch(&slot->sound, rate <= 0.0f ? 1.0f : rate);
    ma_result r = ma_sound_seek_to_pcm_frame(&slot->sound, 0);
    if (r != MA_SUCCESS)
      return set_last_error(map_resource_error(r, audio_error::invalid_handle));
    r = ma_sound_start(&slot->sound);
    if (r != MA_SUCCESS)
      return set_last_error(map_resource_error(r, audio_error::invalid_handle));
    return set_last_error(audio_error::ok);
  }

  audio_error stop(sound_handle h) {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialised)
      return set_last_error(audio_error::not_initialized);
    auto* slot = slot_for(s, h);
    if (!slot || !slot->sound_initialised)
      return set_last_error(audio_error::invalid_handle);
    ma_result r = ma_sound_stop(&slot->sound);
    if (r != MA_SUCCESS)
      return set_last_error(map_resource_error(r, audio_error::invalid_handle));
    return set_last_error(audio_error::ok);
  }

  audio_error unload(sound_handle h) {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialised)
      return set_last_error(audio_error::not_initialized);
    auto* slot = slot_for_id(s, h.id);
    if (slot == nullptr || !slot->in_use)
      return set_last_error(audio_error::invalid_handle);
    release_slot_locked(s, h.id);
    return set_last_error(audio_error::ok);
  }

  audio_error set_master_volume(float v) {
    auto& s = state();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialised)
      return set_last_error(audio_error::not_initialized);
    if (v < 0.0f)
      v = 0.0f;
    ma_result r = ma_engine_set_volume(&s.engine, v);
    if (r != MA_SUCCESS)
      return set_last_error(map_resource_error(r, audio_error::engine_init_failed));
    return set_last_error(audio_error::ok);
  }

  std::vector<device_info> enumerate_devices(device_kind kind) {
    std::vector<device_info> out;
    ma_context context{};
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
      set_last_error(audio_error::engine_init_failed);
      return out;
    }

    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 capture_count = 0;
    ma_result r = ma_context_get_devices(&context, &playback_infos, &playback_count, &capture_infos,
                                         &capture_count);
    if (r != MA_SUCCESS) {
      ma_context_uninit(&context);
      set_last_error(audio_error::io_failed);
      return out;
    }

    ma_device_info* infos = kind == device_kind::input ? capture_infos : playback_infos;
    ma_uint32 count = kind == device_kind::input ? capture_count : playback_count;
    try {
      out.reserve(count);
      for (ma_uint32 i = 0; i < count; ++i) {
        out.push_back(device_info{
            encode_device_id(infos[i].id),
            infos[i].name,
            infos[i].isDefault == MA_TRUE,
        });
      }
      set_last_error(audio_error::ok);
    } catch (const std::bad_alloc&) {
      out.clear();
      set_last_error(audio_error::out_of_slots);
    }
    ma_context_uninit(&context);
    return out;
  }

  capture_handle start_capture(captured_audio_callback cb, capture_options opts) {
    if (!cb)
      return fail_capture(audio_error::invalid_handle);

    auto slot = std::make_unique<capture_slot>();
    slot->callback = std::move(cb);
    ma_result r = ma_context_init(nullptr, 0, nullptr, &slot->context);
    if (r != MA_SUCCESS)
      return fail_capture(audio_error::engine_init_failed);
    slot->context_initialised = true;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = opts.channels.value_or(0);
    config.sampleRate = opts.sample_rate.value_or(0);
    config.dataCallback = capture_data_callback;
    config.pUserData = slot.get();

    if (opts.device_id && !opts.device_id->empty()) {
      ma_device_info* capture_infos = nullptr;
      ma_uint32 capture_count = 0;
      r = ma_context_get_devices(&slot->context, nullptr, nullptr, &capture_infos, &capture_count);
      if (r != MA_SUCCESS) {
        uninit_capture_slot(*slot);
        return fail_capture(audio_error::io_failed);
      }
      bool found = false;
      for (ma_uint32 i = 0; i < capture_count; ++i) {
        if (encode_device_id(capture_infos[i].id) == *opts.device_id) {
          slot->selected_id = capture_infos[i].id;
          config.capture.pDeviceID = &slot->selected_id;
          found = true;
          break;
        }
      }
      if (!found) {
        uninit_capture_slot(*slot);
        return fail_capture(audio_error::invalid_handle);
      }
    }

    r = ma_device_init(&slot->context, &config, &slot->device);
    if (r != MA_SUCCESS) {
      uninit_capture_slot(*slot);
      return fail_capture(audio_error::engine_init_failed);
    }
    slot->device_initialised = true;
    slot->channels = slot->device.capture.channels;
    slot->sample_rate = slot->device.sampleRate;
    slot->in_use = true;

    auto& s = state();
    int id = -1;
    {
      std::lock_guard<std::mutex> g(s.mu);
      auto acquired = acquire_capture_slot_locked(s);
      if (!acquired) {
        uninit_capture_slot(*slot);
        return fail_capture(audio_error::out_of_slots);
      }
      id = *acquired;
      s.capture_slots[static_cast<usize>(id)] = std::move(slot);
    }

    capture_slot* started_slot = nullptr;
    {
      std::lock_guard<std::mutex> g(s.mu);
      started_slot = capture_slot_for_id(s, id);
    }
    if (started_slot == nullptr)
      return fail_capture(audio_error::invalid_handle);
    r = ma_device_start(&started_slot->device);
    if (r != MA_SUCCESS) {
      (void)stop_capture(capture_handle{id});
      return fail_capture(audio_error::engine_init_failed);
    }

    set_last_error(audio_error::ok);
    return capture_handle{id};
  }

  audio_error stop_capture(capture_handle h) {
    std::unique_ptr<capture_slot> slot;
    auto& s = state();
    {
      std::lock_guard<std::mutex> g(s.mu);
      auto index = static_cast<usize>(h.id);
      if (!h.valid() || index >= s.capture_slots.size() || !s.capture_slots[index])
        return set_last_error(audio_error::invalid_handle);
      slot = std::move(s.capture_slots[index]);
      s.capture_free_list.push_back(h.id);
    }
    uninit_capture_slot(*slot);
    return set_last_error(audio_error::ok);
  }

} // namespace fxe::audio
