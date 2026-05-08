#pragma once

// Engine-level audio façade over miniaudio. A single global ma_engine drives
// playback; sound_handle is a stable id into an internal slot table so the
// engine retains ownership and JS bindings can hand opaque ids back.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <fxe/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::audio {

  struct sound_handle {
    int id = -1;
    bool valid() const {
      return id >= 0;
    }
  };

  struct capture_handle {
    int id = -1;
    bool valid() const {
      return id >= 0;
    }
  };

  enum class device_kind {
    input,
    output,
  };

  struct device_info {
    std::string id;
    std::string name;
    bool is_default = false;
  };

  struct capture_options {
    std::optional<u32> sample_rate;
    std::optional<u32> channels;
    std::optional<std::string> device_id;
  };

  using captured_audio_callback =
      std::function<void(const float* samples, usize frame_count, u32 channels, u32 sample_rate)>;

  enum class audio_error {
    ok,
    not_initialized,
    invalid_handle,
    decode_failed,
    engine_init_failed,
    out_of_slots,
    io_failed,
  };

  audio_error last_error();

  // Initialise the global engine. Idempotent. Safe to call before any window
  // exists; miniaudio uses the platform default device.
  [[nodiscard]] audio_error initialize();

  // Tear the global engine down and release every loaded sound.
  void shutdown();

  // Decode and load a sound from a filesystem path. Returns an invalid handle
  // on failure; call last_error() for the structured failure reason.
  sound_handle load_from_path(std::string_view path);

  // Decode and load a sound from an in-memory buffer. The buffer is copied
  // internally; callers may free `data` immediately after the call. Returns an
  // invalid handle on failure; call last_error() for the structured reason.
  sound_handle load_from_bytes(const u8* data, usize size);

  // Start playback. `volume` is linear gain, `rate` is a pitch/speed
  // multiplier (1.0 = native rate). Calling play on an already-playing sound
  // restarts it from the beginning.
  [[nodiscard]] audio_error play(sound_handle h, float volume = 1.0f, bool loop = false,
                                 float rate = 1.0f);

  // Stop a playing sound.
  [[nodiscard]] audio_error stop(sound_handle h);

  // Free the underlying ma_sound and recycle the slot. After unload, the
  // handle is invalid and must not be reused.
  [[nodiscard]] audio_error unload(sound_handle h);

  // Master gain applied to the engine output bus. Linear, clamped to >= 0.
  [[nodiscard]] audio_error set_master_volume(float v);

  // Enumerate capture/playback devices. Device ids are opaque strings suitable
  // for capture_options::device_id.
  std::vector<device_info> enumerate_devices(device_kind kind);

  // Start/stop float32 capture from the selected or default input device.
  capture_handle start_capture(captured_audio_callback cb, capture_options opts = {});
  [[nodiscard]] audio_error stop_capture(capture_handle h);

} // namespace fxe::audio
