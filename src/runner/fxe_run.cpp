// fxe_run — minimal CLI that drives the embedded V8 host to execute JS files.
//
// Usage:
//   fxe_run [--debug[=PORT]] [--debug-host=ADDR] [--debug-pause]
//           [--debug-keepalive] [--watch] [--vsync|--no-vsync] [--fps-limit=N]
//           [--msaa=N] [--bloom|--no-bloom] [--render-surface=window|offscreen]
//           [--show-fps] [--screenshot[=PATH]] [--screenshot-delay=MS]
//           [--screenshot-frames=N] [--screenshot-size=WxH]
//           [--screenshot-format=png|jpeg] [--screenshot-quality=N]
//           [--screenshot-stay] <script.js> [more.js ...]
//
// --debug              Start the fxe debug protocol server (NDJSON over TCP)
//                      on PORT (default 9333; use 0 for an OS-assigned port).
// --debug-host=ADDR    Bind address (default 127.0.0.1).
// --debug-pause        Pause before the first instruction; the debugger must
//                      send `Debugger.resume` to start.
// --debug-keepalive    After the script ends, keep the debug server up so a
//                      client can keep poking the runtime.
//
//
// --vsync / --no-vsync  Override Renderer(..., { vsync }) for every renderer
//                       created by the script.
// --fps-limit=N         Override Window.run/App.run fps. N <= 0 leaves the
//                       loop event-driven unless --no-lazy is set.
// --no-lazy             Disable lazy frames: drive a redraw every loop
//                       iteration so animated overlays keep ticking even
//                       when no input arrives. Alias: --continuous.
// --msaa=N              Override Renderer multisampleCount (0/1 disable MSAA).
// --bloom / --no-bloom  Override Renderer enableBloom.
// --render-surface=MODE Select renderer backing for `new Renderer(win)`.
//                       `window` uses the native swapchain; `offscreen`
//                       uses OffscreenRenderer and keeps Window invisible.
// --show-fps            Draw a small top-left FPS counter before each
//                       Renderer.endFrame().
// --screenshot[=PATH]  Render the script, capture the framebuffer once a
//                      frame has been presented, write it to PATH (default
//                      screenshot.png), then close the window so the script
//                      exits. Mirrors `chromium --screenshot=` semantics.
// --screenshot-delay=MS  Wait MS ms before capturing (script must keep
//                        ticking; the watchdog runs on the render thread).
// --screenshot-frames=N  Wait N additional frames after the delay so the
//                        readback has data (default 1).
// --screenshot-size=WxH  Clamp output to a WxH bounding box, preserving
//                        aspect ratio. Format inferred from the file
//                        extension unless --screenshot-format overrides it.
// --screenshot-format=F  Force png or jpeg.
// --screenshot-quality=N JPEG quality 1-100 (default 90).
// --screenshot-stay     Don't auto-close the window after capture.
//
// Environment variables (applied before CLI flags; CLI takes precedence):
//   FXE_DEBUG=PORT        Enable debug server on PORT (default 9333).
//   FXE_DEBUG_HOST=ADDR   Bind address (default 127.0.0.1).
//   FXE_DEBUG_PAUSE=1     Pause before the first instruction.
//   FXE_DEBUG_KEEPALIVE=1 Keep debug server alive after script exit.
//   FXE_VSYNC=1|0         Override Renderer vsync (1=on, 0=off).
//   FXE_FPS_LIMIT=N       Override fps limit.
//   FXE_MSAA=N            Override multisampleCount.
//   FXE_BLOOM=1|0         Override enableBloom.
//   FXE_RENDER_SURFACE=window|offscreen  Override renderer backing.
//   FXE_SHOW_FPS=1        Enable FPS counter overlay.
//   FXE_NO_LAZY=1         Disable lazy frames (continuous redraw).
//   FXE_WATCH=1           Enable HMR file watcher.
//
// Detection between classic-script and ES-module is automatic based on a top-
// level `import` / `export` scan. The synthetic `fxe` specifier exports
// `Window`, `Renderer`, `Primitives`, and `CommandBuffer`, so the canonical
// idiom works:
//
//   import { Window, Renderer, Primitives } from 'fxe';
//   const win = new Window({ width: 640, height: 480, title: 'demo' });
//   const r   = new Renderer(win);
//   r.beginFrame();
//   Primitives.fillRect(r, 10, 10, 200, 80, 0, 0xffffffff);
//   r.endFrame();
//
// Other environment variables:
//   FXE_V8_ICUDTL — explicit path to icudtl.dat (used when V8 was not built
//                    with embedded ICU data). Falls back to argv[0]-relative
//                    lookup; finally falls back to the build-time discovered
//                    location baked into FXE_V8_ICUDTL_PATH.

#include <fxe/debug.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/renderer.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/window.hpp>

#include "../audio/audio.hpp"
#include "../debug/screenshot.hpp"

#include "../js/bind_process.hpp"
#include "../runtime/bundle_loader.hpp"
#include "../runtime/v8/fs_watcher.hpp"

#include "../runtime/updater.hpp"
#include "cpu_profile.hpp"
#include "cpu_profile_merge.hpp"
#include "cpu_profile_native.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef FXE_V8_ICUDTL_PATH
#define FXE_V8_ICUDTL_PATH ""
#endif

namespace {
  std::string resolve_icudtl(const char* argv0) {
    if (const char* env = std::getenv("FXE_V8_ICUDTL"); env && *env)
      return env;

    namespace fs = std::filesystem;
    std::error_code ec;
    if (argv0) {
      fs::path beside = fs::path(argv0).parent_path() / "icudtl.dat";
      if (fs::exists(beside, ec))
        return beside.string();
    }
    if constexpr (sizeof(FXE_V8_ICUDTL_PATH) > 1) {
      fs::path baked{FXE_V8_ICUDTL_PATH};
      if (fs::exists(baked, ec))
        return baked.string();
    }
    return {};
  }

