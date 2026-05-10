// JS bindings for fxe::audio.
//
// Surface:
//   global Audio.load(path: string)              -> Promise<Sound>
//   global Audio.loadFromBytes(bytes: Uint8Array) -> Promise<Sound>
//   global Audio.setMasterVolume(v: number)
//   class  Sound { play({volume?, loop?, rate?}); stop(); dispose(); }
//
// Loading is synchronous on the calling thread (miniaudio decodes inline).
// We still return a Promise so the public API stays future-proof: a worker
// thread or async decode can be slotted in without changing TS callers.
//
// Sound objects own a sound_handle by id. The GC finaliser unloads the
// underlying ma_sound; explicit dispose() also unloads and marks the slot
// invalid so subsequent play()/stop() are no-ops.
//
// Type tag 'AUDS'.

#include "bind_audio.hpp"
#include "../audio/audio.hpp"
#include "../os/os.hpp"
#include "bind_timers.hpp"
#include "weak_holder.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <v8.h>
#include <vector>

namespace fxe::js {
  namespace {
    using namespace v8;

    inline constexpr unsigned int TAG_AUDIO_CAPTURE = 0x41554443u; // 'AUDC'

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& sound_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& capture_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void sound_reset_for_isolate(Isolate* iso) {
      auto& t = sound_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
      auto& c = capture_tpl_table();
      auto cit = c.find(iso);
      if (cit != c.end()) {
        cit->second.Reset();
        c.erase(cit);
      }
    }
    struct sound_resetter_register {
      sound_resetter_register() {
        register_template_resetter(&sound_reset_for_isolate);
      }
    };
    static sound_resetter_register s_sound_resetter_register;

    struct sound_holder : weak_holder<sound_holder> {
      audio::sound_handle handle;
      bool disposed = false;

      void on_finalize(v8::Isolate*) {
        if (!disposed && handle.valid())
          (void)audio::unload(handle);
      }
    };

    struct capture_chunk {
      std::vector<float> samples;
      usize frame_count = 0;
      u32 channels = 0;
      u32 sample_rate = 0;
    };

    struct capture_holder {
      Isolate* isolate = nullptr;
      audio::capture_handle handle;
      std::atomic<bool> stopped{false};
      std::atomic<bool> drain_scheduled{false};
      std::mutex mu;
      std::deque<capture_chunk> chunks;
      Global<Function> callback;
      Global<Context> context;
      Global<Object>* persistent = nullptr;
    };

    sound_holder* unwrap_sound(Local<Object> self) {
      return static_cast<sound_holder*>(unwrap(self, TAG_AUDIO_SOUND));
    }

    Local<Object> make_sound_object(Isolate* iso, Local<Context> ctx, audio::sound_handle handle) {
      EscapableHandleScope hs(iso);
      auto tpl = sound_tpl_table()[iso].Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      auto obj = fn->NewInstance(ctx).ToLocalChecked();
      auto* holder = new sound_holder{{}, handle, false};
      set_native(iso, obj, holder, TAG_AUDIO_SOUND);
      holder->bind(iso, obj);
      return hs.Escape(obj);
    }

    void schedule_capture_drain(const std::shared_ptr<capture_holder>& holder);

    void stop_capture_holder(const std::shared_ptr<capture_holder>& holder) {
      if (!holder)
        return;
      bool expected = false;
      if (holder->stopped.compare_exchange_strong(expected, true) && holder->handle.valid()) {
        (void)audio::stop_capture(holder->handle);
        holder->handle = {};
      }
      holder->callback.Reset();
      holder->context.Reset();
      {
        std::lock_guard<std::mutex> lock(holder->mu);
        holder->chunks.clear();
      }
    }

    void capture_finalizer(const WeakCallbackInfo<std::shared_ptr<capture_holder>>& info) {
      auto* ref = info.GetParameter();
      if (ref) {
        if (*ref && (*ref)->persistent) {
          (*ref)->persistent->Reset();
          delete (*ref)->persistent;
          (*ref)->persistent = nullptr;
        }
        stop_capture_holder(*ref);
        delete ref;
      }
    }

    std::shared_ptr<capture_holder>* unwrap_capture_ref(Local<Object> self) {
      return static_cast<std::shared_ptr<capture_holder>*>(unwrap(self, TAG_AUDIO_CAPTURE));
    }

    std::shared_ptr<capture_holder> unwrap_capture(Local<Object> self) {
      auto* ref = unwrap_capture_ref(self);
      return ref ? *ref : std::shared_ptr<capture_holder>{};
    }

