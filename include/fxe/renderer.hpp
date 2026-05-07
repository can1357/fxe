#pragma once

#include <fxe/types.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <fxe/command_buffer.hpp>

namespace fxe {
  enum class blend_mode : u8 { none = 0, alpha, premultiplied, additive, multiply };

  class window;

  struct renderer_options {
    u32 multisample_count = 4;
    bool enable_bloom = true;
    bool vsync = true;
  };

  // Backend-agnostic renderer interface. Owns the GPU-side mirror of the command_buffer
  // and the per-frame uniform block. The viewport / projection helpers preserve the
  // semantics of the original GFW renderer so primitives written against vec4{x,y,z,0}
  // (screen-space) and vec4{x,y,z,1} (world-space) keep working unchanged.
  class renderer : public command_buffer {
  public:
    static constexpr math::vec2 s2v_offset{-1.0f, +1.0f};
    static constexpr u32 default_multisample_count = 4;

    explicit renderer() = default;

    // Per-frame entry points. Backends MUST call update_constants() inside begin_frame
    // before any primitives can be queued.
    virtual void begin_frame(const math::vec3& eye_pos = {}, const math::vec3& eye_dir = {},
                             const math::mat4x4& world_view_proj = math::identity()) = 0;
    virtual void end_frame() = 0;
    virtual bool queue_dev(const command_buffer& src, const vshader_cbuf& cbuf,
                           const render_config& cfg) = 0;
    virtual void stage_captured_frame() {}

    // Push (or replace) the font/sprite atlas the backend samples for textured
    // primitives. Pixel layout is RGBA8 in row-major order. Atlases with .a
    // populated and RGB=255 produce anti-aliased glyph coverage when modulated
    // by per-vertex tint. Backends that don't support atlas sampling MAY treat
    // this as a no-op (the null backend does).
    virtual void set_atlas(u32 width, u32 height, const u8* rgba_pixels) {
      (void)width;
      (void)height;
      (void)rgba_pixels;
    }

    // Capture the previously-presented frame as RGBA8 pixels.
    // Synchronous: blocks the render thread until the GPU readback completes.
    // `rgba` is replaced with width*height*4 bytes (row-major, tightly packed
    // — row stride = width*4); width/height with framebuffer dimensions.
    // Returns ok=false (with `error` populated) if the backend cannot honour
    // the request — e.g. the null backend, or before the first frame.
    // Encoding to PNG/JPEG is the caller's responsibility.
    struct capture_result {
      bool ok = false;
      u32 width = 0;
      u32 height = 0;
      std::vector<u8> rgba;
      std::string error;
    };
    virtual capture_result capture_frame() {
      capture_result r;
      r.error = "capture not supported by this backend";
      return r;
    }
    [[nodiscard]] virtual window& get_window() = 0;
    [[nodiscard]] virtual const window& get_window() const = 0;

    // Graphics options.
    virtual bool check_multisample_count(u32 count) const noexcept {
      return count == 1 || count == default_multisample_count;
    }
    bool set_multisample_count(u32 count) noexcept {
      if (multisample_count_ == count)
        return true;
      if (!check_multisample_count(count))
        return false;
      multisample_count_ = count;
      recreate_buffers_ = true;
      return true;
    }
    [[nodiscard]] u32 multisample_count() const noexcept {
      return multisample_count_;
    }
    void set_bloom_enabled(bool on) noexcept {
      bloom_enabled_ = on;
    }
    [[nodiscard]] bool bloom_enabled() const noexcept {
      return bloom_enabled_;
    }

    void set_blend_mode(blend_mode mode) noexcept {
      if (blend_mode_ == mode)
        return;
      blend_mode_ = mode;
      recreate_buffers_ = true;
    }
    [[nodiscard]] blend_mode current_blend_mode() const noexcept {
      return blend_mode_;
    }

    void set_clear_color(const math::vec4& c) noexcept {
      clear_color_ = c;
      clear_color_set_ = true;
    }
    [[nodiscard]] math::vec4 clear_color() const noexcept {
      return clear_color_;
    }
    [[nodiscard]] bool clear_color_user_set() const noexcept {
      return clear_color_set_;
    }

    // Geometry helpers — preserve original GFW semantics.
    [[nodiscard]] math::vec2 get_screen() const noexcept {
      return viewport_.size;
    }
    [[nodiscard]] const viewport_desc& viewport() const noexcept {
      return viewport_;
    }
    [[nodiscard]] const vshader_cbuf& constants() const noexcept {
      return cbuf_;
    }