  void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--debug[=PORT]] [--debug-host=ADDR] [--debug-pause]\n"
                 "          [--debug-keepalive] [--watch] [--vsync|--no-vsync]\n"
                 "          [--fps-limit=N] [--msaa=N] [--bloom|--no-bloom]\n"
                 "          [--render-surface=window|offscreen] [--show-fps] [--no-lazy]\n"
                 "          [--screenshot[=PATH]] [--screenshot-delay=MS]\n"
                 "          [--screenshot-frames=N] [--screenshot-size=WxH]\n"
                 "          [--screenshot-format=png|jpeg] [--screenshot-quality=N]\n"
                 "          [--screenshot-stay] <script.js> [more.js ...]\n"
                 "\n"
                 "  Runs the supplied JavaScript files through fxe's embedded V8.\n"
                 "  Use `import { Window, Renderer, Primitives } from 'fxe';`\n"
                 "  to access the engine constructors.\n"
                 "\n"
                 "  --debug[=PORT]         Start the debug protocol server on PORT\n"
                 "                          (default 9333, 0 = OS-assigned).\n"
                 "  --debug-host=ADDR      Bind address (default 127.0.0.1).\n"
                 "  --debug-pause          Pause before the first instruction.\n"
                 "  --debug-keepalive      Keep the server alive after script exit.\n"
                 "\n"
                 "  --vsync / --no-vsync  Override Renderer vsync for all renderers.\n"
                 "  --fps-limit=N         Override Window.run/App.run cadence.\n"
                 "                          N <= 0 leaves the loop event-driven\n"
                 "                          unless --no-lazy is also set.\n"
                 "  --no-lazy             Disable lazy frames: drive a redraw on\n"
                 "                          every loop iteration so animations\n"
                 "                          (e.g. --show-fps) keep ticking when\n"
                 "                          no input arrives. Vsync still paces\n"
                 "                          actual presents. Alias: --continuous.\n"
                 "  --msaa=N              Override Renderer multisampleCount.\n"
                 "                          Use 0 or 1 to disable MSAA.\n"
                 "  --bloom / --no-bloom  Override Renderer enableBloom.\n"
                 "  --render-surface=MODE Select renderer backing: window or offscreen.\n"
                 "                          Offscreen keeps Window invisible and uses\n"
                 "                          OffscreenRenderer for new Renderer(win).\n"
                 "  --show-fps            Draw a top-left FPS counter overlay.\n"
                 "\n"
                 "  --watch                Watch loaded file modules and fire global\n"
                 "                          HMR on changed TS/JS/JSON sources.\n"
                 "  --screenshot[=PATH]    Capture the framebuffer once a frame has\n"
                 "                          rendered and write it to PATH (default\n"
                 "                          screenshot.png), then exit. Format is\n"
                 "                          inferred from the extension unless\n"
                 "                          --screenshot-format overrides it.\n"
                 "  --screenshot-delay=MS  Wait MS milliseconds before capturing.\n"
                 "                          The render loop must keep ticking.\n"
                 "  --screenshot-frames=N  After the delay, render N additional\n"
                 "                          frames before reading the framebuffer\n"
                 "                          (default 1; arms readback then waits).\n"
                 "  --screenshot-size=WxH  Clamp output to a WxH bounding box,\n"
                 "                          preserving aspect ratio.\n"
                 "  --screenshot-format=F  Force png or jpeg.\n"
                 "  --screenshot-quality=N JPEG quality 1-100 (default 90).\n"
                 "  --screenshot-stay      Don't auto-close the window after\n"
                 "                          capture; keep the script running.\n"
                 "  --cpu-prof[=PATH]      Profile V8 + native code into PATH\n"
                 "                          (default fxe.cpuprofile). Merged.\n"
                 "  --cpu-prof-md[=PATH]   Also emit an agent-friendly markdown\n"
                 "                          report (default: PATH with .md ext).\n"
                 "  --cpu-prof-hz=N        Native sampling rate (default 1000).\n"
                 "  --cpu-prof-js-only     Skip the native sampler.\n"
                 "  --cpu-prof-native-only Skip V8's CPU profiler.\n"
                 "Environment variables (applied before CLI flags; CLI takes precedence):\n"
                 "  FXE_DEBUG=PORT          Enable debug server on PORT (0 = OS-assigned).\n"
                 "  FXE_DEBUG_HOST=ADDR      Bind address (default 127.0.0.1).\n"
                 "  FXE_DEBUG_PAUSE=1        Pause before the first instruction.\n"
                 "  FXE_DEBUG_KEEPALIVE=1     Keep debug server alive after script exit.\n"
                 "  FXE_VSYNC=1|0            Override Renderer vsync (1=on, 0=off).\n"
                 "  FXE_FPS_LIMIT=N          Override fps limit.\n"
                 "  FXE_MSAA=N               Override multisampleCount.\n"
                 "  FXE_BLOOM=1|0            Override enableBloom (1=on, 0=off).\n"
                 "  FXE_RENDER_SURFACE=MODE Override renderer backing (window/offscreen).\n"
                 "  FXE_SHOW_FPS=1           Enable FPS counter overlay.\n"
                 "  FXE_NO_LAZY=1            Disable lazy frames (continuous redraw).\n"
                 "  FXE_WATCH=1              Enable HMR file watcher.",
                 argv0 ? argv0 : "fxe_run");
  }

  struct cli_options {
    bool debug = false;
    u16 debug_port = 9333;
    std::string debug_host = "127.0.0.1";
    bool debug_pause = false;
    bool debug_keepalive = false;
    std::vector<std::string> scripts;

    // Screenshot one-shot (Chromium-style: --screenshot=PATH).
    bool screenshot = false;
    std::string screenshot_path = "screenshot.png";
    std::string screenshot_format; // png|jpeg; empty -> infer from path
    int screenshot_quality = 90;
    long long screenshot_delay_ms = 0;
    int screenshot_frames = 1; // additional frames to render after delay
    u32 screenshot_max_w = 0;
    u32 screenshot_max_h = 0;
    bool screenshot_exit_after = true; // close window once captured
    bool show_usage = false;
    bool watch = false;

    // CPU profile (V8 + native sampling, merged).
    bool cpu_prof = false;
    std::string cpu_prof_path = "fxe.cpuprofile";
    bool cpu_prof_md = false;
    std::string cpu_prof_md_path; // empty -> derived from cpu_prof_path
    int cpu_prof_hz = 1000;
    bool cpu_prof_native_only = false;
    bool cpu_prof_js_only = false;

    fxe::js::runner_render_overrides render_overrides;
  };

  bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
  }

  // Read boolean env var: "1" / "true" / "yes" (case-insensitive) → true.
  bool env_bool(const char* name) {
    if (const char* v = std::getenv(name); v && *v) {
      char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
      return c == '1' || c == 't' || c == 'y';
    }
    return false;
  }

  bool apply_render_surface(cli_options& o, std::string_view value) {
    std::string mode(value);
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode == "window") {
      o.render_overrides.override_render_surface = true;
      o.render_overrides.render_surface = fxe::js::runner_render_surface::window;
      o.render_overrides.override_window_visible = false;
      o.render_overrides.window_visible = true;
      return true;
    }
    if (mode == "offscreen") {
      o.render_overrides.override_render_surface = true;
      o.render_overrides.render_surface = fxe::js::runner_render_surface::offscreen;
      // Offscreen mode still lets scripts construct Window objects for event
      // loops/input dispatch, but those windows are only control surfaces.
      o.render_overrides.override_window_visible = true;
      o.render_overrides.window_visible = false;
      return true;
    }
    return false;
  }

  // Apply environment variables as defaults into `opts`.
  // CLI flags parsed later will override these values for any flag that
  // appears on the command line, because parse_args always writes the field
  // when it encounters the corresponding --flag.
  void apply_env_overrides(cli_options& o) {
    if (const char* v = std::getenv("FXE_DEBUG"); v && *v) {
      o.debug = true;
      try {
        o.debug_port = static_cast<u16>(std::stoi(v));
      } catch (...) {
      }
    }
    if (const char* v = std::getenv("FXE_DEBUG_HOST"); v && *v) {
      o.debug_host = v;
    }
    if (env_bool("FXE_DEBUG_PAUSE"))
      o.debug_pause = true;
    if (env_bool("FXE_DEBUG_KEEPALIVE"))
      o.debug_keepalive = true;
    if (const char* v = std::getenv("FXE_VSYNC"); v && *v) {
      char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
      o.render_overrides.override_vsync = true;
      o.render_overrides.vsync = (c == '1' || c == 't' || c == 'y');
    }
    if (const char* v = std::getenv("FXE_FPS_LIMIT"); v && *v) {
      o.render_overrides.override_fps = true;
      try {
        o.render_overrides.fps = std::stod(v);
      } catch (...) {
      }
    }
    if (const char* v = std::getenv("FXE_MSAA"); v && *v) {
      o.render_overrides.override_multisample_count = true;
      try {
        o.render_overrides.multisample_count = static_cast<u32>(std::stoul(v));
      } catch (...) {
      }
    }
    if (const char* v = std::getenv("FXE_BLOOM"); v && *v) {
      char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
      o.render_overrides.override_bloom = true;
      o.render_overrides.enable_bloom = (c == '1' || c == 't' || c == 'y');
    }
    if (const char* v = std::getenv("FXE_RENDER_SURFACE"); v && *v)
      (void)apply_render_surface(o, v);
    if (env_bool("FXE_SHOW_FPS"))
      o.render_overrides.show_fps_counter = true;
    if (env_bool("FXE_NO_LAZY"))
      o.render_overrides.force_continuous = true;
    if (env_bool("FXE_WATCH"))
      o.watch = true;
  }

  cli_options parse_args(int argc, char** argv, cli_options base = {}) {
    cli_options o = base;
    for (int i = 1; i < argc; ++i) {
      std::string_view a = argv[i];
      if (a == "-h" || a == "--help") {
        o.show_usage = true;
        return o;
      }
      if (a == "--debug") {
        o.debug = true;
      } else if (starts_with(a, "--debug=")) {
        o.debug = true;
        auto v = a.substr(8);
        if (!v.empty())
          o.debug_port = static_cast<u16>(std::stoi(std::string(v)));
      } else if (starts_with(a, "--debug-host=")) {
        o.debug_host = std::string(a.substr(13));
      } else if (a == "--debug-pause") {
        o.debug_pause = true;
      } else if (a == "--debug-keepalive") {
        o.debug_keepalive = true;
      } else if (a == "--vsync") {
        o.render_overrides.override_vsync = true;
        o.render_overrides.vsync = true;
      } else if (a == "--no-vsync") {
        o.render_overrides.override_vsync = true;
        o.render_overrides.vsync = false;
      } else if (starts_with(a, "--fps-limit=")) {
        o.render_overrides.override_fps = true;
        o.render_overrides.fps = std::stod(std::string(a.substr(12)));
      } else if (starts_with(a, "--fps=")) {
        o.render_overrides.override_fps = true;
        o.render_overrides.fps = std::stod(std::string(a.substr(6)));
      } else if (starts_with(a, "--msaa=")) {
        o.render_overrides.override_multisample_count = true;
        o.render_overrides.multisample_count =
            static_cast<u32>(std::stoul(std::string(a.substr(7))));
      } else if (starts_with(a, "--samples=")) {
        o.render_overrides.override_multisample_count = true;
        o.render_overrides.multisample_count =
            static_cast<u32>(std::stoul(std::string(a.substr(10))));
      } else if (a == "--bloom") {
        o.render_overrides.override_bloom = true;
        o.render_overrides.enable_bloom = true;
      } else if (a == "--no-bloom") {
        o.render_overrides.override_bloom = true;
        o.render_overrides.enable_bloom = false;
      } else if (a == "--show-fps") {
        o.render_overrides.show_fps_counter = true;
      } else if (starts_with(a, "--render-surface=") || starts_with(a, "--surface=")) {
        std::string_view value = starts_with(a, "--surface=") ? a.substr(10) : a.substr(17);
        if (!apply_render_surface(o, value)) {
          std::fprintf(stderr, "fxe_run: --render-surface must be 'window' or 'offscreen'\n");
          o.show_usage = true;
          return o;
        }
      } else if (a == "--no-lazy" || a == "--continuous") {
        o.render_overrides.force_continuous = true;
      } else if (a == "--watch") {
        o.watch = true;
      } else if (a == "--screenshot") {
        o.screenshot = true;
      } else if (starts_with(a, "--screenshot=")) {
        o.screenshot = true;
        o.screenshot_path = std::string(a.substr(13));
      } else if (starts_with(a, "--screenshot-delay=")) {
        o.screenshot_delay_ms = std::stoll(std::string(a.substr(19)));
      } else if (starts_with(a, "--screenshot-frames=")) {
        o.screenshot_frames = std::stoi(std::string(a.substr(20)));
      } else if (starts_with(a, "--screenshot-format=")) {
        o.screenshot_format = std::string(a.substr(20));
      } else if (starts_with(a, "--screenshot-quality=")) {
        o.screenshot_quality = std::stoi(std::string(a.substr(21)));
      } else if (starts_with(a, "--screenshot-size=")) {
        // WxH (e.g. 1920x1080)
        std::string v(a.substr(18));
        auto x = v.find('x');
        if (x == std::string::npos)
          x = v.find('X');
        if (x == std::string::npos) {
          std::fprintf(stderr, "fxe_run: --screenshot-size expects WxH\n");
          o.show_usage = true;
          return o;
        }
        o.screenshot_max_w = static_cast<u32>(std::stoul(v.substr(0, x)));
        o.screenshot_max_h = static_cast<u32>(std::stoul(v.substr(x + 1)));
      } else if (a == "--cpu-prof") {
        o.cpu_prof = true;
      } else if (starts_with(a, "--cpu-prof=")) {
        o.cpu_prof = true;
        o.cpu_prof_path = std::string(a.substr(11));
      } else if (a == "--cpu-prof-md") {
        o.cpu_prof = true;
        o.cpu_prof_md = true;
      } else if (starts_with(a, "--cpu-prof-md=")) {
        o.cpu_prof = true;
        o.cpu_prof_md = true;
        o.cpu_prof_md_path = std::string(a.substr(14));
      } else if (starts_with(a, "--cpu-prof-hz=")) {
        o.cpu_prof_hz = std::stoi(std::string(a.substr(14)));
      } else if (a == "--cpu-prof-native-only") {
        o.cpu_prof = true;
        o.cpu_prof_native_only = true;
      } else if (a == "--cpu-prof-js-only") {
        o.cpu_prof = true;
        o.cpu_prof_js_only = true;
      } else if (a == "--screenshot-stay") {
        o.screenshot_exit_after = false;
      } else if (starts_with(a, "--")) {
        std::fprintf(stderr, "fxe_run: unknown flag '%.*s'\n", int(a.size()), a.data());
        o.show_usage = true;
        return o;
      } else {
        o.scripts.emplace_back(a);
      }
    }
    return o;
  }

  // Runner signal handling is deliberately tiny in the signal handler: record
  // the requested shutdown, then let the normal render-thread pump close
  // windows and unwind profiling/debug teardown in order.
  volatile std::sig_atomic_t& shutdown_signal() {
    static volatile std::sig_atomic_t sig = 0;
    return sig;
  }

  void request_shutdown_from_signal(int sig) {
    if (shutdown_signal() == 0)
      shutdown_signal() = sig;
  }

  void install_runner_signal_handlers() {
    std::signal(SIGINT, &request_shutdown_from_signal);
    std::signal(SIGTERM, &request_shutdown_from_signal);
#ifdef SIGHUP
    std::signal(SIGHUP, &request_shutdown_from_signal);
#endif
  }

  int requested_shutdown_signal() {
    return static_cast<int>(shutdown_signal());
  }

  void close_windows_for_signal(fxe::js::host* host) {
    if (!host || requested_shutdown_signal() == 0)
      return;
    for (auto* win : host->windows()) {
      if (win && !win->should_close())
        win->close();
    }
  }

  int signal_exit_status(int sig) {
    return sig > 0 ? 128 + sig : 0;
  }

  // Console sink: forward V8 console output to the debug server. Trampolines
  // between the host's C-style callback signature and the server method.
  void console_forwarder(void* user, std::string_view level, std::string_view text) {
    if (auto* srv = static_cast<fxe::debug::server*>(user))
      srv->emit_console(level, text);
  }

  // ---- Screenshot watchdog ----------------------------------------------
  //
  // Runs once per host iteration on the render thread (via the same hook the
  // debug server uses). State machine:
  //   wait    : count down delay_ms wall-clock from the first frame.
  //   arm     : call capture_frame() to arm the GPU readback.
  //   tick    : let `frames` more end_frame()s elapse so the readback fills.
  //   capture : pull pixels, resample, encode, write the file.
  //   done    : ask the window to close so the script's main loop returns.
  struct screenshot_watchdog {
    fxe::js::host* host = nullptr;
    fxe::debug::server* debug_srv = nullptr; // chained pump (may be null)
    std::string path;
    std::string format; // png|jpeg, lowercase
    int quality = 90;
    long long delay_ms = 0;
    int frames_remaining = 1;
    u32 max_w = 0, max_h = 0;
    bool exit_after = true;
    int exit_code = 0;

    enum class phase_t { wait, arm, tick, capture, done } phase = phase_t::wait;
    std::chrono::steady_clock::time_point t0{};

    void step() {
      if (phase == phase_t::done)
        return;
      if (t0.time_since_epoch().count() == 0)
        t0 = std::chrono::steady_clock::now();

      auto* rdr = host ? host->active_renderer() : nullptr;
      auto* win = host ? host->active_window() : nullptr;
      if (!rdr || !win)
        return; // script hasn't created them yet

      switch (phase) {
      case phase_t::wait: {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        if (elapsed >= delay_ms)
          phase = phase_t::arm;
        break;
      }
      case phase_t::arm: {
        // First call arms the readback; pixels become available after the
        // next end_frame() completes.
        (void)rdr->capture_frame();
        phase = phase_t::tick;
        break;
      }
      case phase_t::tick: {
        if (--frames_remaining <= 0)
          phase = phase_t::capture;
        break;
      }
      case phase_t::capture: {
        auto cap = rdr->capture_frame();
        if (!cap.ok) {
          // Not ready yet (race with first frame); spin one more tick.
          return;
        }
        if (cap.rgba.size() < static_cast<usize>(cap.width) * cap.height * 4u) {
          std::fprintf(stderr, "fxe_run: screenshot: short readback\n");
          exit_code = 1;
          phase = phase_t::done;
          if (exit_after)
            win->close();
          return;
        }

        // Optional bounding-box scale-down (preserve aspect).
        u32 out_w = cap.width;
        u32 out_h = cap.height;
        if (max_w > 0 && out_w > max_w) {
          double k = static_cast<double>(max_w) / out_w;
          out_w = max_w;
          out_h = static_cast<u32>(out_h * k + 0.5);
        }
        if (max_h > 0 && out_h > max_h) {
          double k = static_cast<double>(max_h) / out_h;
          out_h = max_h;
          out_w = static_cast<u32>(out_w * k + 0.5);
        }
        if (out_w == 0)
          out_w = 1;
        if (out_h == 0)
          out_h = 1;

        const u8* pixels = cap.rgba.data();
        u32 stride = cap.width * 4u;
        std::vector<u8> resampled;
        if (out_w != cap.width || out_h != cap.height) {
          if (!fxe::debug::crop_resize_rgba8(pixels, cap.width, cap.height, stride, 0, 0, cap.width,
                                             cap.height, out_w, out_h, resampled)) {
            std::fprintf(stderr, "fxe_run: screenshot: resize failed\n");
            exit_code = 1;
            phase = phase_t::done;
            if (exit_after)
              win->close();
            return;
          }
          pixels = resampled.data();
          stride = out_w * 4u;
        }

        std::string encoded;
        if (format == "jpeg")
          encoded = fxe::debug::encode_jpeg_rgba8(pixels, out_w, out_h, stride, quality);
        else
          encoded = fxe::debug::encode_png_rgba8(pixels, out_w, out_h, stride);
        if (encoded.empty()) {
          std::fprintf(stderr, "fxe_run: screenshot: %s encode failed\n", format.c_str());
          exit_code = 1;
        } else if (auto* f = std::fopen(path.c_str(), "wb")) {
          usize n = std::fwrite(encoded.data(), 1, encoded.size(), f);
          std::fclose(f);
          if (n != encoded.size()) {
            std::fprintf(stderr, "fxe_run: screenshot: short write to %s\n", path.c_str());
            exit_code = 1;
          } else {
            std::fprintf(stderr, "fxe_run: screenshot: wrote %s (%ux%u, %zu bytes)\n", path.c_str(),
                         out_w, out_h, encoded.size());
          }
        } else {
          std::fprintf(stderr, "fxe_run: screenshot: cannot open %s: %s\n", path.c_str(),
                       std::strerror(errno));
          exit_code = 1;
        }

        phase = phase_t::done;
        if (exit_after)
          win->close();
        break;
      }
      case phase_t::done:
        break;
      }
    }
  };

  // ---- HMR watcher ------------------------------------------------------
  //
  // Platform file-watch backed HMR. Rather than polling a whole directory tree,
  // this attaches one watcher per file-backed module that is currently resident
  // in the V8 module cache. Any change fires globalThis.__fxe_hmr.fire(path),
  // which invalidates the changed module plus transitive importers and invokes
  // both path-specific and global HMR handlers.
  struct hmr_watcher {
    fxe::js::host* host = nullptr;
    std::unordered_map<std::string, std::unique_ptr<fxe::runtime::fs_watcher>> watchers;
    std::unordered_set<std::string> failed_watch_paths;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending;
    std::mutex pending_mutex;
    std::chrono::steady_clock::time_point next_module_sync{};

    static bool is_hmr_source_path(const std::string& path) {
      auto ext = std::filesystem::path(path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return ext == ".ts" || ext == ".tsx" || ext == ".mts" || ext == ".cts" || ext == ".js" ||
             ext == ".jsx" || ext == ".mjs" || ext == ".cjs" || ext == ".json";
    }

    static std::string normalize_watch_path(const std::string& raw) {
      namespace fs = std::filesystem;
      std::error_code ec;
      auto path = fs::absolute(fs::path(raw), ec);
      if (ec)
        path = fs::path(raw);
      return path.lexically_normal().generic_string();
    }

    void enqueue(const std::string& raw_path) {
      if (!is_hmr_source_path(raw_path))
        return;

      const auto path = normalize_watch_path(raw_path);
      auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
      std::lock_guard lock(pending_mutex);
      pending[path] = due;
    }

    void sync_loaded_modules() {
      if (!host)
        return;

      for (const auto& path : host->loaded_module_paths()) {
        if (!is_hmr_source_path(path) || watchers.contains(path) ||
            failed_watch_paths.contains(path))
          continue;

        fxe::runtime::fs_watch_error watch_error;
        auto watcher = fxe::runtime::fs_watcher::create(
            path, false,
            [this](const fxe::runtime::fs_watch_event& event) {
              if (event.k == fxe::runtime::fs_watch_event::kind::deleted)
                return;
              enqueue(event.path);
            },
            &watch_error);
        if (!watcher) {
          failed_watch_paths.insert(path);
          std::string msg = watch_error.message;
          if (msg.empty())
            msg = "unknown watcher error";
          std::fprintf(stderr, "fxe_run: --watch: cannot watch %s: %s\n", path.c_str(),
                       msg.c_str());
          continue;
        }
        watchers.emplace(path, std::move(watcher));
      }
    }

    void step() {
      using clock = std::chrono::steady_clock;
      const auto now = clock::now();
      if (now >= next_module_sync) {
        sync_loaded_modules();
        next_module_sync = now + std::chrono::milliseconds(500);
      }

      std::vector<std::string> ready;
      {
        std::lock_guard lock(pending_mutex);
        for (auto it = pending.begin(); it != pending.end();) {
          if (it->second > now) {
            ++it;
            continue;
          }
          ready.push_back(it->first);
          it = pending.erase(it);
        }
      }

      std::sort(ready.begin(), ready.end());
      ready.erase(std::unique(ready.begin(), ready.end()), ready.end());

      for (const auto& path : ready) {
        int handlers_called = 0;
        auto result = host->fire_hmr(path, handlers_called);
        if (!result.ok) {
          std::fprintf(stderr, "fxe_run: --watch: HMR failed for %s: %s\n", path.c_str(),
                       result.message.c_str());
          continue;
        }
        std::fprintf(stderr, "fxe_run: --watch: HMR applied %s (%d handler%s)\n", path.c_str(),
                     handlers_called, handlers_called == 1 ? "" : "s");
        sync_loaded_modules();
      }
    }
  };

  constexpr const char* kFpsCounterScript = R"JS(
(() => {
  if (globalThis.__fxeRunnerFpsCounterInstalled) return;
  globalThis.__fxeRunnerFpsCounterInstalled = true;
  const Renderer = globalThis.Renderer;
  const Primitives = globalThis.Primitives;
  if (!Renderer || !Renderer.prototype || !Primitives) return;
  const originalEndFrame = Renderer.prototype.endFrame;
  if (typeof originalEndFrame !== 'function') return;

  let windowStart = Date.now();
  let frames = 0;
  let label = 'FPS --';

  Renderer.prototype.endFrame = function fxeRunnerFpsEndFrame(...args) {
    const now = Date.now();
    frames += 1;
    const elapsed = now - windowStart;
    if (elapsed >= 250) {
      label = 'FPS ' + Math.round((frames * 1000) / elapsed);
      frames = 0;
      windowStart = now;
    }
    try {
      Primitives.fillRect(this, 8, 8, 84, 24, 0, 0x000000cc);
      Primitives.drawText(this, 12, 12, 0, label, 14, 0xffffffff);
    } catch (_) {
      // The overlay must never change app behavior if rendering is mid-teardown.
    }
    return originalEndFrame.apply(this, args);
  };
})();
)JS";

  constexpr const char* kFrameStatsScript = R"JS(
