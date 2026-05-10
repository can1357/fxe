// Method table for the fxe debug protocol. Each handler receives a
// dispatch_context (already validated by the server) and the parsed `params`
// object. Handlers run on the render thread.
//
// The dispatch layer always assumes window + renderer are wired up — fxe_debug
// links against fxe_window. V8-host-dependent handlers degrade to a typed
// error when no host is attached so the same binary works for both fxe_run
// (with V8) and any future native runner.
//
// JSON ID/handle warning: this dispatch layer uses nlohmann::ordered_json at
// protocol boundaries, but several upstream IDs originate in JavaScript/V8
// where numeric values have already passed through IEEE-754 double precision.
// Never read ID/handle fields with json::get<i64>() directly. Use
// parse_int64_safe() at the boundary so fractional, out-of-range, or lossy
// numeric IDs fail with invalid_params instead of silently aliasing handles.

#include "dispatch.hpp"

#include <fxe/debug.hpp>
#include <fxe/renderer.hpp>
#include <fxe/string_utils.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/window.hpp>
#if defined(FXE_DEBUG_HAS_WEBAUTHN)
#include "../webauthn/debug_handlers.hpp"
#endif

#include <atomic>
#include <unordered_map>

#include "screenshot.hpp"
#include "server_internal.hpp"
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fxe/types.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace fxe::debug {
  runtime_handlers& runtime_storage();
  profiler_handlers& profiler_storage();
  heap_profiler_handlers& heap_profiler_storage();

  namespace {
    std::atomic<bool> g_network_enabled{false};
    std::atomic<u64> g_network_request_counter{0};

    double network_timestamp_now() {
      using clock = std::chrono::steady_clock;
      return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    double network_wall_time_now() {
      using clock = std::chrono::system_clock;
      return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    json network_headers_to_json(const std::vector<std::pair<std::string, std::string>>& headers) {
      json out{json::object()};
      for (const auto& [name, value] : headers) {
        if (name.empty())
          continue;
        const std::string key = ascii_lower(name);
        auto it = out.find(key);
        if (it != out.end() && it->is_string() && key == "set-cookie")
          *it = it->get_ref<const std::string&>() + "\n" + value;
        else
          out[key] = value;
      }
      return out;
    }

    std::optional<std::string> network_post_data(std::optional<std::string_view> post_data) {
      if (!post_data.has_value() || post_data->empty())
        return std::nullopt;
      constexpr usize kMaxPostData = 65536;
      const usize n = std::min(post_data->size(), kMaxPostData);
      return std::string(post_data->substr(0, n));
    }

    void emit_network_event(std::string_view method, json params) {
      if (!g_network_enabled.load(std::memory_order_acquire))
        return;
      auto* srv = active_server();
      if (!srv)
        return;
      srv->emit_event(method, std::move(params));
    }

    using handler_fn = json (*)(dispatch_context&, const json&);
  } // namespace

  namespace {

    [[noreturn]] void invalid_params(std::string msg) {
      throw dispatch_error{err_code::invalid_params, std::move(msg), ""};
    }

    i64 parse_int64_safe(const json& j, std::string* err) {
      if (err)
        err->clear();
      auto fail = [err](std::string msg) -> i64 {
        if (err)
          *err = std::move(msg);
        return 0;
      };

      if (!j.is_number())
        return fail("must be a JSON number");
      double d = j.get<double>();
      if (!std::isfinite(d))
        return fail("must be finite");
      if (std::floor(d) != d)
        return fail("must be an integer");

      constexpr double kMinInt64 = -9223372036854775808.0;
      constexpr double kMaxInt64Exclusive = 9223372036854775808.0;
      if (d < kMinInt64 || d >= kMaxInt64Exclusive)
        return fail("is outside int64 range");

      auto out = static_cast<i64>(d);
      if (static_cast<double>(out) != d)
        return fail("loses precision when converted to int64");

      constexpr double kMaxLosslessJsonInteger = 9007199254740991.0;
      if (d < -kMaxLosslessJsonInteger || d > kMaxLosslessJsonInteger)
        return fail("is outside the lossless JSON integer range");

      return out;
    }

    usize parse_window_id(const json& j) {
      std::string err;
      auto id = parse_int64_safe(j, &err);
      if (!err.empty())
        invalid_params("windowId " + err);
      if (id < 0)
        invalid_params("windowId must be >= 0");
      return static_cast<usize>(id);
    }

    [[noreturn]] void no_host() {
      throw dispatch_error{err_code::detached, "V8 host not attached", ""};
    }

    [[noreturn]] void runtime_unavailable() {
      throw dispatch_error{err_code::service_unavailable,
                           "Runtime unavailable: V8 host not attached", "runtime_unavailable"};
    }

    [[noreturn]] void profiler_unavailable() {
      throw dispatch_error{err_code::service_unavailable,
                           "Profiler unavailable: V8 host not attached", "profiler_unavailable"};
    }

    [[noreturn]] void no_renderer() {
      throw dispatch_error{err_code::detached, "renderer not attached", ""};
    }

    [[noreturn]] void no_window() {
      throw dispatch_error{err_code::detached, "window not attached", ""};
    }

    [[noreturn]] void window_not_found(usize idx) {
      throw dispatch_error{err_code::detached, "window_not_found: index " + std::to_string(idx),
                           "window_not_found"};
    }

    // If `params` carries a numeric `windowId`, resolve via the host registry;
    // otherwise fall back to the per-context default (active window). Throws
    // dispatch_error{detached, "window_not_found"} when the index is out of
    // range so clients can disambiguate from "no window attached".
    window* resolve_window(dispatch_context& cx, const json& params) {
      if (params.is_object()) {
        if (auto it = params.find("windowId"); it != params.end() && it->is_number()) {
          if (!cx.host)
            no_host();
          auto idx = parse_window_id(*it);
          auto* win = cx.host->window_at(idx);
          if (!win)
            window_not_found(idx);
          return win;
        }
      }
      // Default: prefer the host's active (first registered) window so
      // protocol calls work even when only the JS side has wired up windows.
      if (cx.host) {
        if (auto* w = cx.host->active_window())
          return w;
      }
      if (!cx.win)
        no_window();
      return cx.win;
    }

    renderer* resolve_renderer(dispatch_context& cx, const json& params) {
      if (params.is_object()) {
        if (auto it = params.find("windowId"); it != params.end() && it->is_number()) {
          if (!cx.host)
            no_host();
          auto idx = parse_window_id(*it);
          auto* win = cx.host->window_at(idx);
          if (!win)
            window_not_found(idx);
          auto* r = cx.host->renderer_for(win);
          if (!r)
            no_renderer();
          return r;
        }
      }
      if (cx.host) {
        if (auto* r = cx.host->active_renderer())
          return r;
      }
      if (!cx.rdr)
        no_renderer();
      return cx.rdr;
    }

    bool profiler_available() {
      const auto& h = profiler_storage();
      return h.enable || h.disable || h.start || h.stop || h.start_precise_coverage ||
             h.take_precise_coverage;
    }

    bool heap_profiler_available() {
      const auto& h = heap_profiler_storage();
      return h.enable || h.disable || h.take_heap_snapshot || h.collect_garbage;
    }

    json h_handshake(dispatch_context& cx, const json& /*params*/) {
      json::array_t caps;
      caps.emplace_back(std::string("System.handshake"));
      caps.emplace_back(std::string("System.shutdown"));
      caps.emplace_back(std::string("Console.enable"));
      caps.emplace_back(std::string("Console.disable"));
      caps.emplace_back(std::string("Runtime.evaluate"));
      caps.emplace_back(std::string("Runtime.getGlobals"));
      caps.emplace_back(std::string("Runtime.fireHmr"));
      caps.emplace_back(std::string("Runtime.invalidateModule"));
      caps.emplace_back(std::string("Runtime.reimportModule"));
      caps.emplace_back(std::string("Page.framebufferSize"));
      caps.emplace_back(std::string("Page.requestRedraw"));
      caps.emplace_back(std::string("Page.screenshot"));
      caps.emplace_back(std::string("Page.windows"));
      caps.emplace_back(std::string("Input.dispatchMouseEvent"));
      caps.emplace_back(std::string("Input.dispatchKeyEvent"));
      caps.emplace_back(std::string("Window.close"));
      caps.emplace_back(std::string("Window.pollInput"));
      caps.emplace_back(std::string("Debugger.pause"));
      caps.emplace_back(std::string("Debugger.resume"));
      caps.emplace_back(std::string("Debugger.step"));
      caps.emplace_back(std::string("Schema.getDomains"));
      caps.emplace_back(std::string("Reconciler.snapshot"));
      caps.emplace_back(std::string("Runtime.enable"));
      caps.emplace_back(std::string("Debugger.enable"));
      caps.emplace_back(std::string("Network.enable"));
      caps.emplace_back(std::string("Network.disable"));
      caps.emplace_back(std::string("Performance.timeline"));
      caps.emplace_back(std::string("Window.subscribe"));
      caps.emplace_back(std::string("Window.unsubscribe"));
      caps.emplace_back(std::string("Fetch.subscribe"));
      caps.emplace_back(std::string("Fetch.unsubscribe"));
      caps.emplace_back(std::string("Fs.subscribe"));
      caps.emplace_back(std::string("Fs.unsubscribe"));
#if defined(FXE_DEBUG_HAS_WEBAUTHN)
      for (const auto capability : webauthn::debug::schema_capabilities())
        caps.emplace_back(std::string(capability));
#endif

      const auto& prof = profiler_storage();
      if (prof.enable)
        caps.emplace_back(std::string("Profiler.enable"));
      if (prof.disable)
        caps.emplace_back(std::string("Profiler.disable"));
      if (prof.start)
        caps.emplace_back(std::string("Profiler.start"));
      if (prof.stop)
        caps.emplace_back(std::string("Profiler.stop"));
      if (prof.start_precise_coverage)
        caps.emplace_back(std::string("Profiler.startPreciseCoverage"));
      if (prof.take_precise_coverage)
        caps.emplace_back(std::string("Profiler.takePreciseCoverage"));
      const auto& heap_prof = heap_profiler_storage();
      if (heap_prof.enable)
        caps.emplace_back(std::string("HeapProfiler.enable"));
      if (heap_prof.disable)
        caps.emplace_back(std::string("HeapProfiler.disable"));
      if (heap_prof.take_heap_snapshot)
        caps.emplace_back(std::string("HeapProfiler.takeHeapSnapshot"));
      if (heap_prof.collect_garbage)
        caps.emplace_back(std::string("HeapProfiler.collectGarbage"));
      json out{json::object()};
      out["engine"] = std::string("fxe");
      out["version"] = std::string("0.1.0");
      out["capabilities"] = std::move(caps);
      json features{json::object()};
      features["host"] = cx.host != nullptr;
      features["window"] = cx.win != nullptr;
      features["renderer"] = cx.rdr != nullptr;
      out["features"] = std::move(features);
      return out;
    }

    json h_shutdown(dispatch_context& cx, const json&) {
      if (cx.win)
        cx.win->close();
      return json{json::object()};
    }

    // ---- Runtime.* ---------------------------------------------------------
    // The actual implementation is registered by fxe_js (via
    // set_runtime_handlers) so that fxe_debug doesn't need a build-time link
    // to V8. When fxe_js isn't linked, these stubs return a typed error.
    json h_runtime_evaluate(dispatch_context& cx, const json& params) {
      if (!cx.host || !runtime_storage().evaluate)
        runtime_unavailable();
      auto out = runtime_storage().evaluate(cx, params);
      if (cx.srv && out.is_object()) {
        if (auto details = out.find("exceptionDetails");
            details != out.end() && details->is_object()) {
          using clock = std::chrono::system_clock;
          const double timestamp =
              std::chrono::duration<double>(clock::now().time_since_epoch()).count();
          json event_params{json::object()};
          event_params["timestamp"] = timestamp;
          event_params["exceptionDetails"] = *details;
          cx.srv->emit_event("Runtime.exceptionThrown", std::move(event_params));
        }
      }
      // Preserve the existing fxe NDJSON `{value}` shape while also exposing
      // the CDP Runtime.evaluate `{result:{type,value}}` shape DevTools expects.
      if (out.is_object() && out.find("result") == out.end() && out.find("value") != out.end()) {
        json remote{json::object()};
        const auto& v = out.at("value");
        if (v.is_null())
          remote["type"] = "undefined";
        else if (v.is_boolean())
          remote["type"] = "boolean";
        else if (v.is_number())
          remote["type"] = "number";
        else if (v.is_string())
          remote["type"] = "string";
        else
          remote["type"] = "object";
        remote["value"] = v;
        out["result"] = std::move(remote);
      }
      return out;
    }
    json h_runtime_get_globals(dispatch_context& cx, const json& params) {
      if (!cx.host || !runtime_storage().get_globals)
        runtime_unavailable();
      return runtime_storage().get_globals(cx, params);
    }

    json h_runtime_fire_hmr(dispatch_context& cx, const json& params) {
      if (!cx.host || !runtime_storage().fire_hmr)
        runtime_unavailable();
      return runtime_storage().fire_hmr(cx, params);
    }

    json h_runtime_invalidate_module(dispatch_context& cx, const json& params) {
      if (!cx.host || !runtime_storage().invalidate_module)
        runtime_unavailable();
      return runtime_storage().invalidate_module(cx, params);
    }

    json h_runtime_reimport_module(dispatch_context& cx, const json& params) {
      if (!cx.host || !runtime_storage().reimport_module)
        runtime_unavailable();
      return runtime_storage().reimport_module(cx, params);
    }

    json h_reconciler_snapshot(dispatch_context& cx, const json& params) {
      if (!cx.host || !runtime_storage().reconciler_snapshot)
        runtime_unavailable();
      return runtime_storage().reconciler_snapshot(cx, params);
    }

    // ---- CDP compatibility -------------------------------------------------
    // These methods let CDP-shaped clients probe the engine over both debug
    // transports. They do not advertise breakpoint, stepping, CPU profiling, or
    // source-map support unless a real provider is wired in.
    json h_schema_get_domains(dispatch_context&, const json&) {
      json domains{json::array()};
      domains.push_back({{"name", "Schema"}, {"version", "1.3"}});
      domains.push_back({{"name", "Runtime"}, {"version", "1.3"}});
      domains.push_back({{"name", "Debugger"}, {"version", "1.3"}});
      domains.push_back({{"name", "Reconciler"}, {"version", "1.3"}});
      domains.push_back({{"name", "Network"}, {"version", "1.3"}});
#if defined(FXE_DEBUG_HAS_WEBAUTHN)
      domains.push_back({{"name", "WebAuthn"}, {"version", "1.3"}});
#endif
      if (profiler_available())
        domains.push_back({{"name", "Profiler"}, {"version", "1.3"}});
      if (heap_profiler_available())
        domains.push_back({{"name", "HeapProfiler"}, {"version", "1.3"}});
      json out{json::object()};
      out["domains"] = std::move(domains);
      return out;
    }

    json h_runtime_enable(dispatch_context&, const json&) {
      return json{json::object()};
    }

    json h_network_enable(dispatch_context&, const json&) {
      g_network_enabled.store(true, std::memory_order_release);
      return json{json::object()};
    }

    json h_network_disable(dispatch_context&, const json&) {
      g_network_enabled.store(false, std::memory_order_release);
      return json{json::object()};
    }

    json h_debugger_enable(dispatch_context&, const json&) {
      return json{json::object()};
    }

    [[noreturn]] void profiler_not_implemented() {
      profiler_unavailable();
    }
    json h_profiler_enable(dispatch_context& cx, const json& params) {
      auto& h = profiler_storage();
      if (!cx.host || !h.enable)
        profiler_not_implemented();
      return h.enable(cx, params);
    }
    json h_profiler_disable(dispatch_context& cx, const json& params) {
      auto& h = profiler_storage();
      if (!cx.host || !h.disable)
        profiler_not_implemented();
      return h.disable(cx, params);
    }

    json h_profiler_start(dispatch_context& cx, const json& params) {
      auto& h = profiler_storage();
      if (!cx.host || !h.start)
        profiler_not_implemented();
      return h.start(cx, params);
    }

    json h_profiler_stop(dispatch_context& cx, const json& params) {
      auto& h = profiler_storage();
      if (!cx.host || !h.stop)
        profiler_not_implemented();
      return h.stop(cx, params);
    }

    json h_profiler_start_precise_coverage(dispatch_context& cx, const json& params) {
      auto& h = profiler_storage();
      if (!cx.host || !h.start_precise_coverage)
        profiler_not_implemented();
      return h.start_precise_coverage(cx, params);
    }

    json h_profiler_take_precise_coverage(dispatch_context& cx, const json& params) {
      auto& h = profiler_storage();
      if (!cx.host || !h.take_precise_coverage)
        profiler_not_implemented();
      return h.take_precise_coverage(cx, params);
    }

    [[noreturn]] void heap_profiler_unavailable() {
      throw dispatch_error{err_code::service_unavailable,
                           "HeapProfiler unavailable: V8 host not attached",
                           "heap_profiler_unavailable"};
    }

    json h_heap_profiler_enable(dispatch_context& cx, const json& params) {
      auto& h = heap_profiler_storage();
      if (!cx.host || !h.enable)
        heap_profiler_unavailable();
      return h.enable(cx, params);
    }

    json h_heap_profiler_disable(dispatch_context& cx, const json& params) {
      auto& h = heap_profiler_storage();
      if (!cx.host || !h.disable)
        heap_profiler_unavailable();
      return h.disable(cx, params);
    }

    json h_heap_profiler_take_heap_snapshot(dispatch_context& cx, const json& params) {
      auto& h = heap_profiler_storage();
      if (!cx.host || !h.take_heap_snapshot)
        heap_profiler_unavailable();
      return h.take_heap_snapshot(cx, params);
    }

    json h_heap_profiler_collect_garbage(dispatch_context& cx, const json& params) {
      auto& h = heap_profiler_storage();
      if (!cx.host || !h.collect_garbage)
        heap_profiler_unavailable();
      return h.collect_garbage(cx, params);
    }

    // ---- Console.* ---------------------------------------------------------

    json h_console_enable(dispatch_context& cx, const json&) {
      detail::server_set_console_enabled(cx.srv, cx.session, true);
      return json{json::object()};
    }
    json h_console_disable(dispatch_context& cx, const json&) {
      detail::server_set_console_enabled(cx.srv, cx.session, false);
      return json{json::object()};
    }

    // ---- Page.* ------------------------------------------------------------

    json h_page_fb_size(dispatch_context& cx, const json& params) {
      auto* w = resolve_window(cx, params);
      auto sz = w->framebuffer_size();
      json out{json::object()};
      out["width"] = static_cast<double>(sz.x);
      out["height"] = static_cast<double>(sz.y);
      return out;
    }
    json h_page_request_redraw(dispatch_context& cx, const json& params) {
      auto* w = resolve_window(cx, params);
      w->post_redraw();
      return json{json::object()};
    }

    json h_page_windows(dispatch_context& cx, const json&) {
      json::array_t out;
      if (cx.host) {
        auto wins = cx.host->windows();
        for (usize i = 0; i < wins.size(); ++i) {
          auto sz = wins[i]->framebuffer_size();
          json v{json::object()};
          v["id"] = static_cast<double>(i);
          v["width"] = static_cast<double>(sz.x);
          v["height"] = static_cast<double>(sz.y);
          out.push_back(std::move(v));
        }
      }
      json reply{json::object()};
      reply["windows"] = std::move(out);
      return reply;
    }

    json h_page_screenshot(dispatch_context& cx, const json& params) {
      auto* rdr = resolve_renderer(cx, params);
      // Lazy-mode mounts only render when the window has a pending redraw
      // request. The first capture_frame() call below arms the readback;
      // without a posted redraw, no end_frame() will fire and the second
      // call (after the SDK's retry sleep) finds nothing to capture. Force
      // a redraw here so the OS event loop drives a render between calls.
      rdr->get_window().post_redraw();

      // ---- parse params ----
      std::string format =
          params.is_object() && params.contains("format") && params["format"].is_string()
              ? params["format"].get<std::string>()
              : "png";
      std::transform(format.begin(), format.end(), format.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (format != "png" && format != "jpeg" && format != "jpg")
        invalid_params("format must be 'png' or 'jpeg'");
      if (format == "jpg")
        format = "jpeg";

      int quality = 90;
      if (params.is_object()) {
        if (auto it = params.find("quality"); it != params.end() && it->is_number())
          quality = std::clamp(static_cast<int>(it->get<double>()), 1, 100);
      }

      // Crop rect (in framebuffer pixels). Zero w/h means "to the edge".
      double clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
      if (auto it = params.find("clip"); it != params.end() && it->is_object()) {
        const auto& clip = *it;
        clip_x = clip.value("x", 0.0);
        clip_y = clip.value("y", 0.0);
        clip_w = clip.value("width", 0.0);
        clip_h = clip.value("height", 0.0);
      }
      if (clip_x < 0 || clip_y < 0 || clip_w < 0 || clip_h < 0)
        invalid_params("clip rect must be non-negative");

      // Output sizing: scale (multiplicative), and optional max{Width,Height}
      // bounding box. `scale` of 0 is treated as 1.0.
      double scale = 1.0;
      if (auto it = params.find("scale"); it != params.end() && it->is_number())
        scale = it->get<double>();
      if (scale <= 0.0)
        scale = 1.0;
      if (scale > 4.0)
        scale = 4.0;
      double max_w = 0.0;
      if (auto it = params.find("maxWidth"); it != params.end() && it->is_number())
        max_w = it->get<double>();
      double max_h = 0.0;
      if (auto it = params.find("maxHeight"); it != params.end() && it->is_number())
        max_h = it->get<double>();

      bool omit_data = false;
      if (auto it = params.find("omitData"); it != params.end() && it->is_boolean())
        omit_data = it->get<bool>();
      std::string save_path;
      if (auto it = params.find("path"); it != params.end() && it->is_string())
        save_path = it->get<std::string>();

      // ---- snapshot ----
      auto cap = rdr->capture_frame();
      if (!cap.ok)
        throw dispatch_error{err_code::capture_failed,
                             cap.error.empty() ? "capture_frame failed" : cap.error, ""};
      if (cap.rgba.size() < static_cast<usize>(cap.width) * cap.height * 4u)
        throw dispatch_error{err_code::capture_failed, "capture rgba buffer truncated", ""};

      // ---- crop + scale ----
      u32 cx_ = static_cast<u32>(clip_x);
      u32 cy_ = static_cast<u32>(clip_y);
      u32 cw_ = static_cast<u32>(clip_w);
      u32 ch_ = static_cast<u32>(clip_h);
      if (cx_ >= cap.width || cy_ >= cap.height)
        invalid_params("clip origin outside framebuffer");
      if (cw_ == 0)
        cw_ = cap.width - cx_;
      if (ch_ == 0)
        ch_ = cap.height - cy_;
      cw_ = std::min(cw_, cap.width - cx_);
      ch_ = std::min(ch_, cap.height - cy_);

      double out_w_d = static_cast<double>(cw_) * scale;
      double out_h_d = static_cast<double>(ch_) * scale;
      if (max_w > 0.0 && out_w_d > max_w) {
        double k = max_w / out_w_d;
        out_w_d *= k;
        out_h_d *= k;
      }
      if (max_h > 0.0 && out_h_d > max_h) {
        double k = max_h / out_h_d;
        out_w_d *= k;
        out_h_d *= k;
      }
      u32 out_w = std::max<u32>(1u, static_cast<u32>(out_w_d + 0.5));
      u32 out_h = std::max<u32>(1u, static_cast<u32>(out_h_d + 0.5));

      const u8* pixels = cap.rgba.data();
      const u32 src_stride = cap.width * 4u;
      std::vector<u8> processed;
      std::string screenshot_err;
      bool needs_resample =
          (cw_ != cap.width) || (ch_ != cap.height) || (out_w != cw_) || (out_h != ch_);
      if (needs_resample) {
        if (!crop_resize_rgba8(pixels, cap.width, cap.height, src_stride, cx_, cy_, cw_, ch_, out_w,
                               out_h, processed, &screenshot_err))
          throw dispatch_error{err_code::capture_failed,
                               screenshot_err.empty() ? "crop/resize failed" : screenshot_err, ""};
        pixels = processed.data();
      } else {
        out_w = cap.width;
        out_h = cap.height;
      }
      const u32 out_stride = out_w * 4u;

      // ---- encode ----
      std::string encoded;
      if (format == "png")
        encoded = encode_png_rgba8(pixels, out_w, out_h, out_stride, &screenshot_err);
      else
        encoded = encode_jpeg_rgba8(pixels, out_w, out_h, out_stride, quality, &screenshot_err);
      if (encoded.empty())
        throw dispatch_error{err_code::capture_failed,
                             screenshot_err.empty() ? format + " encode failed" : screenshot_err,
                             ""};

      // ---- optional server-side save ----
      bool saved = false;
      std::string save_err;
      if (!save_path.empty()) {
        if (auto* f = std::fopen(save_path.c_str(), "wb")) {
          usize n = std::fwrite(encoded.data(), 1, encoded.size(), f);
          std::fclose(f);
          saved = (n == encoded.size());
          if (!saved)
            save_err = "short write";
        } else {
          save_err = std::strerror(errno);
          if (save_err.empty())
            save_err = "fopen failed";
        }
      }

      // ---- response ----
      json out{json::object()};
      out["format"] = format;
      out["width"] = static_cast<double>(out_w);
      out["height"] = static_cast<double>(out_h);
      out["sourceWidth"] = static_cast<double>(cap.width);
      out["sourceHeight"] = static_cast<double>(cap.height);
      out["byteSize"] = static_cast<double>(encoded.size());
      if (!omit_data || save_path.empty()) {
        const usize b64_len =
            sodium_base64_ENCODED_LEN(encoded.size(), sodium_base64_VARIANT_ORIGINAL) - 1u;
        std::string b64(b64_len, '\0');
        sodium_bin2base64(b64.data(), b64_len + 1u,
                          reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(),
                          sodium_base64_VARIANT_ORIGINAL);
        out["dataBase64"] = std::move(b64);
      }
      if (!save_path.empty()) {
        out["path"] = save_path;
        out["saved"] = saved;
        if (!saved)
          out["saveError"] = save_err;
      }
      return out;
    }

    // ---- Input.* -----------------------------------------------------------

    input_event parse_mouse_event(const json& params) {
      if (!params.is_object())
        invalid_params("expected object params");
      input_event ev{};
      std::string type;
      if (auto it = params.find("type"); it != params.end() && it->is_string())
        type = it->get<std::string>();
      if (type == "move")
        ev.kind = input_event::kind_t::mouse_move;
      else if (type == "down")
        ev.kind = input_event::kind_t::mouse_button_down;
      else if (type == "up")
        ev.kind = input_event::kind_t::mouse_button_up;
      else if (type == "wheel")
        ev.kind = input_event::kind_t::mouse_wheel;
      else
        invalid_params("invalid mouse event type");
      if (auto it = params.find("x"); it != params.end() && it->is_number())
        ev.x = it->get<double>();
      if (auto it = params.find("y"); it != params.end() && it->is_number())
        ev.y = it->get<double>();
      if (auto it = params.find("dx"); it != params.end() && it->is_number())
        ev.dx = it->get<double>();
      if (auto it = params.find("dy"); it != params.end() && it->is_number())
        ev.dy = it->get<double>();
      std::string btn = "left";
      if (auto it = params.find("button"); it != params.end() && it->is_string())
        btn = it->get<std::string>();
      if (btn == "left")
        ev.button = 0;
      else if (btn == "right")
        ev.button = 1;
      else if (btn == "middle")
        ev.button = 2;
      else if (auto it = params.find("button"); it != params.end() && it->is_number())
        ev.button = static_cast<int>(it->get<double>());
      if (auto it = params.find("modifiers"); it != params.end() && it->is_number())
        ev.modifiers = static_cast<int>(it->get<double>());
      return ev;
    }

    input_event parse_key_event(const json& params) {
      if (!params.is_object())
        invalid_params("expected object params");
      input_event ev{};
      std::string type;
      if (auto it = params.find("type"); it != params.end() && it->is_string())
        type = it->get<std::string>();
      if (type == "down")
        ev.kind = input_event::kind_t::key_down;
      else if (type == "up")
        ev.kind = input_event::kind_t::key_up;
      else if (type == "char")
        ev.kind = input_event::kind_t::key_char;
      else
        invalid_params("invalid key event type");
      if (auto it = params.find("modifiers"); it != params.end() && it->is_number())
        ev.modifiers = static_cast<int>(it->get<double>());
      if (auto it = params.find("codepoint"); it != params.end() && it->is_number())
        ev.codepoint = static_cast<unsigned>(it->get<double>());
      if (auto it = params.find("key"); it != params.end() && it->is_number())
        ev.key = static_cast<int>(it->get<double>());
      if (auto it = params.find("scancode"); it != params.end() && it->is_number())
        ev.scancode = static_cast<int>(it->get<double>());
      // String "key" e.g. "a", "Enter", "Escape" — best-effort GLFW mapping.
      if (auto it = params.find("key"); it != params.end() && it->is_string()) {
        const auto& s = it->get_ref<const std::string&>();
        if (s.size() == 1 && s[0] >= 0x20 && s[0] <= 0x7e) {
          char c = s[0];
          if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
          ev.key = c;
          if (ev.kind == input_event::kind_t::key_char)
            ev.codepoint = static_cast<unsigned>(s[0]);
        } else if (s == "Enter")
          ev.key = 257;
        else if (s == "Escape")
          ev.key = 256;
        else if (s == "Tab")
          ev.key = 258;
        else if (s == "Backspace")
          ev.key = 259;
        else if (s == "ArrowLeft")
          ev.key = 263;
        else if (s == "ArrowRight")
          ev.key = 262;
        else if (s == "ArrowDown")
          ev.key = 264;
        else if (s == "ArrowUp")
          ev.key = 265;
        else if (s == "Space")
          ev.key = 32;
      }
      return ev;
    }

    json h_input_mouse(dispatch_context& cx, const json& params) {
      auto* w = resolve_window(cx, params);
      auto ev = parse_mouse_event(params);
      w->inject(ev);
      return json{json::object()};
    }

    json h_input_key(dispatch_context& cx, const json& params) {
      auto* w = resolve_window(cx, params);
      auto ev = parse_key_event(params);
      w->inject(ev);
      return json{json::object()};
    }

    json h_window_close(dispatch_context& cx, const json& params) {
      auto* w = resolve_window(cx, params);
      w->close();
      w->post_redraw(); // wake the loop so close takes effect promptly
      return json{json::object()};
    }

    // Window.pollInput drains the buffered events that JS side hasn't pulled.
    // Useful for tests when no JS handler is installed yet.
    json h_window_poll_input(dispatch_context& cx, const json& params) {
      auto* w = resolve_window(cx, params);
      auto evs = w->drain_input_events();
      json::array_t a;
      for (auto& e : evs) {
        json v{json::object()};
        const char* kind = "unknown";
        switch (e.kind) {
        case input_event::kind_t::mouse_move:
          kind = "move";
          break;
        case input_event::kind_t::mouse_button_down:
          kind = "down";
          break;
        case input_event::kind_t::mouse_button_up:
          kind = "up";
          break;
        case input_event::kind_t::mouse_wheel:
          kind = "wheel";
          break;
        case input_event::kind_t::key_down:
          kind = "keydown";
          break;
        case input_event::kind_t::key_up:
          kind = "keyup";
          break;
        case input_event::kind_t::key_char:
          kind = "char";
          break;
        default:
          break;
        }
        v["type"] = std::string(kind);
        v["x"] = e.x;
        v["y"] = e.y;
        v["dx"] = e.dx;
        v["dy"] = e.dy;
        v["button"] = static_cast<double>(e.button);
        v["key"] = static_cast<double>(e.key);
        v["scancode"] = static_cast<double>(e.scancode);
        v["modifiers"] = static_cast<double>(e.modifiers);
        v["codepoint"] = static_cast<double>(e.codepoint);
        a.push_back(std::move(v));
      }
      json out{json::object()};
      out["events"] = std::move(a);
      return out;
    }

    // ---- Debugger.* --------------------------------------------------------

    json h_debugger_pause(dispatch_context& cx, const json&) {
      detail::server_set_pause(cx.srv, true, false);
      return json{json::object()};
    }
    json h_debugger_resume(dispatch_context& cx, const json&) {
      detail::server_set_pause(cx.srv, false, false);
      return json{json::object()};
    }
    json h_debugger_step(dispatch_context& cx, const json&) {
      detail::server_set_pause(cx.srv, false, true);
      return json{json::object()};
    }

    // ---- Performance.* -----------------------------------------------------
    // The actual snapshot is supplied by fxe_js so fxe_debug doesn't link V8.
    // When no host is attached we still return a well-formed unavailable
    // payload so clients can probe the engine cheaply and branch explicitly.
    json h_performance_timeline(dispatch_context& cx, const json& params) {
      if (cx.host) {
        if (auto* fn = runtime_storage().performance_snapshot)
          return fn(cx, params);
      }
      json out{json::object()};
      out["marks"] = json::object();
      out["render"] = json::object();
      out["available"] = false;
      return out;
    }

    // ---- *.subscribe / unsubscribe ----------------------------------------
    // Toggle event channels. The engine pushes events through
    // emit_event_if_attached() unconditionally; the server filters here.
    json h_window_subscribe(dispatch_context& cx, const json&) {
      detail::server_set_channel_enabled(cx.srv, cx.session, event_channel::window, true);
      return json{json::object()};
    }
    json h_window_unsubscribe(dispatch_context& cx, const json&) {
      detail::server_set_channel_enabled(cx.srv, cx.session, event_channel::window, false);
      return json{json::object()};
    }
    json h_fetch_subscribe(dispatch_context& cx, const json&) {
      detail::server_set_channel_enabled(cx.srv, cx.session, event_channel::fetch, true);
      return json{json::object()};
    }
    json h_fetch_unsubscribe(dispatch_context& cx, const json&) {
      detail::server_set_channel_enabled(cx.srv, cx.session, event_channel::fetch, false);
      return json{json::object()};
    }
    json h_fs_subscribe(dispatch_context& cx, const json&) {
      detail::server_set_channel_enabled(cx.srv, cx.session, event_channel::fs, true);
      return json{json::object()};
    }
    json h_fs_unsubscribe(dispatch_context& cx, const json&) {
      detail::server_set_channel_enabled(cx.srv, cx.session, event_channel::fs, false);
      return json{json::object()};
    }

    const std::unordered_map<std::string_view, handler_fn>& table() {
      static const std::unordered_map<std::string_view, handler_fn> t = [] {
        std::unordered_map<std::string_view, handler_fn> handlers = {
            {"System.handshake", &h_handshake},
            {"System.shutdown", &h_shutdown},
            {"Runtime.evaluate", &h_runtime_evaluate},
            {"Runtime.getGlobals", &h_runtime_get_globals},
            {"Runtime.fireHmr", &h_runtime_fire_hmr},
            {"Runtime.invalidateModule", &h_runtime_invalidate_module},
            {"Runtime.reimportModule", &h_runtime_reimport_module},
            {"Schema.getDomains", &h_schema_get_domains},
            {"Runtime.enable", &h_runtime_enable},
            {"Network.enable", &h_network_enable},
            {"Network.disable", &h_network_disable},
            {"Console.enable", &h_console_enable},
            {"Console.disable", &h_console_disable},
            {"Page.framebufferSize", &h_page_fb_size},
            {"Page.requestRedraw", &h_page_request_redraw},
            {"Page.screenshot", &h_page_screenshot},
            {"Page.windows", &h_page_windows},
            {"Input.dispatchMouseEvent", &h_input_mouse},
            {"Input.dispatchKeyEvent", &h_input_key},
            {"Window.close", &h_window_close},
            {"Window.pollInput", &h_window_poll_input},
            {"Debugger.pause", &h_debugger_pause},
            {"Debugger.resume", &h_debugger_resume},
            {"Debugger.step", &h_debugger_step},
            {"Debugger.enable", &h_debugger_enable},
            {"Reconciler.snapshot", &h_reconciler_snapshot},
            {"Profiler.enable", &h_profiler_enable},
            {"Profiler.disable", &h_profiler_disable},
            {"Profiler.start", &h_profiler_start},
            {"Profiler.stop", &h_profiler_stop},
            {"Profiler.startPreciseCoverage", &h_profiler_start_precise_coverage},
            {"Profiler.takePreciseCoverage", &h_profiler_take_precise_coverage},
            {"HeapProfiler.enable", &h_heap_profiler_enable},
            {"HeapProfiler.disable", &h_heap_profiler_disable},
            {"HeapProfiler.takeHeapSnapshot", &h_heap_profiler_take_heap_snapshot},
            {"HeapProfiler.collectGarbage", &h_heap_profiler_collect_garbage},
            {"Performance.timeline", &h_performance_timeline},
            {"Window.subscribe", &h_window_subscribe},
            {"Window.unsubscribe", &h_window_unsubscribe},
            {"Fetch.subscribe", &h_fetch_subscribe},
            {"Fetch.unsubscribe", &h_fetch_unsubscribe},
            {"Fs.subscribe", &h_fs_subscribe},
            {"Fs.unsubscribe", &h_fs_unsubscribe},
        };
#if defined(FXE_DEBUG_HAS_WEBAUTHN)
        webauthn::debug::register_webauthn_dispatch_handlers();
        for (const auto& method : webauthn::debug::handler_table())
          handlers.emplace(method.name, method.fn);
#endif
        return handlers;
      }();
      return t;
    }
  } // namespace

  bool method_exists(std::string_view method) {
    return table().find(method) != table().end();
  }
  // Lives at namespace scope so both the in-table handlers (anon namespace)
  // and set_runtime_handlers / get_runtime_handlers can refer to the same
  // storage without grappling with anon-namespace mangling.
  runtime_handlers& runtime_storage() {
    static runtime_handlers h{};
    return h;
  }
  profiler_handlers& profiler_storage() {
    static profiler_handlers h{};
    return h;
  }
  heap_profiler_handlers& heap_profiler_storage() {
    static heap_profiler_handlers h{};
    return h;
  }
  void set_profiler_handlers(profiler_handlers h) noexcept {
    profiler_storage() = std::move(h);
  }
  profiler_handlers get_profiler_handlers() {
    return profiler_storage();
  }
  void set_heap_profiler_handlers(heap_profiler_handlers h) noexcept {
    heap_profiler_storage() = std::move(h);
  }
  heap_profiler_handlers get_heap_profiler_handlers() {
    return heap_profiler_storage();
  }
  void set_runtime_handlers(runtime_handlers h) noexcept {
    runtime_storage() = h;
  }
  runtime_handlers get_runtime_handlers() noexcept {
    return runtime_storage();
  }

  namespace network {
    bool enabled() {
      return g_network_enabled.load(std::memory_order_acquire);
    }

    std::string fresh_request_id() {
      return std::to_string(g_network_request_counter.fetch_add(1, std::memory_order_acq_rel) + 1u);
    }

    void emit_request_will_be_sent(std::string_view req_id, std::string_view url,
                                   std::string_view method,
                                   const std::vector<std::pair<std::string, std::string>>& headers,
                                   std::optional<std::string_view> post_data,
                                   std::string_view type) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["wallTime"] = network_wall_time_now();
      params["initiator"] = {{"type", "script"}};
      json request{
          {"url", std::string(url)},
          {"method", std::string(method)},
          {"headers", network_headers_to_json(headers)},
      };
      if (auto body = network_post_data(post_data); body.has_value())
        request["postData"] = std::move(*body);
      params["request"] = std::move(request);
      if (!type.empty())
        params["type"] = std::string(type);
      emit_network_event("Network.requestWillBeSent", std::move(params));
    }

    void emit_response_received(std::string_view req_id, std::string_view url, int status,
                                std::string_view status_text,
                                const std::vector<std::pair<std::string, std::string>>& headers,
                                std::string_view mime_type, std::string_view type,
                                i64 encoded_length) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["type"] = std::string(type);
      params["response"] = {
          {"url", std::string(url)},
          {"status", status},
          {"statusText", std::string(status_text)},
          {"headers", network_headers_to_json(headers)},
          {"mimeType", std::string(mime_type)},
          {"encodedDataLength", encoded_length},
      };
      emit_network_event("Network.responseReceived", std::move(params));
    }

    void emit_loading_finished(std::string_view req_id, i64 encoded_length) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["encodedDataLength"] = encoded_length;
      emit_network_event("Network.loadingFinished", std::move(params));
    }

    void emit_loading_failed(std::string_view req_id, std::string_view type,
                             std::string_view error_text, bool canceled) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["type"] = std::string(type);
      params["errorText"] = std::string(error_text);
      if (canceled)
        params["canceled"] = true;
      emit_network_event("Network.loadingFailed", std::move(params));
    }

    void emit_ws_created(std::string_view req_id, std::string_view url) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["url"] = std::string(url);
      params["initiator"] = {{"type", "script"}};
      emit_network_event("Network.webSocketCreated", std::move(params));
    }

    void
    emit_ws_handshake_request(std::string_view req_id,
                              const std::vector<std::pair<std::string, std::string>>& headers) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["wallTime"] = network_wall_time_now();
      params["request"] = {{"headers", network_headers_to_json(headers)}};
      emit_network_event("Network.webSocketWillSendHandshakeRequest", std::move(params));
    }

    void
    emit_ws_handshake_response(std::string_view req_id, int status, std::string_view status_text,
                               const std::vector<std::pair<std::string, std::string>>& headers) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["response"] = {
          {"status", status},
          {"statusText", std::string(status_text)},
          {"headers", network_headers_to_json(headers)},
      };
      emit_network_event("Network.webSocketHandshakeResponseReceived", std::move(params));
    }

    void emit_ws_frame_sent(std::string_view req_id, int opcode, std::string_view payload_b64) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["response"] = {
          {"opcode", opcode}, {"mask", true}, {"payloadData", std::string(payload_b64)}};
      emit_network_event("Network.webSocketFrameSent", std::move(params));
    }

    void emit_ws_frame_received(std::string_view req_id, int opcode, std::string_view payload_b64) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      params["response"] = {
          {"opcode", opcode},
          {"mask", false},
          {"payloadData", std::string(payload_b64)},
      };
      emit_network_event("Network.webSocketFrameReceived", std::move(params));
    }

    void emit_ws_closed(std::string_view req_id) {
      if (!enabled())
        return;
      json params{json::object()};
      params["requestId"] = std::string(req_id);
      params["timestamp"] = network_timestamp_now();
      emit_network_event("Network.webSocketClosed", std::move(params));
    }
  } // namespace network

  json dispatch(dispatch_context& cx, std::string_view method, const json& params) {
    auto it = table().find(method);
    if (it == table().end())
      throw dispatch_error{err_code::method_not_found, std::string{method}, ""};
    return it->second(cx, params);
  }
} // namespace fxe::debug