    Local<Float32Array> make_float32_array(Isolate* iso, capture_chunk& chunk) {
      const usize byte_len = chunk.samples.size() * sizeof(float);
      if (byte_len == 0) {
        auto store = ArrayBuffer::NewBackingStore(iso, 0);
        auto ab = ArrayBuffer::New(iso, std::move(store));
        return Float32Array::New(ab, 0, 0);
      }
      auto* samples = new std::vector<float>(std::move(chunk.samples));
      auto store = ArrayBuffer::NewBackingStore(
          samples->data(), byte_len,
          [](void*, usize, void* data) { delete static_cast<std::vector<float>*>(data); }, samples);
      auto ab = ArrayBuffer::New(iso, std::move(store));
      return Float32Array::New(ab, 0, samples->size());
    }

    void drain_capture(const std::weak_ptr<capture_holder>& weak) {
      auto holder = weak.lock();
      if (!holder || holder->stopped.load() || holder->callback.IsEmpty() ||
          holder->context.IsEmpty())
        return;

      auto* iso = holder->isolate;
      Isolate::Scope isolate_scope(iso);
      HandleScope hs(iso);
      auto ctx = holder->context.Get(iso);
      Context::Scope context_scope(ctx);

      for (;;) {
        std::deque<capture_chunk> chunks;
        {
          std::lock_guard<std::mutex> lock(holder->mu);
          chunks.swap(holder->chunks);
        }
        if (chunks.empty()) {
          holder->drain_scheduled.store(false);
          std::lock_guard<std::mutex> lock(holder->mu);
          if (!holder->chunks.empty() && !holder->drain_scheduled.exchange(true)) {
            fxe::os::post_main_thread_dispatch([weak] { drain_capture(weak); });
            wake_event_loop();
          }
          return;
        }

        auto cb = holder->callback.Get(iso);
        for (auto& chunk : chunks) {
          if (holder->stopped.load()) {
            holder->drain_scheduled.store(false);
            return;
          }
          auto view = make_float32_array(iso, chunk);
          auto info = Object::New(iso);
          set_prop(ctx, info, "frameCount", static_cast<double>(chunk.frame_count));
          set_prop(ctx, info, "channels", chunk.channels);
          set_prop(ctx, info, "sampleRate", chunk.sample_rate);
          Local<Value> argv[] = {view, info};
          TryCatch try_catch(iso);
          (void)cb->Call(ctx, ctx->Global(), 2, argv);
          if (try_catch.HasCaught()) {
            holder->drain_scheduled.store(false);
            return;
          }
        }
      }
    }

    void schedule_capture_drain(const std::shared_ptr<capture_holder>& holder) {
      if (!holder || holder->stopped.load())
        return;
      if (!holder->drain_scheduled.exchange(true)) {
        std::weak_ptr<capture_holder> weak(holder);
        fxe::os::post_main_thread_dispatch([weak] { drain_capture(weak); });
        wake_event_loop();
      }
    }

    void enqueue_capture_samples(const std::shared_ptr<capture_holder>& holder,
                                 const float* samples, usize frame_count, u32 channels,
                                 u32 sample_rate) {
      if (!holder || holder->stopped.load() || samples == nullptr || frame_count == 0 ||
          channels == 0)
        return;
      if (frame_count > std::numeric_limits<usize>::max() / channels)
        return;
      const usize sample_count = frame_count * channels;
      capture_chunk chunk;
      try {
        chunk.samples.assign(samples, samples + sample_count);
      } catch (const std::bad_alloc&) {
        return;
      }
      chunk.frame_count = frame_count;
      chunk.channels = channels;
      chunk.sample_rate = sample_rate;
      {
        std::lock_guard<std::mutex> lock(holder->mu);
        holder->chunks.push_back(std::move(chunk));
      }
      schedule_capture_drain(holder);
    }

    Local<Object> make_capture_object(Isolate* iso, Local<Context> ctx,
                                      std::shared_ptr<capture_holder> holder) {
      EscapableHandleScope hs(iso);
      auto tpl = capture_tpl_table()[iso].Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      auto obj = fn->NewInstance(ctx).ToLocalChecked();
      auto* ref = new std::shared_ptr<capture_holder>(std::move(holder));
      set_native(iso, obj, ref, TAG_AUDIO_CAPTURE);
      auto* persistent = new Global<Object>(iso, obj);
      (*ref)->persistent = persistent;
      persistent->SetWeak(ref, capture_finalizer, WeakCallbackType::kParameter);
      return hs.Escape(obj);
    }