(() => {
  if (globalThis.__fxeRunnerFrameStatsInstalled) return;
  globalThis.__fxeRunnerFrameStatsInstalled = true;
  const Renderer = globalThis.Renderer;
  if (!Renderer || !Renderer.prototype) return;
  const originalEndFrame = Renderer.prototype.endFrame;
  if (typeof originalEndFrame !== 'function') return;
  const stats = {
    frames: 0,
    intervals: 0,
    minMs: 0,
    maxMs: 0,
    totalMs: 0,
    lastMs: 0,
  };
  globalThis.__fxeRunnerFrameStats = stats;
  const nowMs = () => {
    const perf = globalThis.performance;
    return perf && typeof perf.now === 'function' ? perf.now() : Date.now();
  };
  Renderer.prototype.endFrame = function fxeRunnerFrameStatsEndFrame(...args) {
    const result = originalEndFrame.apply(this, args);
    const now = nowMs();
    if (stats.lastMs > 0) {
      const dt = now - stats.lastMs;
      if (Number.isFinite(dt) && dt > 0) {
        stats.intervals += 1;
        stats.totalMs += dt;
        if (stats.minMs === 0 || dt < stats.minMs) stats.minMs = dt;
        if (dt > stats.maxMs) stats.maxMs = dt;
      }
    }
    stats.lastMs = now;
    stats.frames += 1;
    return result;
  };
})();
)JS";
  // Combined trampoline: drains the debug server (if any) and then steps the
  // screenshot watchdog. Both attach through the host's single pump slot.
  struct combined_pump {
    fxe::js::host* host = nullptr;
    fxe::debug::server* srv = nullptr;
    screenshot_watchdog* shot = nullptr;
    hmr_watcher* hmr = nullptr;
  };

  void combined_pump_trampoline(void* user) {
    auto* cp = static_cast<combined_pump*>(user);
    if (cp->srv)
      cp->srv->pump_tasks();
    if (cp->shot)
      cp->shot->step();
    if (cp->hmr)
      cp->hmr->step();
    close_windows_for_signal(cp->host);
  }
  bool combined_paused_trampoline(void* user) {
    auto* cp = static_cast<combined_pump*>(user);
    return cp->srv ? cp->srv->is_paused() : false;
  }

  fxe::runner::frame_fps_stats read_frame_fps_stats(fxe::js::host& host) {
    fxe::runner::frame_fps_stats out;
    auto er = host.debug_evaluate(R"JS(
(() => {
  const s = globalThis.__fxeRunnerFrameStats;
  if (!s || !s.intervals || !s.totalMs || !s.minMs || !s.maxMs) return null;
  return {
    frames: s.frames || 0,
    intervals: s.intervals || 0,
    minFps: 1000 / s.maxMs,
    maxFps: 1000 / s.minMs,
    avgFps: (s.intervals * 1000) / s.totalMs,
  };
})()
)JS",
                                  true);
    if (!er.exception.empty() || er.json_value.empty())
      return out;
    try {
      auto j = nlohmann::json::parse(er.json_value);
      if (!j.is_object())
        return out;
      out.frames = j.value("frames", 0ull);
      out.intervals = j.value("intervals", 0ull);
      out.min_fps = j.value("minFps", 0.0);
      out.max_fps = j.value("maxFps", 0.0);
      out.avg_fps = j.value("avgFps", 0.0);
      out.valid = out.intervals > 0 && out.min_fps > 0.0 && out.max_fps > 0.0 && out.avg_fps > 0.0;
    } catch (...) {
      out = {};
    }
    return out;
  }
} // namespace

