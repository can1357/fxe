#pragma once

#include <fxe/types.hpp>

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <cstdint>
#include <vector>

#include <fxe/math.hpp>
namespace fxe {
  struct clipboard_image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> data;
  };

  struct image_data {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> data;
  };

  struct drag_payload {
    std::vector<std::string> files;
    std::optional<std::string> text;
    std::optional<image_data> icon;
    std::optional<std::string> html;
    std::optional<image_data> image;
  };

  struct window_desc {
    u32 width = 1280;
    u32 height = 720;
    bool fullscreen = false;
    bool visible = true;
    std::string_view title = "fxe";
    bool resizable = true;
    bool decorated = true;
    bool transparent = false;
    bool always_on_top = false;
    bool maximized = false;
    int min_width = 0, min_height = 0, max_width = 0, max_height = 0;
    int x = INT_MIN, y = INT_MIN;
  };

  enum class cursor_kind : u8 {
    arrow,
    ibeam,
    crosshair,
    hand,
    hresize,
    vresize,
    all_resize,
    nesw_resize,
    nwse_resize,
    not_allowed,
    hidden,
  };

  enum class title_bar_style : u8 {
    default_,
    hidden,
    hidden_inset,
    custom_buttons,
  };

  enum class vibrancy_kind : u8 {
    sidebar,
    titlebar,
    menu,
  };
  struct monitor_info {
    std::string name;
    int x = 0, y = 0;
    int width = 0, height = 0;
    int work_x = 0, work_y = 0, work_width = 0, work_height = 0;
    float scale_x = 1.0f, scale_y = 1.0f;
    int refresh_hz = 0;
    bool primary = false;
  };

  std::vector<monitor_info> list_monitors();
  monitor_info primary_monitor();

  // Synthetic input event delivered by the debug server's `inject()` path.
  // Mirrors the GLFW callback set, but expressed without leaking GLFW types
  // through the public header.
  struct input_event {
    enum class kind_t : u8 {
      mouse_move,
      mouse_button_down,
      mouse_button_up,
      mouse_wheel,
      key_down,
      key_up,
      key_char,
      cursor_enter,
      cursor_leave,
      window_resize,
      window_move,
      window_focus,
      window_blur,
      window_iconify,
      window_restore,
      window_maximize,
      window_unmaximize,
      window_close,
      window_scale,
      drop_files,
      drag_enter,
      drag_over,
      drag_leave,
      message,
      compose,
    };
    kind_t kind = kind_t::mouse_move;
    double x = 0.0;
    double y = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    int button = 0; // 0=left, 1=right, 2=middle
    int key = 0;    // GLFW key code (or ASCII for letters)
    int scancode = 0;
    int modifiers = 0;
    unsigned codepoint = 0;                               // for key_char
    int width = 0, height = 0;                            // for window_resize
    int pos_x = 0, pos_y = 0;                             // for window_move
    float scale_x = 0.0f, scale_y = 0.0f;                 // for window_scale
    std::vector<std::string> paths;                       // for drop_files and drag enter/over
    std::string message_channel;                          // for message
    std::vector<std::vector<u8>> message_args_serialised; // for message
    std::string preedit;                                  // for compose
    int cursor = 0;                                       // for compose
    std::string committed;                                // for compose
  };

  class window {
  public:
    // Drain pending events without blocking. Use for tight animation loops.
    virtual void poll() = 0;
    // Block until an event arrives (or post_redraw is called from another
    // context). Wakeup also fires when the window is resized, exposed,
    // re-focused, or otherwise needs a repaint.
    virtual void wait_events() = 0;
    // Bounded variant of wait_events. seconds <= 0 acts like poll().
    virtual void wait_events_timeout(double seconds) = 0;
    // Wake the wait loop AND mark the window dirty. Safe to call from any
    // GLFW-eligible thread (in practice: the same thread that owns the
    // window, or any thread that calls glfwPostEmptyEvent).
    virtual void post_redraw() = 0;
    // Atomically test-and-clear the dirty flag.
    [[nodiscard]] virtual bool take_redraw_request() = 0;

    virtual void close() = 0;
    [[nodiscard]] virtual bool should_close() const = 0;
    [[nodiscard]] virtual math::uvec2 framebuffer_size() const = 0;
    virtual void set_vsync(bool enabled) = 0;
    [[nodiscard]] virtual void* native_handle() const = 0;

    // No-op defaults so backends (and stub_window) only need to override what
    // they actually support.
    virtual void set_title(std::string_view) {}
    [[nodiscard]] virtual std::string get_title() const {
      return title();
    }
    [[nodiscard]] virtual std::string title() const {
      return {};
    }
    virtual void set_size(int w, int h) {
      (void)w;
      (void)h;
    }
    virtual void set_position(int x, int y) {
      (void)x;
      (void)y;
    }
    [[nodiscard]] virtual math::ivec2 position() const {
      return {};
    }
    [[nodiscard]] virtual math::uvec2 content_size() const {
      return {};
    }
    [[nodiscard]] virtual math::ivec4 get_bounds() const {
      const auto pos = position();
      const auto size = content_size();
      return {pos.x, pos.y, static_cast<int>(size.x), static_cast<int>(size.y)};
    }
    virtual void set_min_size(int w, int h) {
      (void)w;
      (void)h;
    }
    virtual void set_max_size(int w, int h) {
      (void)w;
      (void)h;
    }
    [[nodiscard]] virtual std::optional<math::ivec2> min_size() const {
      return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<math::ivec2> max_size() const {
      return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<math::ivec2> get_min_size() const {
      return min_size();
    }
    [[nodiscard]] virtual std::optional<math::ivec2> get_max_size() const {
      return max_size();
    }
    virtual void set_opacity(float a) {
      (void)a;
    }
    [[nodiscard]] virtual float opacity() const {
      return 1.0f;
    }
    virtual void set_always_on_top(bool) {}
    virtual void set_resizable(bool) {}
    virtual void set_decorated(bool) {}
    [[nodiscard]] virtual bool is_resizable_actual() const {
      return false;
    }
    [[nodiscard]] virtual bool is_resizable() const {
      return is_resizable_actual();
    }
    [[nodiscard]] virtual bool is_decorated() const {
      return false;
    }
    [[nodiscard]] virtual bool is_always_on_top() const {
      return false;
    }
    virtual void set_title_bar_style(title_bar_style style) {
      (void)style;
    }
    virtual bool set_traffic_light_position(int x, int y) {
      (void)x;
      (void)y;
      return false;
    }
    virtual bool set_window_controls_overlay(bool enabled) {
      (void)enabled;
      return false;
    }
    virtual bool set_vibrancy(const char* kind) {
      (void)kind;
      return false;
    }
    virtual bool set_blur_behind(bool enabled) {
      (void)enabled;
      return false;
    }
    virtual void set_visible(bool) {}
    virtual bool set_icon(const u8* rgba, int w, int h) {
      (void)rgba;
      (void)w;
      (void)h;
      return false;
    }
    virtual void minimize() {}
    virtual void maximize() {}
    virtual void restore() {}
    virtual void focus() {}
    virtual void request_attention() {}
    virtual void center() {}
    [[nodiscard]] virtual bool is_focused() const {
      return false;
    }
    [[nodiscard]] virtual bool is_minimized() const {
      return false;
    }
    [[nodiscard]] virtual bool is_maximized() const {
      return false;
    }
    [[nodiscard]] virtual bool is_visible() const {
      return true;
    }
    virtual void set_fullscreen(bool on, int monitor_index = -1) {
      (void)on;
      (void)monitor_index;
    }
    [[nodiscard]] virtual bool is_fullscreen() const {
      return false;
    }
    virtual void set_cursor(cursor_kind) {}
    virtual void set_cursor_visible(bool) {}
    virtual void set_cursor_pos(double x, double y) {
      (void)x;
      (void)y;
    }
    [[nodiscard]] virtual math::dvec2 cursor_pos() const {
      return {};
    }
    virtual void set_cursor_lock(bool) {}
    [[nodiscard]] virtual std::string clipboard_text() const {
      return {};
    }
    virtual void set_clipboard_text(std::string_view) {}

    [[nodiscard]] virtual bool read_clipboard_image(clipboard_image& out) const {
      (void)out;
      return false;
    }
    virtual bool set_clipboard_image(const clipboard_image& image) {
      (void)image;
      return false;
    }
    [[nodiscard]] virtual std::optional<std::string> clipboard_html() const {
      return std::nullopt;
    }
    virtual bool set_clipboard_html(std::string_view) {
      return false;
    }
    [[nodiscard]] virtual std::optional<std::string> clipboard_rtf() const {
      return std::nullopt;
    }
    virtual bool set_clipboard_rtf(std::string_view) {
      return false;
    }
    [[nodiscard]] virtual std::optional<std::vector<u8>> clipboard_mime(std::string_view) const {
      return std::nullopt;
    }
    virtual bool set_clipboard_mime(std::string_view, const std::vector<u8>&) {
      return false;
    }

    [[nodiscard]] virtual bool is_transparent() const {
      return false;
    }
    virtual void set_drag_region(const std::vector<math::ivec4>& rects) {
      (void)rects;
    }

    virtual bool start_drag(const drag_payload& payload) {
      (void)payload;
      return false;
    }

    virtual void post_message(std::string channel, std::vector<std::vector<u8>> args) {
      input_event ev{};
      ev.kind = input_event::kind_t::message;
      ev.message_channel = std::move(channel);
      ev.message_args_serialised = std::move(args);
      inject(ev);
      post_redraw();
    }
    // Synthesize an input event as if it had come from the OS. Default impl
    // buffers into the internal queue.
    virtual void inject(const input_event& ev) {
      injected_events_.push_back(ev);
    }
    // Drain & return everything injected since the last call. Thread: render.
    virtual std::vector<input_event> drain_input_events() {
      std::vector<input_event> out;
      out.swap(injected_events_);
      return out;
    }
    // ---- Live-resize redraw hook -----------------------------------------
    //
    // Some platforms (notably macOS Cocoa, Windows during a resize loop)
    // enter a modal event-tracking loop while the user is actively resizing.
    // During that loop the host's `glfwWaitEvents` / `runUntilDate` call
    // does **not** return even though the OS keeps delivering refresh /
    // framebuffer-size callbacks. Without a handler installed here the
    // window stays painted with the pre-resize cache and only refreshes
    // after the user releases the mouse.
    //
    // The handler is invoked synchronously from the GLFW window-refresh and
    // framebuffer-size callbacks. The runner wires it to "drive one frame
    // end-to-end" (microtasks, RAF, paint) so live resize stays smooth.
    using redraw_handler = std::function<void()>;
    virtual void set_redraw_handler(redraw_handler /*handler*/) {}

    // Backdrop colour shown by the platform compositor for any region of the
    // surface that is not yet covered by a freshly presented frame. On macOS
    // this maps to `CAMetalLayer.backgroundColor`; on platforms that do not
    // expose an equivalent it is a no-op. Set it to your app's body
    // background to avoid a brief flash during live resize, where the layer
    // bounds grow on one CATransaction but the new frame lands on the next.
    // Components are linear-space 0..1 RGBA.
    virtual void set_surface_background_color(float r, float g, float b, float a) {
      (void)r;
      (void)g;
      (void)b;
      (void)a;
    }

    virtual ~window() = default;

  protected:
    std::vector<input_event> injected_events_;
  };
  std::unique_ptr<window> create_window(const window_desc& desc = {});

#if FXE_HAS_WGPU
} // namespace fxe

namespace wgpu {
  class Surface;
  class Instance;
} // namespace wgpu

namespace fxe {
  // Platform-aware WebGPU surface factory. Builds the appropriate
  // wgpu::SurfaceDescriptor chain (Metal layer / X11 / Wayland / HWND) for
  // `w.native_handle()` and returns the freshly created wgpu::Surface.
  // Defined alongside the window backend (src/window/glfw_window.cpp).
  wgpu::Surface make_wgpu_surface(window& w, const wgpu::Instance& instance);
#endif
} // namespace fxe
