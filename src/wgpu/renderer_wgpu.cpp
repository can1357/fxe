#include <fxe/renderer.hpp>
#include <fxe/window.hpp>

#include <atomic>

#include <fxe/types.hpp>
#include <stdexcept>

namespace fxe {
  // Default (CPU-only) renderer used when no GPU backend is configured. Performs the same
  // command-buffer accounting as a real backend so logic-only consumers (tests, JS host
  // smoke-tests, headless CI) work unchanged. The Dawn-backed renderer in
  // renderer_dawn.cpp overrides this when FXE_HAS_WGPU is set.
  class null_renderer final : public renderer {
  public:
    explicit null_renderer(window& win) : win_(win) {}

    void begin_frame(const math::vec3& eye_pos, const math::vec3& eye_dir,
                     const math::mat4x4& world_view_proj) override {
      auto fb = win_.framebuffer_size();
      float w = fb.x ? float(fb.x) : 1.0f;
      float h = fb.y ? float(fb.y) : 1.0f;
      update_constants(eye_pos, eye_dir, world_view_proj, w, h);
      clear();
    }

    void end_frame() override {
      if (capture_armed_.load(std::memory_order_acquire)) {
        ++capture_frame_seq_;
        captured_frame_available_ = false;
      }
    }

    bool queue_dev(const command_view& src, const vshader_cbuf&, const render_config&) override {
      queue(src);
      return true;
    }

    void stage_captured_frame() override {
      captured_frame_available_ = true;
    }

    capture_result capture_frame() override {
      capture_result r;
      const bool was_armed = capture_armed_.exchange(true, std::memory_order_acq_rel);
      auto fb = win_.framebuffer_size();
      r.width = fb.x;
      r.height = fb.y;
      r.error = was_armed ? "capture not supported by null renderer"
                          : "capture armed, but capture is not supported by null renderer";
      if (!captured_frame_available_)
        win_.post_redraw();
      return r;
    }

    window& get_window() override {
      return win_;
    }
    const window& get_window() const override {
      return win_;
    }
    std::vector<u32> supported_multisample_counts() const override {
      return {1, default_multisample_count};
    }

  private:
    window& win_;
    std::atomic<bool> capture_armed_{false};
    u64 capture_frame_seq_ = 0;
    bool captured_frame_available_ = false;
  };

#if !FXE_HAS_WGPU
  std::unique_ptr<renderer> create_renderer(window& win, const renderer_options&) {
    return std::make_unique<null_renderer>(win);
  }
#endif
} // namespace fxe