int main(int argc, char** argv) {
  // Mount any embedded bundle appended to argv[0] before parsing args; a
  // packed binary may inject default scripts via the bundle entry name.
  fxe::runtime::mount_bundle_from_argv0(argv[0]);
  fxe::js::set_host_argv(std::vector<std::string>(argv, argv + argc));

  cli_options opts;
  apply_env_overrides(opts);
  try {
    opts = parse_args(argc, argv, opts);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fxe_run: invalid flag value: %s\n", e.what());
    usage(argv[0]);
    return 64;
  }
  if (opts.show_usage || opts.scripts.empty()) {
    usage(argv[0]);
    return opts.show_usage ? 0 : 64;
  }

  // Gate the C1 auto-rollback on an explicit env var so the test suite
  // (which uses a shared userData root and stages updates without calling
  // markReady) doesn't roll back unexpectedly. Production builds should set
  // FXE_UPDATER_AUTO_ROLLBACK=1 at install time.
  if (const char* env = std::getenv("FXE_UPDATER_AUTO_ROLLBACK"); env && env[0] == '1') {
    std::string rolled = fxe::runtime::updater::auto_rollback_if_unready();
    if (!rolled.empty()) {
      std::fprintf(stderr,
                   "fxe: previous launch did not call App.update.markReady(); "
                   "rolled back from version %s\n",
                   rolled.c_str());
    }
  }

  std::string icudtl = resolve_icudtl(argv[0]);
  fxe::js::initialize(argv[0], icudtl);

  int status = 0;
  install_runner_signal_handlers();
  {
    fxe::js::host host;

    fxe::js::set_runner_render_overrides(opts.render_overrides);

    // ----- Debug server -----------------------------------------------------
    std::unique_ptr<fxe::debug::server> debug_srv;
    if (opts.debug) {
      fxe::debug::server_options sopts;
      sopts.port = opts.debug_port;
      sopts.host = opts.debug_host;
      sopts.start_paused = opts.debug_pause;
      sopts.keepalive = opts.debug_keepalive;
      debug_srv = std::make_unique<fxe::debug::server>(std::move(sopts));
      debug_srv->attach_host(&host);
      if (!debug_srv->start()) {
        std::fprintf(stderr, "fxe_run: debug server failed to start: %s\n",
                     debug_srv->last_error().c_str());
        return 1;
      }
      // User-facing listening notice (Node-style).
      std::fprintf(stderr,
                   "Debugger listening on tcp://%s:%u\n"
                   "  Protocol: NDJSON / JSON-RPC (fxe debug v0.1.0)\n"
                   "  Try:      bun run clients/ts/src/cli.ts inspect --port %u\n",
                   opts.debug_host.c_str(), unsigned(debug_srv->bound_port()),
                   unsigned(debug_srv->bound_port()));
      std::fflush(stderr);

      host.set_console_sink(&console_forwarder, debug_srv.get());

      // Honor --debug-pause: hold the main thread until the client clears it.
      if (opts.debug_pause) {
        std::fprintf(stderr, "fxe_run: paused; awaiting Debugger.resume...\n");
        std::fflush(stderr);
        while (debug_srv->is_paused() && requested_shutdown_signal() == 0) {
          debug_srv->pump_tasks();
          close_windows_for_signal(&host);
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (requested_shutdown_signal() != 0)
          status = signal_exit_status(requested_shutdown_signal());
      }
    }

    // ----- Screenshot watchdog + combined pump ------------------------------
    screenshot_watchdog shot;
    combined_pump pump_state{&host, debug_srv.get(), nullptr, nullptr};
    if (opts.screenshot) {
      shot.host = &host;
      shot.debug_srv = debug_srv.get();
      shot.path = opts.screenshot_path;
      shot.delay_ms = opts.screenshot_delay_ms;
      shot.frames_remaining = std::max(1, opts.screenshot_frames);
      shot.max_w = opts.screenshot_max_w;
      shot.max_h = opts.screenshot_max_h;
      shot.exit_after = opts.screenshot_exit_after;
      shot.quality = std::clamp(opts.screenshot_quality, 1, 100);

      // Resolve format: explicit flag wins; else infer from extension.
      std::string fmt = opts.screenshot_format;
      std::transform(fmt.begin(), fmt.end(), fmt.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (fmt == "jpg")
        fmt = "jpeg";
      if (fmt.empty()) {
        auto dot = opts.screenshot_path.rfind('.');
        if (dot != std::string::npos) {
          std::string ext = opts.screenshot_path.substr(dot + 1);
          std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (ext == "jpg" || ext == "jpeg")
            fmt = "jpeg";
          else
            fmt = "png";
        } else {
          fmt = "png";
        }
      }
      if (fmt != "png" && fmt != "jpeg") {
        std::fprintf(stderr, "fxe_run: --screenshot-format must be png or jpeg (got '%s')\n",
                     fmt.c_str());
        return 64;
      }
      shot.format = fmt;
      pump_state.shot = &shot;
    }

    // ----- HMR watcher ------------------------------------------------------
    hmr_watcher hmr;
    if (opts.watch) {
      hmr.host = &host;
      pump_state.hmr = &hmr;
      std::fprintf(stderr, "fxe_run: --watch: HMR file watcher enabled\n");
      std::fflush(stderr);
    }
    host.attach_debug_pump(&combined_pump_trampoline, &combined_paused_trampoline, &pump_state);

    // ----- CPU profile (V8 + native, merged) -------------------------------
    fxe::runner::native_profiler nprof;
    bool nprof_started = false;
    bool jsprof_started = false;
    if (opts.cpu_prof) {
      if (!opts.cpu_prof_native_only) {
        auto r =
            host.debug_profiler_start(static_cast<int>(1'000'000 / std::max(1, opts.cpu_prof_hz)));
        if (!r.ok) {
          std::fprintf(stderr, "fxe_run: --cpu-prof: V8 profiler start failed: %s\n",
                       r.message.c_str());
          status = 1;
        } else {
          jsprof_started = true;
        }
      }
      if (status == 0 && !opts.cpu_prof_js_only) {
        // Size the ring for ~ hz * 120s of headroom; capped to keep the
        // resident set bounded.
        usize cap = static_cast<usize>(opts.cpu_prof_hz) * 120u;
        if (cap < 4096)
          cap = 4096;
        if (cap > (1u << 20))
          cap = (1u << 20);
        std::string err;
        if (!nprof.start(opts.cpu_prof_hz, cap, err)) {
          std::fprintf(stderr, "fxe_run: --cpu-prof: native profiler start failed: %s\n",
                       err.c_str());
          // Non-fatal: keep V8 profile if it started.
        } else {
          nprof_started = true;
        }
      }
      std::fprintf(stderr, "fxe_run: --cpu-prof: js=%s native=%s hz=%d -> %s\n",
                   jsprof_started ? "on" : "off", nprof_started ? "on" : "off", opts.cpu_prof_hz,
                   opts.cpu_prof_path.c_str());
      std::fflush(stderr);
    }

    if (status == 0 && opts.cpu_prof_md) {
      auto r = host.run_script(kFrameStatsScript, "<fxe-runner-frame-stats>");
      if (!r.ok) {
        std::fprintf(stderr, "fxe_run: --cpu-prof-md: frame stats install failed: %s\n",
                     r.message.c_str());
        status = 1;
      }
    }

    if (opts.render_overrides.show_fps_counter) {
      auto r = host.run_script(kFpsCounterScript, "<fxe-runner-fps-counter>");
      if (!r.ok) {
        std::fprintf(stderr, "fxe_run: --show-fps: %s\n", r.message.c_str());
        status = 1;
      }
    }

    // ----- Script execution -------------------------------------------------
    if (status == 0) {
      for (auto& s : opts.scripts) {
        if (requested_shutdown_signal() != 0) {
          status = signal_exit_status(requested_shutdown_signal());
          break;
        }
        auto r = host.run_file(s);
        if (requested_shutdown_signal() != 0 && r.ok) {
          status = signal_exit_status(requested_shutdown_signal());
          break;
        }
        if (!r.ok) {
          std::fprintf(stderr, "fxe_run: %s: %s\n", s.c_str(), r.message.c_str());
          status = 1;
          break;
        }
      }
    }

    // Surface screenshot watchdog failures (encode/write errors) as exit code.
    if (opts.screenshot && shot.exit_code != 0 && status == 0)
      status = shot.exit_code;

    // ----- CPU profile: stop + merge + write -------------------------------
    if (opts.cpu_prof) {
      fxe::runner::profile_data js_prof;
      fxe::runner::profile_data native_prof;

      // Stop the native sampler first. V8's CPU profiler hooks SIGPROF on
      // POSIX and restores its saved prior action when stopped; if our
      // sampler thread is still alive at that moment, the next pthread_kill
      // hits SIG_DFL and kills the process. Native stop disarms our
      // handler (-> SIG_IGN) and joins the sampler before doing anything
      // else, so it's safe to land here first.
      if (nprof_started)
        native_prof = nprof.stop();

      if (jsprof_started) {
        auto r = host.debug_profiler_stop();
        if (!r.ok) {
          std::fprintf(stderr, "fxe_run: --cpu-prof: V8 profiler stop failed: %s\n",
                       r.message.c_str());
        } else {
          std::string err;
          if (!fxe::runner::parse_v8_profile(r.profile_json, js_prof, err)) {
            std::fprintf(stderr, "fxe_run: --cpu-prof: %s\n", err.c_str());
            // Fall back to writing raw V8 JSON only.
            if (auto* f = std::fopen(opts.cpu_prof_path.c_str(), "wb")) {
              std::fwrite(r.profile_json.data(), 1, r.profile_json.size(), f);
              std::fclose(f);
            }
          }
        }
      }

      auto merged = fxe::runner::merge_profiles(js_prof, native_prof);

      // Write JSON.
      auto json = fxe::runner::serialize_cpuprofile(merged);
      if (auto* f = std::fopen(opts.cpu_prof_path.c_str(), "wb")) {
        if (std::fwrite(json.data(), 1, json.size(), f) != json.size())
          std::fprintf(stderr, "fxe_run: --cpu-prof: short write to %s\n",
                       opts.cpu_prof_path.c_str());
        std::fclose(f);
        std::fprintf(stderr,
                     "fxe_run: --cpu-prof: wrote %s "
                     "(%zu nodes, %zu samples, %llu dropped)\n",
                     opts.cpu_prof_path.c_str(), merged.nodes.size(), merged.samples.size(),
                     static_cast<unsigned long long>(merged.dropped_samples));
      } else {
        std::fprintf(stderr, "fxe_run: --cpu-prof: cannot open %s: %s\n",
                     opts.cpu_prof_path.c_str(), std::strerror(errno));
      }

      // Markdown export.
      if (opts.cpu_prof_md) {
        std::string md_path = opts.cpu_prof_md_path;
        if (md_path.empty()) {
          md_path = opts.cpu_prof_path;
          auto dot = md_path.rfind('.');
          if (dot != std::string::npos)
            md_path.replace(dot, std::string::npos, ".md");
          else
            md_path += ".md";
        }
        auto fps = read_frame_fps_stats(host);
        auto md = fxe::runner::render_markdown(merged, fps.valid ? &fps : nullptr);
        if (auto* f = std::fopen(md_path.c_str(), "wb")) {
          std::fwrite(md.data(), 1, md.size(), f);
          std::fclose(f);
          std::fprintf(stderr, "fxe_run: --cpu-prof-md: wrote %s\n", md_path.c_str());
        } else {
          std::fprintf(stderr, "fxe_run: --cpu-prof-md: cannot open %s: %s\n", md_path.c_str(),
                       std::strerror(errno));
        }
      }
      std::fflush(stderr);
    }

    // ----- Keepalive --------------------------------------------------------
    if (debug_srv && opts.debug_keepalive && status == 0) {
      std::fprintf(stderr, "fxe_run: script exited; debug server keepalive engaged. "
                           "Send System.shutdown to exit.\n");
      std::fflush(stderr);
      while (debug_srv->running()) {
        debug_srv->pump_tasks();
        if (pump_state.hmr)
          pump_state.hmr->step();
        close_windows_for_signal(&host);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }

    if (debug_srv) {
      host.set_console_sink(nullptr, nullptr);
      debug_srv->stop();
    }
    fxe::audio::shutdown();
    fxe::js::shutdown();
    std::exit(status);
  }
}