    void sound_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      // Internal-only constructor. JS callers must use Audio.load(...).
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Sound is not user-constructible; use Audio.load");
        return;
      }
      // Allow construction: make_sound_object calls NewInstance which lands
      // here. Internal fields are populated by the caller after the fact.
    }

    void capture_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso,
                               "CaptureSession is not user-constructible; use Audio.startCapture");
        return;
      }
    }

    bool read_number(Local<Context> ctx, Local<Object> opts, Local<String> key, double& out) {
      if (auto v = get_prop<Local<Value>>(ctx, opts, key)) {
        out = (*v)->NumberValue(ctx).FromMaybe(out);
        return true;
      }
      return false;
    }

    bool read_bool(Isolate* iso, Local<Context> ctx, Local<Object> opts, Local<String> key,
                   bool& out) {
      if (auto v = get_prop<Local<Value>>(ctx, opts, key)) {
        out = (*v)->BooleanValue(iso);
        return true;
      }
      return false;
    }

    bool read_string(Isolate* iso, Local<Context> ctx, Local<Object> opts, Local<String> key,
                     std::string& out) {
      if (auto v = get_prop<Local<String>>(ctx, opts, key)) {
        out = to_std_string(iso, *v);
        return true;
      }
      return false;
    }

    bool read_positive_u32(Local<Context> ctx, Local<Object> opts, Local<String> key,
                           std::optional<u32>& out) {
      double value = 0.0;
      if (!read_number(ctx, opts, key, value))
        return true;
      if (!std::isfinite(value) || value <= 0.0 ||
          value > static_cast<double>(std::numeric_limits<u32>::max()))
        return false;
      out = static_cast<u32>(value);
      return true;
    }

    bool parse_capture_options(Isolate* iso, Local<Context> ctx, Local<Value> value,
                               audio::capture_options& opts) {
      if (value->IsUndefined() || value->IsNull())
        return true;
      if (!value->IsObject())
        return false;
      auto obj = value.As<Object>();
      if (!read_positive_u32(ctx, obj, "sampleRate"_v8(iso), opts.sample_rate))
        return false;
      if (!read_positive_u32(ctx, obj, "channels"_v8(iso), opts.channels))
        return false;
      if (auto device_value = get_prop<Local<Value>>(ctx, obj, "deviceId")) {
        if (!(*device_value)->IsString())
          return false;
        std::string device_id;
        if (!read_string(iso, ctx, obj, "deviceId"_v8(iso), device_id))
          return false;
        opts.device_id = std::move(device_id);
      }
      return true;
    }

    const char* audio_code(audio::audio_error error) {
      switch (error) {
      case audio::audio_error::decode_failed:
        return "EAUDIO_DECODE";
      case audio::audio_error::io_failed:
        return "EAUDIO_IO";
      case audio::audio_error::engine_init_failed:
      case audio::audio_error::not_initialized:
        return "EAUDIO_INIT";
      case audio::audio_error::invalid_handle:
        return "EAUDIO_INVALID_HANDLE";
      case audio::audio_error::out_of_slots:
        return "EAUDIO_OUT_OF_SLOTS";
      case audio::audio_error::ok:
        return "EAUDIO_DECODE";
      }
      return "EAUDIO_DECODE";
    }

    const char* audio_reason(audio::audio_error error) {
      switch (error) {
      case audio::audio_error::decode_failed:
        return "failed to decode sound";
      case audio::audio_error::io_failed:
        return "failed to read sound file";
      case audio::audio_error::engine_init_failed:
      case audio::audio_error::not_initialized:
        return "audio engine failed to initialize";
      case audio::audio_error::invalid_handle:
        return "invalid sound handle";
      case audio::audio_error::out_of_slots:
        return "out of sound slots";
      case audio::audio_error::ok:
        return "unknown audio error";
      }
      return "unknown audio error";
    }

    Local<Value> make_audio_error(Isolate* iso, Local<Context> ctx, const char* reason,
                                  const char* code, bool type_error = false) {
      std::string message = "audio: ";
      message += reason;
      auto err = (type_error ? Exception::TypeError(to_v8(iso, message))
                             : Exception::Error(to_v8(iso, message)))
                     .As<Object>();
      set_prop(ctx, err, "code", code);
      return err;
    }

    Local<Value> make_audio_error(Isolate* iso, Local<Context> ctx, audio::audio_error error) {
      return make_audio_error(iso, ctx, audio_reason(error), audio_code(error));
    }

    void throw_audio_error(Isolate* iso, Local<Context> ctx, audio::audio_error error) {
      iso->ThrowException(make_audio_error(iso, ctx, error));
    }

    void capture_session_stop(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto holder = unwrap_capture(info.This());
      if (!holder) {
        throw_audio_error(iso, ctx, audio::audio_error::invalid_handle);
        return;
      }
      stop_capture_holder(holder);
    }

    void sound_play(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* holder = unwrap_sound(info.This());
      if (!holder || holder->disposed || !holder->handle.valid()) {
        throw_audio_error(iso, ctx, audio::audio_error::invalid_handle);
        return;
      }
      double volume = 1.0, rate = 1.0;
      bool loop = false;
      if (info.Length() >= 1 && info[0]->IsObject()) {
        auto opts = info[0].As<Object>();
        read_number(ctx, opts, "volume"_v8(iso), volume);
        read_number(ctx, opts, "rate"_v8(iso), rate);
        read_bool(iso, ctx, opts, "loop"_v8(iso), loop);
      }
      auto err =
          audio::play(holder->handle, static_cast<float>(volume), loop, static_cast<float>(rate));
      if (err != audio::audio_error::ok)
        throw_audio_error(iso, ctx, err);
    }

    void sound_stop(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* holder = unwrap_sound(info.This());
      if (!holder || holder->disposed || !holder->handle.valid()) {
        throw_audio_error(iso, ctx, audio::audio_error::invalid_handle);
        return;
      }
      auto err = audio::stop(holder->handle);
      if (err != audio::audio_error::ok)
        throw_audio_error(iso, ctx, err);
    }

    void sound_dispose(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* holder = unwrap_sound(info.This());
      if (!holder || holder->disposed)
        return;
      if (holder->handle.valid())
        (void)audio::unload(holder->handle);
      holder->disposed = true;
      holder->handle = {};
    }

    Local<Promise> make_resolved([[maybe_unused]] Isolate* iso, Local<Context> ctx,
                                 Local<Value> v) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Resolve(ctx, v);
      return resolver->GetPromise();
    }

    Local<Promise> make_rejected([[maybe_unused]] Isolate* iso, Local<Context> ctx,
                                 Local<Value> err) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Reject(ctx, err);
      return resolver->GetPromise();
    }

    void audio_load(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(make_rejected(
            iso, ctx, make_audio_error(iso, ctx, "expected string path", "EAUDIO_IO", true)));
        return;
      }
      auto path = from_v8<std::string>(ctx, info[0]);
      if (!path) {
        info.GetReturnValue().Set(
            make_rejected(iso, ctx, make_audio_error(iso, ctx, "invalid path", "EAUDIO_IO", true)));
        return;
      }
      auto init_err = audio::initialize();
      if (init_err != audio::audio_error::ok) {
        info.GetReturnValue().Set(make_rejected(iso, ctx, make_audio_error(iso, ctx, init_err)));
        return;
      }
      auto handle = audio::load_from_path(*path);
      if (!handle.valid()) {
        info.GetReturnValue().Set(
            make_rejected(iso, ctx, make_audio_error(iso, ctx, audio::last_error())));
        return;
      }
      auto obj = make_sound_object(iso, ctx, handle);
      info.GetReturnValue().Set(make_resolved(iso, ctx, obj));
    }

    void audio_load_from_bytes(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsUint8Array()) {
        info.GetReturnValue().Set(make_rejected(
            iso, ctx, make_audio_error(iso, ctx, "expected Uint8Array", "EAUDIO_DECODE", true)));
        return;
      }
      auto init_err = audio::initialize();
      if (init_err != audio::audio_error::ok) {
        info.GetReturnValue().Set(make_rejected(iso, ctx, make_audio_error(iso, ctx, init_err)));
        return;
      }
      auto u8_arr = info[0].As<Uint8Array>();
      auto buf = u8_arr->Buffer();
      auto bs = buf->GetBackingStore();
      const u8* base = static_cast<const u8*>(bs->Data());
      const u8* data = base + u8_arr->ByteOffset();
      usize size = u8_arr->ByteLength();
      auto handle = audio::load_from_bytes(data, size);
      if (!handle.valid()) {
        info.GetReturnValue().Set(
            make_rejected(iso, ctx, make_audio_error(iso, ctx, audio::last_error())));
        return;
      }
      auto obj = make_sound_object(iso, ctx, handle);
      info.GetReturnValue().Set(make_resolved(iso, ctx, obj));
    }

    void audio_set_master_volume(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto init_err = audio::initialize();
      if (init_err != audio::audio_error::ok) {
        throw_audio_error(iso, ctx, init_err);
        return;
      }
      double v = 1.0;
      if (info.Length() >= 1)
        v = info[0]->NumberValue(ctx).FromMaybe(1.0);
      auto err = audio::set_master_volume(static_cast<float>(v));
      if (err != audio::audio_error::ok)
        throw_audio_error(iso, ctx, err);
    }

    void audio_enumerate_devices(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(make_audio_error(iso, ctx, "expected 'input' or 'output'",
                                             "EAUDIO_INVALID_HANDLE", true));
        return;
      }
      auto kind_str = info[0].As<String>();
      audio::device_kind kind_value;
      if (kind_str == "input"_v8) {
        kind_value = audio::device_kind::input;
      } else if (kind_str == "output"_v8) {
        kind_value = audio::device_kind::output;
      } else {
        iso->ThrowException(make_audio_error(iso, ctx, "expected 'input' or 'output'",
                                             "EAUDIO_INVALID_HANDLE", true));
        return;
      }

      auto devices = audio::enumerate_devices(kind_value);
      if (devices.empty() && audio::last_error() != audio::audio_error::ok) {
        throw_audio_error(iso, ctx, audio::last_error());
        return;
      }
      auto arr = Array::New(iso, static_cast<int>(devices.size()));
      for (usize i = 0; i < devices.size(); ++i) {
        auto obj = Object::New(iso);
        set_prop(ctx, obj, "id", devices[i].id);
        set_prop(ctx, obj, "name", devices[i].name);
        set_prop(ctx, obj, "isDefault", devices[i].is_default);
        set_index(ctx, arr, static_cast<u32>(i), obj);
      }
      info.GetReturnValue().Set(arr);
    }

    void audio_start_capture(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      audio::capture_options opts;
      Local<Value> cb_value;
      if (info.Length() >= 1 && info[0]->IsFunction()) {
        cb_value = info[0];
      } else {
        if (info.Length() < 2 || !info[1]->IsFunction()) {
          iso->ThrowException(make_audio_error(iso, ctx, "expected options and callback",
                                               "EAUDIO_INVALID_HANDLE", true));
          return;
        }
        if (!parse_capture_options(iso, ctx, info[0], opts)) {
          iso->ThrowException(
              make_audio_error(iso, ctx, "invalid capture options", "EAUDIO_INVALID_HANDLE", true));
          return;
        }
        cb_value = info[1];
      }

      auto holder = std::make_shared<capture_holder>();
      holder->isolate = iso;
      holder->callback.Reset(iso, cb_value.As<Function>());
      holder->context.Reset(iso, ctx);
      std::weak_ptr<capture_holder> weak(holder);
      auto handle = audio::start_capture(
          [weak](const float* samples, usize frame_count, u32 channels, u32 sample_rate) {
            if (auto holder = weak.lock())
              enqueue_capture_samples(holder, samples, frame_count, channels, sample_rate);
          },
          std::move(opts));
      if (!handle.valid()) {
        holder->callback.Reset();
        holder->context.Reset();
        throw_audio_error(iso, ctx, audio::last_error());
        return;
      }
      holder->handle = handle;
      info.GetReturnValue().Set(make_capture_object(iso, ctx, std::move(holder)));
    }

  } // namespace

  void install_audio_bindings(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);

    // Sound class.
    auto stpl = FunctionTemplate::New(iso, sound_constructor);
    stpl->SetClassName("Sound"_v8(iso));
    stpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto sproto = stpl->PrototypeTemplate();
    sproto->Set(iso, "play", FunctionTemplate::New(iso, sound_play));
    sproto->Set(iso, "stop", FunctionTemplate::New(iso, sound_stop));
    sproto->Set(iso, "dispose", FunctionTemplate::New(iso, sound_dispose));
    global->Set(iso, "Sound", stpl);
    sound_tpl_table()[iso].Reset(iso, stpl);

    // CaptureSession class.
    auto ctpl = FunctionTemplate::New(iso, capture_constructor);
    ctpl->SetClassName("CaptureSession"_v8(iso));
    ctpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto cproto = ctpl->PrototypeTemplate();
    cproto->Set(iso, "stop", FunctionTemplate::New(iso, capture_session_stop));
    global->Set(iso, "CaptureSession", ctpl);
    capture_tpl_table()[iso].Reset(iso, ctpl);

    // Audio namespace.
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "load", FunctionTemplate::New(iso, audio_load));
    ns->Set(iso, "loadFromBytes", FunctionTemplate::New(iso, audio_load_from_bytes));
    ns->Set(iso, "setMasterVolume", FunctionTemplate::New(iso, audio_set_master_volume));
    ns->Set(iso, "enumerateDevices", FunctionTemplate::New(iso, audio_enumerate_devices));
    ns->Set(iso, "startCapture", FunctionTemplate::New(iso, audio_start_capture));
    global->Set(iso, "Audio", ns);
  }

} // namespace fxe::js