    // Screen-pixel -> NDC. Mirrors `to_viewport` in the original gfw::renderer.
    [[nodiscard]] math::vec2 to_viewport(math::vec2 px) const noexcept {
      return px * s2v_scale_ + s2v_offset;
    }
    // NDC -> Screen-pixel. Mirrors `to_screen` in the original.
    [[nodiscard]] math::vec2 to_screen(math::vec2 ndc) const noexcept {
      return (ndc - s2v_offset) * v2s_scale_;
    }
    // Project a world-space point into clip space (post-projection NDC, w preserved).
    [[nodiscard]] math::vec4 world_to_viewport(math::vec3 p) const noexcept {
      math::vec4 r = cbuf_.world_view_proj * math::vec4{p.x, p.y, p.z, 1.0f};
      float w = std::abs(r.w == 0.0f ? 1.0f : r.w);
      return r / w;
    }
    [[nodiscard]] math::vec4 world_to_screen(math::vec3 p) const noexcept {
      math::vec4 v = world_to_viewport(p);
      auto s = to_screen({v.x, v.y});
      return {s.x, s.y, v.z, v.w};
    }

    [[nodiscard]] const math::mat4x4& transformation_matrix() const noexcept {
      return cbuf_.world_view_proj;
    }

    // Build the screen-pixel -> NDC matrix used by VS_Transform when is_world == 0.
    static constexpr math::mat4x4 calc_vp_matrix(const viewport_desc& vp) noexcept {
      math::vec2 p2v_scale{1.0f / (vp.size.x * 0.5f), 1.0f / (vp.size.y * -0.5f)};
      math::vec2 p2v_offset = s2v_offset - p2v_scale * vp.at;
      math::mat4x4 m(0.0f);
      m[0] = math::vec4{p2v_scale.x, 0, 0, 0};
      m[1] = math::vec4{0, p2v_scale.y, 0, 0};
      m[2] = math::vec4{0, 0, 1, 0};
      m[3] = math::vec4{p2v_offset.x, p2v_offset.y, 0, 1};
      return m;
    }

    virtual ~renderer() = default;

  protected:
    // Recompute viewport / projection state from a target framebuffer extent. Called by
    // every backend at begin_frame.
    void update_constants(const math::vec3& eye_pos, const math::vec3& eye_dir,
                          const math::mat4x4& world_view_proj, float width, float height) noexcept {
      v2s_scale_ = {width * +0.5f, height * -0.5f};
      s2v_scale_ = {1.0f / v2s_scale_.x, 1.0f / v2s_scale_.y};
      viewport_ = {math::vec2{0, 0}, math::vec2{width, height}, math::vec2{0.0f, 1.0f}};
      cbuf_.world_view_proj = world_view_proj;
      cbuf_.screen_ndc = calc_vp_matrix(viewport_);
      cbuf_.eye_pos = math::vec4{eye_pos.x, eye_pos.y, eye_pos.z, 0.0f};
      cbuf_.eye_dir = math::vec4{eye_dir.x, eye_dir.y, eye_dir.z, 0.0f};
      cbuf_.tint = math::vec4{1, 1, 1, 1};
      cbuf_.capture_offset = {0.5f, 0.5f};
      cbuf_.capture_scale = {0.5f, -0.5f};
      using clock = std::chrono::steady_clock;
      auto now = clock::now();
      if (init_time_.time_since_epoch().count() == 0)
        init_time_ = now;
      cbuf_.time = std::chrono::duration<float, std::milli>(now - init_time_).count();
    }

    bool recreate_buffers_ = true;
    u32 multisample_count_ = default_multisample_count;
    blend_mode blend_mode_ = blend_mode::alpha;
    bool bloom_enabled_ = true;
    math::vec4 clear_color_{0.04f, 0.05f, 0.07f, 1.0f};
    bool clear_color_set_ = false;
    viewport_desc viewport_{};
    math::vec2 v2s_scale_{};
    math::vec2 s2v_scale_{};
    vshader_cbuf cbuf_{};
    std::chrono::steady_clock::time_point init_time_{};
  };

  std::unique_ptr<renderer> create_renderer(window& win, const renderer_options& options = {});
} // namespace fxe
