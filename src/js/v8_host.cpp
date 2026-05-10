// V8 platform / isolate / context lifecycle and the script entry points.
//
// Single isolate per host instance, single thread of execution. Every entry
// point that touches V8 state acquires the canonical Isolate::Scope +
// HandleScope + Context::Scope wrappers. Top-level await is supported via
// the module path: Module::Evaluate returns a Promise that we drain by
// pumping microtasks.

#include "../runtime/bundle_loader.hpp"
#include "../runtime/uv_loop.hpp"
#include "../runtime/v8/fxe_native.hpp"
#include "../runtime/v8/native/async_hooks.hpp"
#include "../runtime/v8/native/inspector.hpp"
#include "../runtime/v8/native/v8_module.hpp"
#include "../runtime/v8/native/vm.hpp"
#include "../runtime/v8/node_compat.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/log.hpp>
#include <fxe/types.hpp>
#include <fxe/typescript.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/v8_literals.hpp>
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
#include "../runtime/v8/native/zlib.hpp"
#endif
#include "bind_app.hpp"
#include "bind_audio.hpp"
#include "bind_blob.hpp"
#include "bind_crash.hpp"
#include "bind_dialog.hpp"
#include "bind_fetch.hpp"
#include "bind_font.hpp"
#include "bind_fs.hpp"
#include "bind_global_shortcut.hpp"
#include "bind_image.hpp"
#include "bind_indexed_db.hpp"
#include "bind_ipc.hpp"
#include "bind_layout.hpp"
#include "bind_markdown.hpp"
#include "bind_menu.hpp"
#include "bind_net.hpp"
#include "bind_notification.hpp"
#include "bind_offscreen.hpp"
#include "bind_os.hpp"
#include "bind_path.hpp"
#include "bind_performance.hpp"
#include "bind_power.hpp"
#include "bind_print.hpp"
#include "bind_process.hpp"
#include "bind_render_stats.hpp"
#include "bind_shell.hpp"
#include "bind_spritesheet.hpp"
#include "bind_sqlite.hpp"
#include "bind_storage.hpp"
#include "bind_text_document.hpp"
#include "bind_timers.hpp"
#include "bind_tray.hpp"
#include "bind_url.hpp"
#include "bind_wasm.hpp"
#include "bind_webauthn.hpp"
#include "bind_websocket.hpp"
#include "os/os.hpp"
#include "source_map.hpp"
#include "v8_code_cache.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <libplatform/libplatform.h>
#include <v8-profiler.h>
#include <v8.h>

namespace fxe::runtime {
  void uninstall_native_async_hooks(v8::Isolate* iso);
}
namespace fxe::js {
  void dispatch_console_sink(v8::Isolate* iso, const char* level, std::string_view text);
  void install_hmr_native_bindings(v8::Isolate* iso, v8::Local<v8::Context> ctx);
  std::vector<std::string> invalidate_module_for_isolate(v8::Isolate* iso, std::string_view path,
                                                         std::string& error);
  v8::MaybeLocal<v8::Value> reimport_module_for_isolate(v8::Isolate* iso,
                                                        v8::Local<v8::Context> ctx,
                                                        std::string_view path, std::string& error);

  namespace {
    std::mutex& template_resetter_mutex() {
      static std::mutex m;
      return m;
    }
    std::vector<template_reset_fn>& template_resetter_list() {
      static std::vector<template_reset_fn> v;
      return v;
    }
    std::unordered_set<template_reset_fn>& template_resetter_set() {
      static std::unordered_set<template_reset_fn> s;
      return s;
    }

    // Total physical RAM in bytes; 0 if unavailable. Used to size V8's
    // ResourceConstraints. fxe::runtime exposes a similar helper to JS via
    // os.totalmem, but we don't want a header dep here just for a sysctl.
    uint64_t physical_memory_bytes() {
#if defined(__APPLE__)
      uint64_t mem = 0;
      size_t size = sizeof(mem);
      return sysctlbyname("hw.memsize", &mem, &size, nullptr, 0) == 0 ? mem : 0;
#elif defined(__linux__)
      struct sysinfo info{};
      if (sysinfo(&info) != 0)
        return 0;
      return static_cast<uint64_t>(info.totalram) * static_cast<uint64_t>(info.mem_unit);
#elif defined(_WIN32)
      MEMORYSTATUSEX status{};
      status.dwLength = sizeof(status);
      return GlobalMemoryStatusEx(&status) ? static_cast<uint64_t>(status.ullTotalPhys) : 0;
#else
      return 0;
#endif
    }

    // Per-isolate map of unhandled-rejected promises. Populated by
    // promise_reject_callback when V8 fires kPromiseRejectWithNoHandler so we
    // can pair up a later kPromiseHandlerAddedAfterReject and dispatch the
    // matching `rejectionHandled` event (Node semantics). Stored as a vector
    // of (promise, reason) pairs because identity hashing on v8::Promise is
    // not collision-free; linear search is fine for the realistic count of
    // outstanding rejections.
    struct pending_rejection {
      v8::Global<v8::Promise> promise;
      v8::Global<v8::Value> reason;
    };
    std::mutex g_pending_rejections_mu;
    std::unordered_map<v8::Isolate*, std::vector<pending_rejection>> g_pending_rejections;

    std::vector<pending_rejection>& pending_rejections_for(v8::Isolate* iso) {
      // Caller MUST hold g_pending_rejections_mu.
      return g_pending_rejections[iso];
    }

    // Visibility for unhandled promise rejections. Without this, a rejected
    // promise that is never `.catch()`-ed disappears silently. Routes through
    // process.emit('unhandledRejection', reason, promise) and
    // process.emit('rejectionHandled', promise) so user code can install
    // handlers exactly like Node.js. Falls back to the host's console sink
    // when no listener is registered.
    void promise_reject_callback(v8::PromiseRejectMessage msg) {
      auto* iso = v8::Isolate::GetCurrent();
      if (!iso)
        return;
      v8::HandleScope hs(iso);
      auto promise = msg.GetPromise();
      auto ctx = iso->GetCurrentContext();
      if (ctx.IsEmpty())
        return;
      switch (msg.GetEvent()) {
      case v8::kPromiseRejectWithNoHandler: {
        auto value = msg.GetValue();
        if (value.IsEmpty())
          value = v8::Undefined(iso);
        {
          std::lock_guard<std::mutex> lk(g_pending_rejections_mu);
          auto& pending = pending_rejections_for(iso);
          pending.push_back(
              {v8::Global<v8::Promise>(iso, promise), v8::Global<v8::Value>(iso, value)});
        }
        v8::Local<v8::Value> argv[2] = {value, promise};
        const int invoked = emit_process_event(iso, ctx, "unhandledRejection", 2, argv);
        if (invoked == 0) {
          // No listener — preserve the previous behavior of warning through
          // the console sink so dropped rejections aren't silently lost.
          v8::String::Utf8Value utf8(iso, value);
          const char* text = (*utf8 != nullptr) ? *utf8 : "<non-stringifiable rejection>";
          char buf[1024];
          const int n = std::snprintf(buf, sizeof(buf), "Unhandled promise rejection: %s", text);
          dispatch_console_sink(iso, "error",
                                std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
        }
        break;
      }
      case v8::kPromiseHandlerAddedAfterReject: {
        bool found = false;
        {
          std::lock_guard<std::mutex> lk(g_pending_rejections_mu);
          auto it = g_pending_rejections.find(iso);
          if (it != g_pending_rejections.end()) {
            auto& pending = it->second;
            for (auto p_it = pending.begin(); p_it != pending.end(); ++p_it) {
              if (p_it->promise.Get(iso) == promise) {
                p_it->promise.Reset();
                p_it->reason.Reset();
                pending.erase(p_it);
                found = true;
                break;
              }
            }
          }
        }
        if (found) {
          v8::Local<v8::Value> argv[1] = {promise};
          (void)emit_process_event(iso, ctx, "rejectionHandled", 1, argv);
        }
        break;
      }
      case v8::kPromiseRejectAfterResolved:
      case v8::kPromiseResolveAfterResolved:
        // V8 emits these for double-settle bugs. They're not part of the
        // Node unhandledRejection/rejectionHandled contract; ignore here so
        // user listeners aren't triggered for what is purely an internal
        // misuse signal.
        break;
      }
    }

    void clear_pending_rejections(v8::Isolate* iso) {
      std::lock_guard<std::mutex> lk(g_pending_rejections_mu);
      auto it = g_pending_rejections.find(iso);
      if (it == g_pending_rejections.end())
        return;
      for (auto& p : it->second) {
        p.promise.Reset();
        p.reason.Reset();
      }
      g_pending_rejections.erase(it);
    }
  } // namespace

  void register_template_resetter(template_reset_fn fn) {
    if (!fn)
      return;
    std::lock_guard<std::mutex> lk(template_resetter_mutex());
    auto& set = template_resetter_set();
    if (set.insert(fn).second)
      template_resetter_list().push_back(fn);
  }

  void run_template_resetters(v8::Isolate* iso) {
    std::vector<template_reset_fn> snapshot;
    {
      std::lock_guard<std::mutex> lk(template_resetter_mutex());
      snapshot = template_resetter_list();
    }
    for (auto fn : snapshot)
      fn(iso);
  }
  namespace {
    // Process-wide V8 platform. Created once via initialize(), destroyed in
    // shutdown(). All host instances share it.
    std::unique_ptr<v8::Platform> g_platform;
    std::once_flag g_init_flag;
    bool g_initialized = false;
    std::string g_argv0;
    std::string g_icudtl;

    constexpr u32 kIsolateSlotHostImpl = 0;
    constexpr u32 kIsolateSlotRuntimeId = 3;

    std::string to_std_string(v8::Isolate* iso, v8::Local<v8::Value> v) {
      if (v.IsEmpty())
        return {};
      v8::String::Utf8Value u(iso, v);
      if (*u)
        return std::string(*u, u.length());
      return {};
    }

    std::string console_arg_to_string(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                      v8::Local<v8::Value> value, bool prefer_error_stack) {
      if (!prefer_error_stack || value.IsEmpty() || !value->IsNativeError())
        return to_std_string(iso, value);

      v8::Local<v8::Object> error = value.As<v8::Object>();
      v8::Local<v8::Value> stack;
      if (error->Get(ctx, "stack"_v8(iso)).ToLocal(&stack) && stack->IsString()) {
        auto rendered = to_std_string(iso, stack);
        if (!rendered.empty())
          return rendered;
      }
      return to_std_string(iso, value);
    }
    void fill_exception_position(v8::Isolate* iso, v8::Local<v8::Context> ctx, v8::TryCatch& tc,
                                 exception_position& out) {
      v8::Local<v8::Message> message = tc.Message();
      if (message.IsEmpty())
        return;

      const int one_based_line = message->GetLineNumber(ctx).FromMaybe(0);
      const int zero_based_column = message->GetStartColumn(ctx).FromMaybe(0);
      std::string url = to_std_string(iso, message->GetScriptResourceName());
      if (url.empty())
        return;

      out.has_position = one_based_line > 0;
      out.url = url;
      out.line_number = std::max(one_based_line - 1, 0);
      out.column_number = std::max(zero_based_column, 0);

      if (auto mapped =
              source_maps().original_position(url, one_based_line, zero_based_column + 1)) {
        out.has_original_position = true;
        out.original_url = mapped->source;
        out.original_line_number = std::max(mapped->line - 1, 0);
        out.original_column_number = std::max(mapped->column - 1, 0);
      }
    }

    void console_callback_impl(const v8::FunctionCallbackInfo<v8::Value>& args, const char* level) {
      auto* iso = args.GetIsolate();
      v8::HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      const bool prefer_error_stack = std::string_view(level) == "error";
      std::string out;
      for (int i = 0; i < args.Length(); ++i) {
        if (i)
          out.push_back(' ');
        out += console_arg_to_string(iso, ctx, args[i], prefer_error_stack);
      }
      dispatch_console_sink(iso, level, out);
      std::printf("%s\n", out.c_str());
      std::fflush(stdout);
    }

    void console_log_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      console_callback_impl(args, "log");
    }

    void console_warn_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      console_callback_impl(args, "warn");
    }

    void console_error_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      console_callback_impl(args, "error");
    }

    void performance_now_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      using clock = std::chrono::steady_clock;
      static const auto start = clock::now();
      auto now = clock::now();
      double ms = std::chrono::duration<double, std::milli>(now - start).count();
      args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), ms));
    }

    // Lightweight detection: is this source an ES module? Looks for top-level
    // `import` or `export` lines. Conservative — false positives just route
    // through the slightly-stricter module compiler.
    bool looks_like_module(std::string_view src) {
      static const std::regex kEsm(R"(^[ \t]*(?:import|export)\b)",
                                   std::regex::ECMAScript | std::regex::multiline);
      return std::regex_search(src.begin(), src.end(), kEsm);
    }

    v8::Local<v8::String> str(v8::Isolate* iso, std::string_view s) {
      return v8::String::NewFromUtf8(iso, s.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string inline_source_map_url(std::string_view js) {
      constexpr std::string_view marker = "//# sourceMappingURL=";
      const auto pos = js.rfind(marker);
      if (pos == std::string_view::npos)
        return {};
      const auto start = pos + marker.size();
      const auto end = js.find_first_of("\r\n", start);
      return std::string(
          js.substr(start, (end == std::string_view::npos ? js.size() : end) - start));
    }

    v8::Local<v8::Value> source_map_url_value(v8::Isolate* iso, std::string_view js) {
      auto url = inline_source_map_url(js);
      if (url.empty())
        return v8::Local<v8::Value>();
      return str(iso, url);
    }

    bool install_fxe_hmr_runtime(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string* error);
  } // namespace
#ifndef FXE_SOURCE_DIR
#define FXE_SOURCE_DIR ""
#endif

  void initialize(std::string_view argv0, std::string_view icudtl_path) {
    std::call_once(g_init_flag, [&] {
      g_argv0 = std::string(argv0);
      g_icudtl = std::string(icudtl_path);

      if (!g_icudtl.empty()) {
        v8::V8::InitializeICU(g_icudtl.c_str());
      } else if (!g_argv0.empty()) {
        v8::V8::InitializeICUDefaultLocation(g_argv0.c_str());
        v8::V8::InitializeExternalStartupData(g_argv0.c_str());
      }
      // Flag block. Must precede V8::Initialize() — flags are baked at init.
      //   --expose_gc:           HeapProfiler.collectGarbage relies on
      //                          RequestGarbageCollectionForTesting, which V8
      //                          documents as valid only with this flag.
      //                          LowMemoryNotification is the fallback signal.
      //   --no-flush-bytecode:   long-running app revisits the same code paths
      //                          every frame; keep bytecode resident so the
      //                          existing code cache + ICs stay hot.
      //   --harmony-import-attributes: modern import-attribute syntax for TS.
      std::string flags = "--expose_gc"
                          " --no-flush-bytecode"
                          " --harmony-import-attributes";
      if (const char* extra = std::getenv("FXE_V8_FLAGS"); extra && *extra) {
        flags.push_back(' ');
        flags.append(extra);
      }
      v8::V8::SetFlagsFromString(flags.c_str());
      // Platform thread pool. Default (0) picks hardware_concurrency()-1, which
      // is wasteful on high-core machines (16+ idle threads contending with
      // render/audio). 4 workers cover compile + GC tasks comfortably.
      constexpr int kV8WorkerThreads = 4;
      g_platform = v8::platform::NewDefaultPlatform(kV8WorkerThreads,
                                                    v8::platform::IdleTaskSupport::kEnabled);
      v8::V8::InitializePlatform(g_platform.get());
      v8::V8::Initialize();
      g_initialized = true;
    });
  }

  void shutdown() noexcept {
    if (!g_initialized)
      return;
    // V8 14 enforces a strict initialisation/teardown order that is hard to
    // match in a static library: the IsolateGroup ref-count and the platform
    // disposer trip when thread_local FunctionTemplate persistents are still
    // alive. shutdown() is invoked once at process exit, so leaking V8 state
    // is the safe choice — the OS reclaims it.
    g_platform.release();
    g_initialized = false;
  }

  void idle_notification(double budget_seconds) {
    if (!g_initialized || !g_platform)
      return;
    auto* iso = v8::Isolate::GetCurrent();
    if (!iso)
      return;
    // Drains V8-posted idle tasks (incremental marking, sweeping) up to the
    // given budget. Requires IdleTaskSupport::kEnabled at platform creation.
    v8::platform::RunIdleTasks(g_platform.get(), iso, budget_seconds);
  }

  void notify_memory_pressure_moderate() {
    if (auto* iso = v8::Isolate::GetCurrent())
      iso->MemoryPressureNotification(v8::MemoryPressureLevel::kModerate);
  }

  void notify_memory_pressure_critical() {
    if (auto* iso = v8::Isolate::GetCurrent())
      iso->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical);
  }
  namespace {
    struct cpu_profiler_deleter {
      void operator()(v8::CpuProfiler* profiler) const noexcept {
        if (profiler)
          profiler->Dispose();
      }
    };
  } // namespace

  struct host::impl {
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    v8::Isolate* isolate = nullptr;
    std::unique_ptr<v8::CpuProfiler, cpu_profiler_deleter> cpu_profiler;
    bool cpu_profile_active = false;
    v8::Global<v8::Context> context;
    v8::Global<v8::Module> fxe_module;
    v8::Global<v8::Module> sqlite_module;
    v8::Global<v8::Module> ipc_module;
    v8::Global<v8::Module> net_module;
    v8::Global<v8::Module> os_module;
    struct module_cache_entry {
      v8::Global<v8::Module> mod;
      std::filesystem::file_time_type mtime{};
      bool embedded = false;
    };
    std::unordered_map<std::string, module_cache_entry> module_cache;
    std::unordered_map<std::string, std::unordered_set<std::string>> importers_by_dependency;
    std::unordered_map<std::string, std::unordered_set<std::string>> dependencies_by_importer;
    std::string entry_path;
    uint64_t runtime_id = 0;
    host::console_sink console_sink_fn = nullptr;
    void* console_sink_user = nullptr;
    host::debug_pump pump_fn = nullptr;
    host::debug_paused pump_paused_fn = nullptr;
    void* pump_user = nullptr;
    host* owner = nullptr;
    std::vector<fxe::window*> windows;
    std::vector<std::pair<fxe::window*, fxe::renderer*>> renderers;
    bool app_running = false;
    struct uv_microtask_checkpoint_state {
      std::mutex mu;
      v8::Isolate* isolate = nullptr;
      v8::Global<v8::Context> context;
    };
    std::shared_ptr<uv_microtask_checkpoint_state> uv_microtask_checkpoint;
    usize uv_microtask_checkpoint_id = 0;
    explicit impl(host::bootstrap_mode mode, uint64_t runtime_id);
    void record_import(std::string_view importer, std::string_view dependency);
    void clear_import_edges(std::string_view importer);
    std::vector<std::string> invalidate_module(v8::Isolate* iso, std::string_view path);
    ~impl();
  };

  runner_render_overrides g_runner_render_overrides;

  void set_runner_render_overrides(const runner_render_overrides& overrides) noexcept {
    g_runner_render_overrides = overrides;
  }

  const runner_render_overrides& get_runner_render_overrides() noexcept {
    return g_runner_render_overrides;
  }
  void dispatch_console_sink(v8::Isolate* iso, const char* level, std::string_view text) {
    auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
    if (p && p->console_sink_fn)
      p->console_sink_fn(p->console_sink_user, level, text);
  }
  host::host(bootstrap_mode mode) : host(mode, 0) {}

  host::host(bootstrap_mode mode, uint64_t runtime_id)
      : p_(std::make_unique<impl>(mode, runtime_id)) {
    p_->owner = this;
  }

  host::~host() = default;

  void host::set_console_sink(console_sink sink, void* user) noexcept {
    p_->console_sink_fn = sink;
    p_->console_sink_user = user;
  }

  void host::attach_debug_pump(debug_pump pump, debug_paused is_paused, void* user) noexcept {
    p_->pump_fn = pump;
    p_->pump_paused_fn = is_paused;
    p_->pump_user = user;
  }

  void host::pump_debug_tasks() {
    if (p_->pump_fn)
      p_->pump_fn(p_->pump_user);
  }

  bool host::debug_is_paused() const noexcept {
    return p_->pump_paused_fn ? p_->pump_paused_fn(p_->pump_user) : false;
  }

  fxe::window* host::active_window() const noexcept {
    return p_->windows.empty() ? nullptr : p_->windows.front();
  }
  fxe::renderer* host::active_renderer() const noexcept {
    return p_->renderers.empty() ? nullptr : p_->renderers.front().second;
  }

  std::vector<fxe::window*> host::windows() const {
    return p_->windows;
  }
  std::vector<fxe::renderer*> host::renderers() const {
    std::vector<fxe::renderer*> out;
    out.reserve(p_->renderers.size());
    for (auto& kv : p_->renderers)
      out.push_back(kv.second);
    return out;
  }
  fxe::window* host::window_at(usize index) const noexcept {
    return index < p_->windows.size() ? p_->windows[index] : nullptr;
  }
  fxe::renderer* host::renderer_for(fxe::window* w) const noexcept {
    if (!w)
      return nullptr;
    for (auto& kv : p_->renderers)
      if (kv.first == w)
        return kv.second;
    return nullptr;
  }
  usize host::window_index(fxe::window* w) const noexcept {
    for (usize i = 0; i < p_->windows.size(); ++i)
      if (p_->windows[i] == w)
        return i;
    return static_cast<usize>(-1);
  }

  void host::register_window(fxe::window* w) noexcept {
    if (!w)
      return;
    for (auto* x : p_->windows)
      if (x == w)
        return;
    p_->windows.push_back(w);
  }
  void host::unregister_window(fxe::window* w) noexcept {
    if (!w)
      return;
    for (auto it = p_->windows.begin(); it != p_->windows.end(); ++it) {
      if (*it == w) {
        p_->windows.erase(it);
        break;
      }
    }
    for (auto it = p_->renderers.begin(); it != p_->renderers.end();) {
      if (it->first == w)
        it = p_->renderers.erase(it);
      else
        ++it;
    }
  }
  void host::register_renderer(fxe::window* owner, fxe::renderer* r) noexcept {
    if (!r)
      return;
    for (auto& kv : p_->renderers)
      if (kv.second == r) {
        kv.first = owner;
        return;
      }
    p_->renderers.emplace_back(owner, r);
  }
  void host::unregister_renderer(fxe::renderer* r) noexcept {
    if (!r)
      return;
    for (auto it = p_->renderers.begin(); it != p_->renderers.end(); ++it) {
      if (it->second == r) {
        p_->renderers.erase(it);
        break;
      }
    }
  }

  bool host::is_app_running() const noexcept {
    return p_->app_running;
  }
  void host::set_app_running(bool running) noexcept {
    p_->app_running = running;
  }

  static host* host_from_isolate_impl(v8::Isolate* iso) {
    if (!iso)
      return nullptr;
    auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
    return p ? p->owner : nullptr;
  }

  host* host_for_isolate(v8::Isolate* iso) {
    return host_from_isolate_impl(iso);
  }

  void register_window_for_isolate(v8::Isolate* iso, fxe::window* w) {
    if (auto* h = host_from_isolate_impl(iso))
      h->register_window(w);
  }
  void unregister_window_for_isolate(v8::Isolate* iso, fxe::window* w) {
    if (auto* h = host_from_isolate_impl(iso))
      h->unregister_window(w);
  }
  void register_renderer_for_isolate(v8::Isolate* iso, fxe::window* owner, fxe::renderer* r) {
    if (auto* h = host_from_isolate_impl(iso))
      h->register_renderer(owner, r);
  }
  void unregister_renderer_for_isolate(v8::Isolate* iso, fxe::renderer* r) {
    if (auto* h = host_from_isolate_impl(iso))
      h->unregister_renderer(r);
  }
  fxe::window* get_active_window_for_isolate(v8::Isolate* iso) {
    auto* h = host_from_isolate_impl(iso);
    return h ? h->active_window() : nullptr;
  }
  fxe::renderer* get_active_renderer_for_isolate(v8::Isolate* iso) {
    auto* h = host_from_isolate_impl(iso);
    return h ? h->active_renderer() : nullptr;
  }

  // Used by bind_window.cpp::win_run to drain debug requests + honor pause.
  void pump_debug_for_isolate(v8::Isolate* iso) {
    if (!iso)
      return;
    auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
    if (p && p->pump_fn)
      p->pump_fn(p->pump_user);
  }
  bool is_paused_for_isolate(v8::Isolate* iso) {
    if (!iso)
      return false;
    auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
    return (p && p->pump_paused_fn) ? p->pump_paused_fn(p->pump_user) : false;
  }
  eval_result host::debug_evaluate(std::string_view expression, bool return_by_value) {
    eval_result result;
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);
    v8::TryCatch tc(iso);

    auto src = str(iso, expression);
    auto org = "<debug.evaluate>"_v8(iso);
    v8::ScriptOrigin sorigin(org);
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(ctx, src, &sorigin).ToLocal(&script)) {
      result.exception = "compile error: " + to_std_string(iso, tc.Exception());
      fill_exception_position(iso, ctx, tc, result.position);
      return result;
    }
    v8::Local<v8::Value> v;
    if (!script->Run(ctx).ToLocal(&v)) {
      result.exception = to_std_string(iso, tc.Exception());
      fill_exception_position(iso, ctx, tc, result.position);
      v8::Local<v8::Value> stack;
      if (tc.StackTrace(ctx).ToLocal(&stack))
        result.exception += "\n" + to_std_string(iso, stack);
      return result;
    }
    iso->PerformMicrotaskCheckpoint();
    if (v->IsUndefined()) {
      // emit no value -> client sees null
    } else if (return_by_value) {
      v8::Local<v8::String> json;
      if (v8::JSON::Stringify(ctx, v).ToLocal(&json)) {
        result.json_value = to_std_string(iso, json);
      } else {
        // Fall back to a string preview when JSON.stringify fails
        // (functions, cycles, BigInt, Symbol).
        result.json_value = "\"" + to_std_string(iso, v) + "\"";
      }
    } else {
      result.json_value = "\"" + to_std_string(iso, v) + "\"";
    }
    return result;
  }

  run_result host::fire_hmr(std::string_view path, int& handlers_called) {
    handlers_called = 0;
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);

    std::string install_error;
    if (!install_fxe_hmr_runtime(iso, ctx, &install_error))
      return {false, "HMR runtime install failed: " + install_error};

    v8::TryCatch tc(iso);
    auto global = ctx->Global();
    v8::Local<v8::Value> hmr_value;
    if (!global->Get(ctx, "__fxe_hmr"_v8(iso)).ToLocal(&hmr_value) || !hmr_value->IsObject()) {
      return {false, "__fxe_hmr is not an object"};
    }
    auto hmr = hmr_value.As<v8::Object>();
    v8::Local<v8::Value> fire_value;
    if (!hmr->Get(ctx, "fire"_v8(iso)).ToLocal(&fire_value) || !fire_value->IsFunction()) {
      return {false, "__fxe_hmr.fire is not a function"};
    }
    auto fire = fire_value.As<v8::Function>();
    v8::Local<v8::Value> argv[] = {str(iso, path)};
    v8::Local<v8::Value> result;
    if (!fire->Call(ctx, hmr, 1, argv).ToLocal(&result)) {
      std::string msg = to_std_string(iso, tc.Exception());
      v8::Local<v8::Value> stack;
      if (tc.StackTrace(ctx).ToLocal(&stack))
        msg += "\n" + to_std_string(iso, stack);
      return {false, msg};
    }
    if (!result->IsNumber())
      return {false, "__fxe_hmr.fire did not return a number"};
    handlers_called = result->Int32Value(ctx).FromMaybe(-1);
    return {true, {}};
  }

  std::vector<std::string> host::loaded_module_paths() const {
    std::vector<std::string> out;
    out.reserve(p_->module_cache.size());
    for (const auto& [path, entry] : p_->module_cache) {
      if (entry.embedded || path.empty() || path.front() == '<')
        continue;
      out.push_back(path);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  std::vector<std::string> host::invalidate_module(std::string_view path) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);
    return p_->invalidate_module(iso, path);
  }

  run_result host::reimport_module(std::string_view path) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);
    std::string error;
    v8::Local<v8::Value> ns;
    if (!reimport_module_for_isolate(iso, ctx, path, error).ToLocal(&ns)) {
      if (error.empty())
        error = "HMR module reimport failed";
      return {false, error};
    }
    return {true, {}};
  }

  std::vector<std::string> host::debug_global_keys() {
    std::vector<std::string> out;
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);
    auto global = ctx->Global();
    v8::Local<v8::Array> names;
    if (!global->GetOwnPropertyNames(ctx).ToLocal(&names))
      return out;
    out.reserve(names->Length());
    for (u32 i = 0; i < names->Length(); ++i) {
      v8::Local<v8::Value> k;
      if (!names->Get(ctx, i).ToLocal(&k))
        continue;
      out.push_back(to_std_string(iso, k));
    }
    return out;
  }

  namespace {
    class heap_snapshot_stream final : public v8::OutputStream {
    public:
      explicit heap_snapshot_stream(const host::heap_snapshot_chunk_sink& sink) : sink_(sink) {}

      int GetChunkSize() override {
        return 64 * 1024;
      }

      WriteResult WriteAsciiChunk(char* data, int size) override {
        if (size <= 0)
          return kContinue;
        if (sink_)
          sink_(std::string_view(data, static_cast<usize>(size)));
        return kContinue;
      }

      void EndOfStream() override {}

    private:
      const host::heap_snapshot_chunk_sink& sink_;
    };

    class string_output_stream final : public v8::OutputStream {
    public:
      explicit string_output_stream(std::string& out) : out_(out) {}

      int GetChunkSize() override {
        return 64 * 1024;
      }

      WriteResult WriteAsciiChunk(char* data, int size) override {
        if (size > 0)
          out_.append(data, static_cast<usize>(size));
        return kContinue;
      }

      void EndOfStream() override {}

    private:
      std::string& out_;
    };
  } // namespace

  namespace {
    int normalize_sampling_interval_us(int interval_us) {
      return interval_us > 0 ? interval_us : 1000;
    }

  } // namespace

  void host::debug_profiler_enable(int sampling_interval_us) {
    auto* iso = p_->isolate;
    if (!iso)
      return;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto interval_us = normalize_sampling_interval_us(sampling_interval_us);
    if (!p_->cpu_profiler) {
      v8::CpuProfiler::UseDetailedSourcePositionsForProfiling(iso);
      p_->cpu_profiler.reset(v8::CpuProfiler::New(iso));
    }
    p_->cpu_profiler->SetSamplingInterval(interval_us);
  }

  void host::debug_profiler_disable() {
    auto* iso = p_->isolate;
    if (!iso || !p_->cpu_profiler)
      return;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    if (p_->cpu_profile_active) {
      if (auto* profile = p_->cpu_profiler->StopProfiling("fxe"_v8(iso)))
        profile->Delete();
      p_->cpu_profile_active = false;
    }
    p_->cpu_profiler.reset();
  }

  host::cpu_profile_result host::debug_profiler_start(int sampling_interval_us) {
    auto* iso = p_->isolate;
    if (!iso)
      return {false, {}, "V8 isolate not attached"};
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto interval_us = normalize_sampling_interval_us(sampling_interval_us);
    if (!p_->cpu_profiler) {
      v8::CpuProfiler::UseDetailedSourcePositionsForProfiling(iso);
      p_->cpu_profiler.reset(v8::CpuProfiler::New(iso));
    }
    p_->cpu_profiler->SetSamplingInterval(interval_us);
    if (p_->cpu_profile_active)
      return {true, {}, {}};
    auto status = p_->cpu_profiler->StartProfiling("fxe"_v8(iso), v8::kLeafNodeLineNumbers,
                                                   /*record_samples*/ true);
    if (status == v8::CpuProfilingStatus::kErrorTooManyProfilers)
      return {false, {}, "V8 refused to start CPU profiling: too many active profilers"};
    p_->cpu_profile_active = true;
    return {true, {}, {}};
  }

  host::cpu_profile_result host::debug_profiler_stop() {
    auto* iso = p_->isolate;
    if (!iso)
      return {false, {}, "V8 isolate not attached"};
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    if (!p_->cpu_profiler)
      return {false, {}, "CPU profiler is not enabled"};
    auto* profile = p_->cpu_profiler->StopProfiling("fxe"_v8(iso));
    p_->cpu_profile_active = false;
    if (!profile)
      return {false, {}, "CPU profile was not active"};
    std::string profile_json;
    string_output_stream stream(profile_json);
    profile->Serialize(&stream, v8::CpuProfile::kJSON);
    profile->Delete();
    if (profile_json.empty())
      return {false, {}, "CPU profile serialization produced no data"};
    return {true, std::move(profile_json), {}};
  }

  void host::debug_collect_garbage() {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    iso->LowMemoryNotification();
    // Valid because initialize() sets --expose_gc before V8::Initialize().
    iso->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
  }

  void host::debug_take_heap_snapshot(const heap_snapshot_chunk_sink& sink) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto* profiler = iso->GetHeapProfiler();
    const v8::HeapSnapshot* snapshot = profiler->TakeHeapSnapshot();
    heap_snapshot_stream stream(sink);
    snapshot->Serialize(&stream, v8::HeapSnapshot::kJSON);
    const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
  }

  void host::install_window_global(window& win) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);
    auto obj = make_window_object(iso, ctx, &win);
    (void)ctx->Global()->Set(ctx, "window"_v8(iso), obj);
  }

  void host::install_renderer_global(renderer& r) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);
    auto obj = make_renderer_object(iso, ctx, &r);
    (void)ctx->Global()->Set(ctx, "renderer"_v8(iso), obj);
  }

  namespace {
    void register_source_map_for(const std::string& path, const std::string& js,
                                 int generated_line_offset);
  }

  // Classic-script path ----------------------------------------------------
  run_result host::run_script_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return {false, "script file not found: " + path.string()};
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string source = buf.str();
    if (is_typescript_path(path)) {
      auto* iso = p_->isolate;
      v8::Isolate::Scope is(iso);
      v8::HandleScope hs(iso);
      auto ts = transpile_typescript(iso, source, path.string());
      if (!ts.ok)
        return {false, "TypeScript transpile error: " + ts.message};
      source = std::move(ts.source);
      register_source_map_for(path.string(), source, ts.source_map_line_offset);
    }
    return run_script(source, path.string());
  }

  run_result host::run_script(std::string_view source, std::string_view origin) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);

    v8::TryCatch tc(iso);
    auto org = str(iso, origin);
    v8::ScriptOrigin sorigin(org, /*line_offset*/ 0, /*col*/ 0,
                             /*shared_cross_origin*/ false, /*script_id*/ -1,
                             source_map_url_value(iso, source));

    std::string cache_id;
    if (origin != "<inline>" && !origin.empty())
      cache_id = "fxe-script:" + std::string(origin);

    v8::Local<v8::Script> script;
    if (!v8_code_cache::compile_script(ctx, cache_id, source, sorigin).ToLocal(&script)) {
      std::string msg = "compile error: " + to_std_string(iso, tc.Exception());
      if (auto m = tc.Message(); !m.IsEmpty()) {
        msg += " @ " + to_std_string(iso, m->Get());
      }
      return {false, std::move(msg)};
    }
    v8::Local<v8::Value> result;
    if (!script->Run(ctx).ToLocal(&result)) {
      std::string msg = "runtime error: " + to_std_string(iso, tc.Exception());
      v8::Local<v8::Value> stack;
      if (tc.StackTrace(ctx).ToLocal(&stack))
        msg += "\n" + to_std_string(iso, stack);
      return {false, std::move(msg)};
    }
    return {true, to_std_string(iso, result)};
  }

  // ES-module path ---------------------------------------------------------
  namespace {
    namespace fs = std::filesystem;

    bool is_relative_specifier(std::string_view spec) {
      return spec == "." || spec == ".." || spec.starts_with("./") || spec.starts_with("../");
    }

    std::string normalize_module_path(const fs::path& path) {
      std::error_code ec;
      auto absolute = path.is_absolute() ? path : fs::absolute(path, ec);
      if (ec)
        absolute = path;
      auto canonical = fs::weakly_canonical(absolute, ec);
      if (!ec)
        return canonical.lexically_normal().string();
      return absolute.lexically_normal().string();
    }

    bool read_text_file(const fs::path& path, std::string& out) {
      // Bundle (single-file) virtual fs takes precedence so packaged apps see
      // their embedded scripts before disk lookups.
      if (auto v = fxe::runtime::read_virtual(path.string())) {
        out = std::move(*v);
        return true;
      }
      std::ifstream in(path, std::ios::binary);
      if (!in)
        return false;
      std::ostringstream buf;
      buf << in.rdbuf();
      out = buf.str();
      return true;
    }

    fs::file_time_type file_mtime(const fs::path& path) {
      std::error_code ec;
      auto t = fs::last_write_time(path, ec);
      return ec ? fs::file_time_type{} : t;
    }

    std::string normalize_slashes(std::string s) {
      for (auto& c : s)
        if (c == '\\')
          c = '/';
      return s;
    }

    // Try a list of candidate file paths; return the first that exists.
    bool first_existing(std::initializer_list<fs::path> candidates, fs::path& out) {
      for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && !fs::is_directory(c, ec)) {
          out = c;
          return true;
        }
      }
      return false;
    }

    // Walks up from `start_dir`, looking for `node_modules/<name>` and
    // resolving its entry file (package.json#main, then index.{ts,js}).
    bool resolve_node_modules(const fs::path& start_dir, std::string_view bare, std::string& out) {
      fs::path dir = start_dir;
      while (true) {
        fs::path pkg = dir / "node_modules" / std::string(bare);
        std::error_code ec;
        if (fs::is_directory(pkg, ec)) {
          // package.json#main
          fs::path pkg_json = pkg / "package.json";
          if (fs::exists(pkg_json, ec)) {
            std::string text;
            if (read_text_file(pkg_json, text)) {
              auto pos = text.find("\"main\"");
              if (pos != std::string::npos) {
                auto colon = text.find(':', pos);
                auto q1 = text.find('"', colon);
                auto q2 = q1 == std::string::npos ? std::string::npos : text.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                  fs::path main_rel = text.substr(q1 + 1, q2 - q1 - 1);
                  fs::path main_path = pkg / main_rel;
                  fs::path resolved;
                  if (first_existing({main_path, fs::path(main_path.string() + ".js"),
                                      fs::path(main_path.string() + ".ts")},
                                     resolved)) {
                    out = normalize_module_path(resolved);
                    return true;
                  }
                }
              }
            }
          }
          // index.{ts,js}
          fs::path resolved;
          if (first_existing({pkg / "index.ts", pkg / "index.js"}, resolved)) {
            out = normalize_module_path(resolved);
            return true;
          }
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir)
          return false;
        dir = dir.parent_path();
      }
    }

    struct synthetic_package {
      std::string_view spec;
      fs::path entry;
    };

    std::vector<synthetic_package> synthetic_packages() {
      fs::path root{FXE_SOURCE_DIR};
      return {
          {"fxe-ui", root / "packages" / "fxe-ui" / "src" / "index.ts"},
          {"fxe-ui/jsx-runtime", root / "packages" / "fxe-ui" / "src" / "jsx-runtime.ts"},
          {"fxe-doc", root / "packages" / "fxe-doc" / "src" / "index.ts"},
      };
    }

    bool resolve_synthetic_package_module(std::string_view spec, std::string& out,
                                          std::string& error) {
      fs::path root{FXE_SOURCE_DIR};
      const bool known_spec = spec == "fxe-ui" || spec == "fxe-ui/jsx-runtime" || spec == "fxe-doc";
      if (!known_spec)
        return false;
      if (root.empty()) {
        error = "fxe-ui synthetic module source directory is not configured";
        return true;
      }

      for (const auto& pkg : synthetic_packages()) {
        if (pkg.spec != spec)
          continue;

        std::error_code ec;
        if (!fs::exists(pkg.entry, ec) || fs::is_directory(pkg.entry, ec)) {
          error = "fxe-ui synthetic module source not found for '" + std::string(spec) +
                  "': " + pkg.entry.string();
          return true;
        }

        out = normalize_module_path(pkg.entry);
        return true;
      }
      return false;
    }

    // Resolves a specifier (relative, absolute, or bare) to a canonical
    // filesystem path. JSON files are accepted. Non-existent path stems are
    // probed with .ts / .js / /index.ts / /index.js suffixes.
    bool resolve_file_specifier(v8::Isolate* iso, std::string_view spec, std::string_view referrer,
                                std::string& out, std::string& error) {
      (void)iso;
      fs::path spec_path{std::string(spec)};
      const bool relative = is_relative_specifier(spec);
      if (!spec_path.is_absolute() && !relative) {
        // Bare specifier: walk node_modules from the referrer's directory.
        fs::path ref{std::string(referrer)};
        fs::path start = ref.empty() ? fs::current_path() : ref.parent_path();
        if (resolve_node_modules(start, spec, out))
          return true;
        error = "Module specifier not resolvable: '" + std::string(spec) +
                "'. Use 'fxe', a relative/absolute path, or an installed package.";
        return false;
      }
      fs::path base;
      if (spec_path.is_absolute()) {
        base = spec_path;
      } else {
        fs::path ref{std::string(referrer)};
        if (ref.empty() || ref.string().starts_with("<")) {
          error = "Relative module specifier has no file referrer: '" + std::string(spec) + "'";
          return false;
        }
        base = ref.parent_path() / spec_path;
      }
      // Probe candidates: exact, +.ts, +.js, +/index.ts, +/index.js.
      fs::path resolved;
      std::error_code ec;
      if (fs::exists(base, ec) && !fs::is_directory(base, ec)) {
        resolved = base;
      } else if (first_existing({fs::path(base.string() + ".ts"), fs::path(base.string() + ".js"),
                                 base / "index.ts", base / "index.js"},
                                resolved)) {
        // ok
      } else {
        error = "Module file not found for specifier: '" + std::string(spec) + "'";
        return false;
      }
      out = normalize_module_path(resolved);
      return true;
    }

    // ---------------------------------------------------------------------
    // Source-map registry bridge.
    // ---------------------------------------------------------------------
    std::unordered_map<v8::Module*, std::string>& module_path_table() {
      thread_local std::unordered_map<v8::Module*, std::string> tbl;
      return tbl;
    }

    void register_source_map_for(const std::string& path, const std::string& js,
                                 int generated_line_offset) {
      auto map_json = extract_inline_source_map(js);
      if (map_json.empty())
        return;
      source_maps().put_json(path, map_json, generated_line_offset);
    }

    void unregister_source_map_for(const std::string& path) {
      source_maps().erase(path);
    }

    void set_module_path(v8::Isolate* iso, v8::Local<v8::Module> mod, const std::string& path) {
      (void)iso;
      module_path_table()[*mod] = path;
    }

    void remap_frame_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      auto* iso = args.GetIsolate();
      v8::HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      std::string file = args.Length() > 0 ? to_std_string(iso, args[0]) : std::string{};
      int line = args.Length() > 1 ? args[1]->Int32Value(ctx).FromMaybe(0) : 0;
      int col = args.Length() > 2 ? args[2]->Int32Value(ctx).FromMaybe(0) : 0;

      std::string out_file = file;
      int out_line = line;
      int out_col = col;
      if (auto mapped = source_maps().original_position(file, line, col)) {
        out_file = mapped->source;
        out_line = mapped->line;
        out_col = mapped->column;
      }

      auto arr = v8::Array::New(iso, 3);
      (void)arr->Set(ctx, 0, str(iso, out_file));
      (void)arr->Set(ctx, 1, v8::Integer::New(iso, out_line));
      (void)arr->Set(ctx, 2, v8::Integer::New(iso, out_col));
      args.GetReturnValue().Set(arr);
    }

    // Per-module `import.meta` initialization. Looks up the module's path
    // from the table and exposes url / dirname / filename / main.
    void import_meta_callback(v8::Local<v8::Context> ctx, v8::Local<v8::Module> mod,
                              v8::Local<v8::Object> meta) {
      auto* iso = v8::Isolate::GetCurrent();
      v8::HandleScope hs(iso);
      auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
      auto& tbl = module_path_table();
      std::string path;
      if (auto it = tbl.find(*mod); it != tbl.end()) {
        path = it->second;
      } else {
        // Fallback: read script origin name.
        path = to_std_string(iso, mod->GetResourceName());
      }
      std::string normalized = normalize_slashes(path);
      std::string url = "file://" + (normalized.empty() || normalized.front() != '/'
                                         ? std::string("/") + normalized
                                         : normalized);
      fs::path fp(path);
      std::string dirname = normalize_slashes(fp.parent_path().string());
      std::string filename = normalize_slashes(fp.string());
      bool is_main = p && !p->entry_path.empty() && p->entry_path == path;
      (void)meta->CreateDataProperty(ctx, "url"_v8(iso), str(iso, url));
      (void)meta->CreateDataProperty(ctx, "dirname"_v8(iso), str(iso, dirname));
      (void)meta->CreateDataProperty(ctx, "filename"_v8(iso), str(iso, filename));
      (void)meta->CreateDataProperty(ctx, "main"_v8(iso), v8::Boolean::New(iso, is_main));
    }

    constexpr const char k_prepare_stack_trace_js[] = R"JS(
Error.prepareStackTrace = function(err, frames) {
  const lines = [String(err)];
  for (const f of frames) {
    let file = '<anonymous>';
    let line = 0;
    let col = 0;
    let fn = f.getFunctionName ? (f.getFunctionName() || '') : '';
    try { file = f.getFileName() || '<anonymous>'; } catch (_) {}
    try { line = f.getLineNumber() || 0; } catch (_) {}
    try { col = f.getColumnNumber() || 0; } catch (_) {}
    let mapped;
    try { mapped = globalThis.__fxe_remap_frame(file, line, col); } catch (_) {}
    if (mapped && mapped.length === 3) {
      file = mapped[0]; line = mapped[1]; col = mapped[2];
    }
    const where = file + ':' + line + ':' + col;
    lines.push(fn ? ('    at ' + fn + ' (' + where + ')')
                  : ('    at ' + where));
  }
  return lines.join('\n');
};
)JS";

    constexpr const char k_fxe_hmr_runtime_js[] = R"JS(
(function installFxeHmr() {
  if (globalThis.__fxe_hmr != null) return;
  const allKey = '*';
  const handlers = Object.create(null);
  function bucket(path) {
    const key = String(path);
    return handlers[key] || (handlers[key] = []);
  }
  function accept(pathOrFn, fn) {
    if (typeof pathOrFn === 'function' && fn === undefined) {
      bucket(allKey).push(pathOrFn);
      return;
    }
    if (typeof pathOrFn !== 'string' || typeof fn !== 'function') {
      throw new TypeError('__fxe_hmr.accept expects (path, handler) or (handler)');
    }
    bucket(pathOrFn).push(fn);
  }
  function nativeHmr() {
    const native = globalThis.__fxe_native && globalThis.__fxe_native.hmr;
    return native && typeof native === 'object' ? native : undefined;
  }
  function invalidate(path) {
    const key = String(path);
    const native = nativeHmr();
    if (native && typeof native.invalidate === 'function') {
      const evicted = native.invalidate(key);
      return Array.isArray(evicted) ? evicted.map(String) : [];
    }
    return [];
  }
  function reimport(path) {
    const key = String(path);
    return Promise.resolve().then(() => {
      const native = nativeHmr();
      if (native && typeof native.reimport === 'function') {
        native.reimport(key);
      } else if (typeof globalThis.__fxe_hmr_reload === 'function') {
        globalThis.__fxe_hmr_reload(key);
      }
    });
  }
  function fire(path) {
    const key = String(path);
    const native = nativeHmr();
    const evicted = invalidate(key);
    let moduleNamespace;
    if (evicted.length !== 0) {
      if (native && typeof native.reimport === 'function') {
        moduleNamespace = native.reimport(key);
      } else if (typeof globalThis.__fxe_hmr_reload === 'function') {
        globalThis.__fxe_hmr_reload(key);
      }
    }
    const calls = [
      ...(handlers[key] || []),
      ...(handlers[allKey] || []),
    ];
    let called = 0;
    for (const handler of calls) {
      ++called;
      try {
        handler(key, moduleNamespace, evicted);
      } catch (error) {
        console.error('fxe hmr:', error);
      }
    }
    if (typeof globalThis.__fxeUiEnsureFrameLoop === 'function') {
      try {
        globalThis.__fxeUiEnsureFrameLoop();
      } catch (error) {
        console.error('fxe hmr:', error);
      }
    }
    return called;
  }
  function watch(path) {
    const key = String(path);
    if (!globalThis.fs || typeof globalThis.fs.watch !== 'function') {
      throw new TypeError('__fxe_hmr.watch requires fs.watch');
    }
    const watcher = globalThis.fs.watch(key, { interval: 50 }, () => {
      fire(key);
    });
    return {
      close() {
        if (watcher && typeof watcher.close === 'function') {
          watcher.close();
        }
      }
    };
  }
  Object.defineProperty(globalThis, '__fxe_hmr', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: { handlers, accept, fire, watch, invalidate, reimport },
  });
})();
)JS";

    bool install_fxe_hmr_runtime(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                 std::string* error = nullptr) {
      v8::TryCatch tc(iso);
      v8::ScriptOrigin origin("<fxe-hmr-runtime>"_v8(iso));
      v8::Local<v8::Script> script;
      if (!v8_code_cache::compile_script(ctx, "fxe:hmr-runtime", k_fxe_hmr_runtime_js, origin)
               .ToLocal(&script)) {
        if (error)
          *error = to_std_string(iso, tc.Exception());
        return false;
      }
      v8::Local<v8::Value> ignored;
      if (!script->Run(ctx).ToLocal(&ignored)) {
        if (error)
          *error = to_std_string(iso, tc.Exception());
        return false;
      }
      return true;
    }

    v8::MaybeLocal<v8::Module> make_json_module(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                                const std::string& path) {
      std::string text;
      if (!read_text_file(path, text)) {
        iso->ThrowError(str(iso, "JSON module not found: " + path));
        return v8::MaybeLocal<v8::Module>();
      }
      v8::TryCatch tc(iso);
      v8::Local<v8::Value> parsed;
      if (!v8::JSON::Parse(ctx, str(iso, text)).ToLocal(&parsed)) {
        std::string msg = "JSON module parse error: " + path;
        if (tc.HasCaught())
          msg += ": " + to_std_string(iso, tc.Exception());
        iso->ThrowError(str(iso, msg));
        return v8::MaybeLocal<v8::Module>();
      }
      (void)parsed; // synthetic module re-parses on evaluation
      std::array<v8::Local<v8::String>, 1> exports{
          "default"_v8(iso),
      };
      v8::MemorySpan<const v8::Local<v8::String>> exports_span(exports.data(), exports.size());
      auto mod = v8::Module::CreateSyntheticModule(
          iso, str(iso, path), exports_span,
          +[](v8::Local<v8::Context> ctx2, v8::Local<v8::Module> m) -> v8::MaybeLocal<v8::Value> {
            auto* iso2 = v8::Isolate::GetCurrent();
            auto& tbl = module_path_table();
            auto it = tbl.find(*m);
            if (it == tbl.end())
              return v8::MaybeLocal<v8::Value>();
            std::string text;
            if (!read_text_file(it->second, text))
              return v8::MaybeLocal<v8::Value>();
            v8::Local<v8::Value> v;
            if (!v8::JSON::Parse(ctx2, str(iso2, text)).ToLocal(&v))
              return v8::MaybeLocal<v8::Value>();
            auto key = "default"_v8(iso2);
            auto ok = m->SetSyntheticModuleExport(iso2, key, v);
            if (ok.IsNothing())
              return v8::MaybeLocal<v8::Value>();
            return v8::Local<v8::Value>(v8::True(iso2));
          });
      return mod;
    }

    v8::MaybeLocal<v8::Module> compile_module_file(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                                   host::impl* p, const std::string& path) {
      if (!p)
        return v8::MaybeLocal<v8::Module>();
      auto current_mtime = file_mtime(path);
      if (auto it = p->module_cache.find(path); it != p->module_cache.end()) {
        if (it->second.mtime == current_mtime)
          return it->second.mod.Get(iso);
        // Stale: drop and recompile. V8 cannot re-instantiate the old module.
        if (!it->second.mod.IsEmpty()) {
          auto old = it->second.mod.Get(iso);
          module_path_table().erase(*old);
        }
        it->second.mod.Reset();
        p->module_cache.erase(it);
        unregister_source_map_for(path);
        p->clear_import_edges(path);
      }

      const auto ext = fs::path(path).extension().string();
      if (ext == ".json") {
        v8::Local<v8::Module> mod;
        if (!make_json_module(iso, ctx, path).ToLocal(&mod))
          return v8::MaybeLocal<v8::Module>();
        p->module_cache[path] =
            host::impl::module_cache_entry{v8::Global<v8::Module>(iso, mod), current_mtime};
        set_module_path(iso, mod, path);
        return mod;
      }

      std::string source;
      if (!read_text_file(path, source)) {
        iso->ThrowError(str(iso, "module file not found: " + path));
        return v8::MaybeLocal<v8::Module>();
      }

      if (is_typescript_path(path)) {
        auto ts = transpile_typescript(iso, source, path);
        if (!ts.ok) {
          iso->ThrowError(str(iso, "TypeScript transpile error: " + ts.message));
          return v8::MaybeLocal<v8::Module>();
        }
        source = std::move(ts.source);
        register_source_map_for(path, source, ts.source_map_line_offset);
      }

      v8::ScriptOrigin origin(str(iso, path), /*line_offset*/ 0, /*col*/ 0,
                              /*shared_cross_origin*/ false, /*script_id*/ -1,
                              source_map_url_value(iso, source), /*opaque*/ false,
                              /*is_wasm*/ false, /*is_module*/ true);
      v8::Local<v8::Module> mod;
      if (!v8_code_cache::compile_module(iso, "fxe-mod:" + path, source, origin).ToLocal(&mod))
        return v8::MaybeLocal<v8::Module>();
      p->module_cache[path] =
          host::impl::module_cache_entry{v8::Global<v8::Module>(iso, mod), current_mtime};
      set_module_path(iso, mod, path);
      return mod;
    }

    v8::MaybeLocal<v8::Module>
    compile_embedded_module(v8::Isolate* iso, [[maybe_unused]] v8::Local<v8::Context> ctx,
                            host::impl* p, const fxe::runtime::node_compat_asset& asset) {
      if (!p)
        return v8::MaybeLocal<v8::Module>();

      const std::string cache_key = asset.canonical_specifier;
      if (auto it = p->module_cache.find(cache_key); it != p->module_cache.end()) {
        if (it->second.embedded)
          return it->second.mod.Get(iso);
        if (!it->second.mod.IsEmpty()) {
          auto old = it->second.mod.Get(iso);
          module_path_table().erase(*old);
        }
        it->second.mod.Reset();
        p->module_cache.erase(it);
        unregister_source_map_for(asset.asset_path);
        p->clear_import_edges(cache_key);
      }

      std::string source{asset.source};
      if (is_typescript_path(asset.asset_path)) {
        auto ts = transpile_typescript(iso, source, asset.asset_path);
        if (!ts.ok) {
          iso->ThrowError(str(iso, "TypeScript transpile error: " + ts.message));
          return v8::MaybeLocal<v8::Module>();
        }
        source = std::move(ts.source);
        register_source_map_for(asset.asset_path, source, ts.source_map_line_offset);
      }

      v8::ScriptOrigin origin(str(iso, asset.asset_path), /*line_offset*/ 0, /*col*/ 0,
                              /*shared_cross_origin*/ false, /*script_id*/ -1,
                              source_map_url_value(iso, source), /*opaque*/ false,
                              /*is_wasm*/ false, /*is_module*/ true);
      v8::Local<v8::Module> mod;
      if (!v8_code_cache::compile_module(iso, "fxe-embed:" + cache_key, source, origin)
               .ToLocal(&mod))
        return v8::MaybeLocal<v8::Module>();

      p->module_cache[cache_key] = host::impl::module_cache_entry{
          v8::Global<v8::Module>(iso, mod), std::filesystem::file_time_type{}, true};
      set_module_path(iso, mod, asset.asset_path);
      return mod;
    }

    bool is_vendored_unenv_referrer(std::string_view referrer_path) {
      return referrer_path.starts_with("vendor/unenv/");
    }

    std::optional<fxe::runtime::node_compat_asset>
    resolve_embedded_bare_asset(std::string_view spec, std::string_view referrer_path) {
      if (!is_vendored_unenv_referrer(referrer_path))
        return std::nullopt;
      if (spec == "pathe")
        return fxe::runtime::resolve_unenv_pathe_asset();
      return std::nullopt;
    }

    std::optional<fxe::runtime::node_compat_asset>
    resolve_embedded_relative_asset(std::string_view spec, std::string_view referrer_path) {
      if (!is_vendored_unenv_referrer(referrer_path) || !is_relative_specifier(spec))
        return std::nullopt;

      fs::path base{std::string(referrer_path)};
      fs::path resolved = (base.parent_path() / fs::path{std::string(spec)}).lexically_normal();
      return fxe::runtime::resolve_node_compat_asset_path(normalize_slashes(resolved.string()));
    }

    v8::MaybeLocal<v8::Module> resolve_module(v8::Local<v8::Context> ctx,
                                              v8::Local<v8::String> specifier,
                                              v8::Local<v8::FixedArray> /*import_attributes*/,
                                              v8::Local<v8::Module> referrer) {
      auto* iso = v8::Isolate::GetCurrent();
      v8::String::Utf8Value sv(iso, specifier);
      std::string_view spec(*sv ? *sv : "", *sv ? sv.length() : 0);
      auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
      if (spec == "fxe") {
        if (p)
          return p->fxe_module.Get(iso);
        return v8::MaybeLocal<v8::Module>();
      }
      if (spec == "fxe:sqlite") {
        if (p)
          return p->sqlite_module.Get(iso);
        return v8::MaybeLocal<v8::Module>();
      }
      if (spec == "fxe:ipc") {
        if (p)
          return p->ipc_module.Get(iso);
        return v8::MaybeLocal<v8::Module>();
      }
      if (spec == "fxe:net") {
        if (p)
          return p->net_module.Get(iso);
        return v8::MaybeLocal<v8::Module>();
      }
      if (spec == "fxe:os") {
        if (p)
          return p->os_module.Get(iso);
        return v8::MaybeLocal<v8::Module>();
      }
      std::string referrer_path = to_std_string(iso, referrer->GetResourceName());
      std::string resolved;
      std::string error;
      auto record_dependency = [&](std::string_view dependency) {
        if (p)
          p->record_import(referrer_path, dependency);
      };
      if (fxe::runtime::is_node_builtin_specifier(spec)) {
        auto asset = fxe::runtime::resolve_node_compat_asset(spec);
        if (asset) {
          auto mod = compile_embedded_module(iso, ctx, p, *asset);
          if (!mod.IsEmpty())
            record_dependency(asset->canonical_specifier);
          return mod;
        }
        fxe::runtime::throw_node_compat_disabled(iso, spec);
        return v8::MaybeLocal<v8::Module>();
      }
      if (auto asset = resolve_embedded_bare_asset(spec, referrer_path)) {
        auto mod = compile_embedded_module(iso, ctx, p, *asset);
        if (!mod.IsEmpty())
          record_dependency(asset->canonical_specifier);
        return mod;
      }
      if (auto asset = resolve_embedded_relative_asset(spec, referrer_path)) {
        auto mod = compile_embedded_module(iso, ctx, p, *asset);
        if (!mod.IsEmpty())
          record_dependency(asset->canonical_specifier);
        return mod;
      }

      if (resolve_synthetic_package_module(spec, resolved, error)) {
        if (!error.empty()) {
          iso->ThrowError(str(iso, error));
          return v8::MaybeLocal<v8::Module>();
        }
        auto mod = compile_module_file(iso, ctx, p, resolved);
        if (!mod.IsEmpty())
          record_dependency(resolved);
        return mod;
      }
      if (!resolve_file_specifier(iso, spec, referrer_path, resolved, error)) {
        iso->ThrowError(str(iso, error));
        return v8::MaybeLocal<v8::Module>();
      }
      auto mod = compile_module_file(iso, ctx, p, resolved);
      if (!mod.IsEmpty())
        record_dependency(resolved);
      return mod;
    }
    // Drains microtasks and host message-loop tasks until the supplied promise
    // settles, the pump count is exhausted, or a short real-time deadline
    // elapses. The sleep prevents timer-backed top-level awaits from spinning
    // through the whole budget before their due time arrives.
    v8::Promise::PromiseState pump_until_settled(v8::Isolate* iso,
                                                 [[maybe_unused]] v8::Local<v8::Context> ctx,
                                                 v8::Local<v8::Promise> p, int max_pumps = 2048) {
      using clock = std::chrono::steady_clock;
      // Drain queued microtasks at least once even if the module promise is
      // already fulfilled — synchronous top-level code may have queued work
      // (open() upgradeneeded, setTimeout(0), …) that won't otherwise run if
      // there is no event loop pump.
      iso->PerformMicrotaskCheckpoint();
      auto state = p->State();
      int pumps = 0;
      const auto deadline = clock::now() + std::chrono::seconds(1);
      while (state == v8::Promise::kPending && pumps < max_pumps && clock::now() < deadline) {
        iso->PerformMicrotaskCheckpoint();
        v8::platform::PumpMessageLoop(g_platform.get(), iso);
        fxe::os::pump_main_thread_dispatches();
#if FXE_HAS_LIBUV
        fxe::runtime::pump_nonblocking();
#endif
        drain_due_timers(iso);
        state = p->State();
        ++pumps;
        if (state == v8::Promise::kPending)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return state;
    }

    bool reload_hmr_module(v8::Isolate* iso, v8::Local<v8::Context> ctx, host::impl* p,
                           std::string_view raw_path, std::string& error) {
      if (!p) {
        error = "V8 host not attached";
        return false;
      }

      const std::string path = normalize_module_path(fs::path(std::string(raw_path)));
      auto it = p->module_cache.find(path);
      if (it == p->module_cache.end())
        return true;
      if (it->second.embedded)
        return true;

      if (!read_text_file(path, error)) {
        error = "module file not found: " + path;
        return false;
      }
      error.clear();

      if (!it->second.mod.IsEmpty()) {
        auto old = it->second.mod.Get(iso);
        module_path_table().erase(*old);
      }
      it->second.mod.Reset();
      p->module_cache.erase(it);

      v8::TryCatch tc(iso);
      v8::Local<v8::Module> mod;
      if (!compile_module_file(iso, ctx, p, path).ToLocal(&mod)) {
        error = "HMR module compile failed: " + to_std_string(iso, tc.Exception());
        return false;
      }
      v8::Maybe<bool> instantiated = mod->InstantiateModule(ctx, &resolve_module);
      if (instantiated.IsNothing() || !instantiated.FromJust()) {
        error = "HMR module instantiation failed";
        if (tc.HasCaught())
          error += ": " + to_std_string(iso, tc.Exception());
        return false;
      }
      v8::Local<v8::Value> eval_result;
      if (!mod->Evaluate(ctx).ToLocal(&eval_result)) {
        error = "HMR module evaluation failed: " + to_std_string(iso, tc.Exception());
        v8::Local<v8::Value> stack;
        if (tc.StackTrace(ctx).ToLocal(&stack))
          error += "\n" + to_std_string(iso, stack);
        return false;
      }
      if (eval_result->IsPromise()) {
        auto promise = eval_result.As<v8::Promise>();
        auto state = pump_until_settled(iso, ctx, promise);
        if (state == v8::Promise::kRejected) {
          error = "HMR module top-level rejection: " +
                  console_arg_to_string(iso, ctx, promise->Result(), true);
          return false;
        }
        if (state == v8::Promise::kPending) {
          error = "HMR module top-level await did not settle within budget";
          return false;
        }
      }
      return true;
    }

    void hmr_reload_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      auto* iso = args.GetIsolate();
      v8::HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
      if (args.Length() < 1) {
        iso->ThrowError("__fxe_hmr_reload expects a module path"_v8(iso));
        return;
      }
      std::string error;
      if (!reload_hmr_module(iso, ctx, p, to_std_string(iso, args[0]), error)) {
        iso->ThrowError(str(iso, error));
        return;
      }
      args.GetReturnValue().Set(v8::True(iso));
    }
  } // namespace

  static void erase_cached_module(v8::Isolate* iso, host::impl* p, const std::string& key,
                                  bool erase_source_map = true) {
    if (!p)
      return;
    auto it = p->module_cache.find(key);
    if (it == p->module_cache.end())
      return;
    if (!it->second.mod.IsEmpty()) {
      auto old = it->second.mod.Get(iso);
      module_path_table().erase(*old);
    }
    it->second.mod.Reset();
    p->module_cache.erase(it);
    if (erase_source_map)
      unregister_source_map_for(key);
  }

  void host::impl::record_import(std::string_view importer, std::string_view dependency) {
    if (importer.empty() || dependency.empty())
      return;
    const std::string importer_key{importer};
    const std::string dependency_key{dependency};
    dependencies_by_importer[importer_key].insert(dependency_key);
    importers_by_dependency[dependency_key].insert(importer_key);
  }

  void host::impl::clear_import_edges(std::string_view importer) {
    const std::string importer_key{importer};
    auto deps_it = dependencies_by_importer.find(importer_key);
    if (deps_it == dependencies_by_importer.end())
      return;
    for (const auto& dependency : deps_it->second) {
      auto importers_it = importers_by_dependency.find(dependency);
      if (importers_it == importers_by_dependency.end())
        continue;
      importers_it->second.erase(importer_key);
      if (importers_it->second.empty())
        importers_by_dependency.erase(importers_it);
    }
    dependencies_by_importer.erase(deps_it);
  }

  std::vector<std::string> host::impl::invalidate_module(v8::Isolate* iso,
                                                         std::string_view raw_path) {
    const std::string root = normalize_module_path(fs::path(std::string(raw_path)));
    std::vector<std::string> queue{root};
    std::unordered_set<std::string> seen;
    std::vector<std::string> ordered;
    for (usize i = 0; i < queue.size(); ++i) {
      const std::string current = queue[i];
      if (!seen.insert(current).second)
        continue;
      ordered.push_back(current);
      auto importers_it = importers_by_dependency.find(current);
      if (importers_it == importers_by_dependency.end())
        continue;
      std::vector<std::string> importers(importers_it->second.begin(), importers_it->second.end());
      std::sort(importers.begin(), importers.end());
      for (const auto& importer : importers)
        queue.push_back(importer);
    }

    std::vector<std::string> evicted;
    for (const auto& key : ordered) {
      auto it = module_cache.find(key);
      if (it == module_cache.end() || it->second.embedded)
        continue;
      evicted.push_back(key);
      erase_cached_module(iso, this, key);
    }

    for (const auto& key : ordered)
      clear_import_edges(key);
    for (const auto& key : ordered) {
      auto importers_it = importers_by_dependency.find(key);
      if (importers_it == importers_by_dependency.end())
        continue;
      for (const auto& importer : importers_it->second) {
        auto deps_it = dependencies_by_importer.find(importer);
        if (deps_it != dependencies_by_importer.end())
          deps_it->second.erase(key);
      }
      importers_by_dependency.erase(importers_it);
    }

    return evicted;
  }

  std::vector<std::string> invalidate_module_for_isolate(v8::Isolate* iso, std::string_view path,
                                                         std::string& error) {
    error.clear();
    if (!iso) {
      error = "V8 isolate not attached";
      return {};
    }
    auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
    if (!p) {
      error = "V8 host not attached";
      return {};
    }
    return p->invalidate_module(iso, path);
  }

  v8::MaybeLocal<v8::Value> reimport_module_for_isolate(v8::Isolate* iso,
                                                        v8::Local<v8::Context> ctx,
                                                        std::string_view raw_path,
                                                        std::string& error) {
    error.clear();
    if (!iso) {
      error = "V8 isolate not attached";
      return v8::MaybeLocal<v8::Value>();
    }
    auto* p = static_cast<host::impl*>(iso->GetData(kIsolateSlotHostImpl));
    if (!p) {
      error = "V8 host not attached";
      return v8::MaybeLocal<v8::Value>();
    }

    const std::string path = normalize_module_path(fs::path(std::string(raw_path)));
    std::string source_probe;
    if (!read_text_file(path, source_probe)) {
      error = "module file not found: " + path;
      return v8::MaybeLocal<v8::Value>();
    }

    p->invalidate_module(iso, path);

    v8::TryCatch tc(iso);
    v8::Local<v8::Module> mod;
    if (!compile_module_file(iso, ctx, p, path).ToLocal(&mod)) {
      error = "HMR module compile failed: " + to_std_string(iso, tc.Exception());
      return v8::MaybeLocal<v8::Value>();
    }
    v8::Maybe<bool> instantiated = mod->InstantiateModule(ctx, &resolve_module);
    if (instantiated.IsNothing() || !instantiated.FromJust()) {
      error = "HMR module instantiation failed";
      if (tc.HasCaught())
        error += ": " + to_std_string(iso, tc.Exception());
      return v8::MaybeLocal<v8::Value>();
    }
    v8::Local<v8::Value> eval_result;
    if (!mod->Evaluate(ctx).ToLocal(&eval_result)) {
      error = "HMR module evaluation failed: " + to_std_string(iso, tc.Exception());
      v8::Local<v8::Value> stack;
      if (tc.StackTrace(ctx).ToLocal(&stack))
        error += "\n" + to_std_string(iso, stack);
      return v8::MaybeLocal<v8::Value>();
    }
    if (eval_result->IsPromise()) {
      auto promise = eval_result.As<v8::Promise>();
      auto state = pump_until_settled(iso, ctx, promise);
      if (state == v8::Promise::kRejected) {
        error = "HMR module top-level rejection: " +
                console_arg_to_string(iso, ctx, promise->Result(), true);
        return v8::MaybeLocal<v8::Value>();
      }
      if (state == v8::Promise::kPending) {
        error = "HMR module top-level await did not settle within budget";
        return v8::MaybeLocal<v8::Value>();
      }
    }
    return mod->GetModuleNamespace();
  }

  // host::impl ctor + dtor live here so the body can see the second anonymous
  // namespace's helpers (source maps, module path table, import.meta callback,
  // remap_frame callback, prepare-stack-trace JS).
  host::impl::impl(host::bootstrap_mode mode, uint64_t runtime_id) : runtime_id(runtime_id) {
    if (!g_initialized)
      initialize();

    v8::Isolate::CreateParams params;
    allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    params.array_buffer_allocator = allocator.get();

    // Size the heap based on the host machine rather than V8's embedded-device
    // defaults. ConfigureDefaults takes physical memory and a virtual-memory
    // ceiling (0 = no limit beyond the OS); V8 picks young/old generation
    // sizes from there.
    {
      const uint64_t physical = physical_memory_bytes();
      params.constraints.ConfigureDefaults(physical, 0);
    }

    isolate = v8::Isolate::New(params);

    isolate->SetData(kIsolateSlotHostImpl, this);
    isolate->SetData(kIsolateSlotRuntimeId,
                     reinterpret_cast<void*>(static_cast<uintptr_t>(runtime_id)));

    // Explicit microtask draining. We already call PerformMicrotaskCheckpoint
    // at frame boundaries and inside the libuv loop; auto policy would re-drain
    // after every binding callback (slower, and re-enters JS at surprising
    // points inside C++ callbacks).
    isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);

    // Async stacks through TS -> binding -> TS get long fast. 32 frames is a
    // good ceiling for debugging without runaway capture cost.
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);

    // Surface unhandled rejections to the console sink so they don't vanish.
    isolate->SetPromiseRejectCallback(&promise_reject_callback);

    v8::Isolate::Scope iscope(isolate);
    v8::HandleScope hscope(isolate);

    // Per-isolate `_v8` literal cache. Must run before any binding installer
    // (below) materialises a `_v8` literal.
    fxe::js::install_string_cache(isolate);

    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    const bool worker_mode = mode == host::bootstrap_mode::worker_thread;

    // Bindings register their constructors / namespaces on the global template.
    // B1's window_thread mode is scaffold-only for now: it still installs the
    // full main-thread binding set until per-window thread pinning lands.
    if (!worker_mode) {
      install_command_buffer_template(isolate, global);
      install_renderer_template(isolate, global);
      install_offscreen_template(isolate, global);
      install_window_template(isolate, global);
      install_primitives_namespace(isolate, global);
      install_text_document_template(isolate, global);
      install_layout_global(isolate, global);
      install_print_global(isolate, global);
      install_markdown_global(isolate, global);
      install_render_stats_global(isolate, global);
      install_image_global(isolate, global);
      install_spritesheet_global(isolate, global);
      install_font_global(isolate, global);
      install_shell_global(isolate, global);
      install_dialog_global(isolate, global);
      install_notification_global(isolate, global);
      install_menu_global(isolate, global);
      install_tray_global(isolate, global);
      install_global_shortcut_global(isolate, global);
      install_audio_bindings(isolate, global);
      install_sqlite_bindings(isolate, global);
      install_storage_globals(isolate, global);
      install_indexed_db_bindings(isolate, global);
      install_webauthn_globals(isolate, global);
      install_ipc_bindings(isolate, global);
      install_runtime_dispatch_handlers();
    }
    install_fs_global(isolate, global);
    install_path_global(isolate, global);
    install_process_global(isolate, global);
    install_timers_global(isolate, global);
    install_url_globals(isolate, global);
    install_fetch_globals(isolate, global);
    install_blob_global(isolate, global);
    install_websocket_global(isolate, global);
    install_wasm_streaming(isolate);

    v8::Local<v8::Context> ctx = v8::Context::New(isolate, nullptr, global);
    context.Reset(isolate, ctx);
    uv_microtask_checkpoint = std::make_shared<uv_microtask_checkpoint_state>();
    uv_microtask_checkpoint->isolate = isolate;
    uv_microtask_checkpoint->context.Reset(isolate, ctx);

    // Force-install console.log on the freshly-created global object so it
    // overrides any embedder-injected variant.
    {
      v8::Context::Scope cs(ctx);
      fxe::runtime::install_fxe_native(isolate, ctx);
      fxe::runtime::install_native_async_hooks(isolate, ctx);
      fxe::runtime::install_native_inspector(isolate, ctx);
      fxe::runtime::install_native_v8_module(isolate, ctx);
      fxe::runtime::install_native_vm(isolate, ctx);
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
      fxe::runtime::install_native_zlib(isolate, ctx);
#endif
      install_hmr_native_bindings(isolate, ctx);
      fxe::runtime::install_node_compat(isolate, ctx);
      auto fn = v8::Function::New(ctx, console_log_callback).ToLocalChecked();
      auto console_obj = v8::Object::New(isolate);
      (void)console_obj->Set(ctx, "log"_v8(isolate), fn);
      auto fn_warn = v8::Function::New(ctx, console_warn_callback).ToLocalChecked();
      auto fn_err = v8::Function::New(ctx, console_error_callback).ToLocalChecked();
      (void)console_obj->Set(ctx, "warn"_v8(isolate), fn_warn);
      (void)console_obj->Set(ctx, "error"_v8(isolate), fn_err);
      (void)console_obj->Set(ctx, "info"_v8(isolate), fn);
      (void)console_obj->Set(ctx, "debug"_v8(isolate), fn);
      (void)ctx->Global()->Set(ctx, "console"_v8(isolate), console_obj);

      // performance.now() — monotonic ms since the host's first init.
      auto perf_now = v8::Function::New(ctx, performance_now_callback).ToLocalChecked();
      auto performance_obj = v8::Object::New(isolate);
      (void)performance_obj->Set(ctx, "now"_v8(isolate), perf_now);
      (void)ctx->Global()->Set(ctx, "performance"_v8(isolate), performance_obj);

      if (!worker_mode) {
        // App extras (OS shims layered onto the existing App global).
        v8::Local<v8::Value> appv;
        if (ctx->Global()->Get(ctx, "App"_v8(isolate)).ToLocal(&appv) && appv->IsObject()) {
          install_app_extras_to(isolate, ctx, appv.As<v8::Object>());
          install_power_monitor_to(isolate, ctx, appv.As<v8::Object>());
          install_crash_reporter_to(isolate, ctx, appv.As<v8::Object>());
        }
        // performance.timeline layered on top of the bare performance object.
        install_performance_global(isolate, ctx->Global());
      }

      auto hmr_reload = v8::Function::New(ctx, hmr_reload_callback).ToLocalChecked();
      (void)ctx->Global()->Set(ctx, "__fxe_hmr_reload"_v8(isolate), hmr_reload);
      // HMR registry plus polling watch bridge. When a fired path is present in
      // the module cache, __fxe_hmr reloads that module before user handlers run.
      (void)install_fxe_hmr_runtime(isolate, ctx);

      auto vertex_topology = v8::Object::New(isolate);
      (void)vertex_topology->Set(ctx, "Triangle"_v8(isolate), 0_v8(isolate));
      (void)vertex_topology->Set(ctx, "Line"_v8(isolate), 1_v8(isolate));
      (void)ctx->Global()->Set(ctx, "VertexTopology"_v8(isolate), vertex_topology);

      if (!worker_mode) {
        // Build the synthetic `fxe` module exporting the engine globals. Each
        // entry is looked up on the global object at evaluation time so we never
        // duplicate constructor templates.
        std::array<v8::Local<v8::String>, 11> exports{
            "Window"_v8(isolate),     "Renderer"_v8(isolate),      "OffscreenRenderer"_v8(isolate),
            "Primitives"_v8(isolate), "CommandBuffer"_v8(isolate), "Monitors"_v8(isolate),
            "App"_v8(isolate),        "Print"_v8(isolate),         "VertexTopology"_v8(isolate),
            "Image"_v8(isolate),      "Spritesheet"_v8(isolate),
        };
        v8::MemorySpan<const v8::Local<v8::String>> exports_span(exports.data(), exports.size());
        auto module_name = "fxe"_v8(isolate);
        auto mod = v8::Module::CreateSyntheticModule(
            isolate, module_name, exports_span,
            +[](v8::Local<v8::Context> ctx,
                v8::Local<v8::Module> mod) -> v8::MaybeLocal<v8::Value> {
              auto* iso = v8::Isolate::GetCurrent();
              auto global = ctx->Global();
              std::array<v8::Local<v8::String>, 11> names = {
                  "Window"_v8(iso),     "Renderer"_v8(iso),      "OffscreenRenderer"_v8(iso),
                  "Primitives"_v8(iso), "CommandBuffer"_v8(iso), "Monitors"_v8(iso),
                  "App"_v8(iso),        "Print"_v8(iso),         "VertexTopology"_v8(iso),
                  "Image"_v8(iso),      "Spritesheet"_v8(iso),
              };
              for (auto key : names) {
                v8::Local<v8::Value> val;
                if (!global->Get(ctx, key).ToLocal(&val)) {
                  return v8::MaybeLocal<v8::Value>();
                }
                v8::Maybe<bool> ok = mod->SetSyntheticModuleExport(iso, key, val);
                if (ok.IsNothing())
                  return v8::MaybeLocal<v8::Value>();
              }
              return v8::Local<v8::Value>(v8::True(iso));
            });
        fxe_module.Reset(isolate, mod);
      }
      // Synthetic `fxe:sqlite` module — exports the Database constructor and
      // a small constants record for fileControl + open flags.
      {
        v8::Local<v8::Module> mod;
        if (build_sqlite_module(isolate, ctx).ToLocal(&mod))
          sqlite_module.Reset(isolate, mod);
      }
      // Synthetic `fxe:ipc` module — same-isolate request/reply + events.
      {
        v8::Local<v8::Module> mod;
        if (build_ipc_module(isolate, ctx).ToLocal(&mod))
          ipc_module.Reset(isolate, mod);
      }
      // Synthetic `fxe:net` module — re-exports native dns/socket namespaces.
      {
        v8::Local<v8::Module> mod;
        if (build_net_module(isolate, ctx).ToLocal(&mod))
          net_module.Reset(isolate, mod);
      }
      // Synthetic `fxe:os` module — re-exports the native OS namespace as named exports.
      {
        v8::Local<v8::Module> mod;
        if (build_os_module(isolate, ctx).ToLocal(&mod))
          os_module.Reset(isolate, mod);
      }

      // Install per-module import.meta hook (must be set on the isolate).
      isolate->SetHostInitializeImportMetaObjectCallback(&import_meta_callback);

      // Install __fxe_remap_frame native + Error.prepareStackTrace JS.
      auto remap_fn = v8::Function::New(ctx, remap_frame_callback).ToLocalChecked();
      (void)ctx->Global()->Set(ctx, "__fxe_remap_frame"_v8(isolate), remap_fn);
      v8::TryCatch tc(isolate);
      v8::ScriptOrigin pst_origin("<fxe-prepare-stack-trace>"_v8(isolate));
      v8::Local<v8::Script> pst_script;
      if (v8::Script::Compile(ctx, str(isolate, k_prepare_stack_trace_js), &pst_origin)
              .ToLocal(&pst_script)) {
        v8::Local<v8::Value> ignored;
        (void)pst_script->Run(ctx).ToLocal(&ignored);
      }
#if FXE_HAS_LIBUV
      if (!worker_mode) {
        auto checkpoint = uv_microtask_checkpoint;
        uv_microtask_checkpoint_id =
            fxe::runtime::uv_loop_runtime::instance().register_microtask_checkpoint([checkpoint] {
              std::lock_guard<std::mutex> lock(checkpoint->mu);
              auto* isolate = checkpoint->isolate;
              if (isolate == nullptr)
                return;
              v8::Isolate::Scope is(isolate);
              v8::HandleScope hs(isolate);
              auto ctx = checkpoint->context.Get(isolate);
              if (!ctx.IsEmpty()) {
                v8::Context::Scope cs(ctx);
                isolate->PerformMicrotaskCheckpoint();
                return;
              }
              isolate->PerformMicrotaskCheckpoint();
            });
      }
#endif
    }
  }

  host::impl::~impl() {
#if FXE_HAS_LIBUV
    fxe::runtime::uv_loop_runtime::instance().unregister_microtask_checkpoint(
        uv_microtask_checkpoint_id);
    uv_microtask_checkpoint_id = 0;
#endif
    if (uv_microtask_checkpoint) {
      std::lock_guard<std::mutex> microtask_lock(uv_microtask_checkpoint->mu);
      uv_microtask_checkpoint->context.Reset();
      uv_microtask_checkpoint->isolate = nullptr;
    }
    for (auto& [_, entry] : module_cache)
      entry.mod.Reset();
    module_cache.clear();
    fxe_module.Reset();
    sqlite_module.Reset();
    ipc_module.Reset();
    net_module.Reset();
    os_module.Reset();
    if (cpu_profiler) {
      if (cpu_profile_active && isolate) {
        v8::Isolate::Scope is(isolate);
        v8::HandleScope hs(isolate);
        if (auto* profile = cpu_profiler->StopProfiling("fxe"_v8(isolate)))
          profile->Delete();
        cpu_profile_active = false;
      }
      cpu_profiler.reset();
    }
    context.Reset();
    dispose_typescript_compiler(isolate);
    if (isolate) {
      // Reset every binding's per-isolate template holder while the
      // isolate is still alive. Resetting a v8::Global tied to a
      // disposed isolate trips a CHECK in V8.
      {
        v8::Isolate::Scope is(isolate);
        v8::HandleScope hs(isolate);
        run_template_resetters(isolate);
        fxe::runtime::uninstall_native_async_hooks(isolate);
        // Drop the `_v8` literal cache while the isolate + a HandleScope are
        // still alive; Eternal handles cannot outlive the isolate.
        fxe::js::uninstall_string_cache(isolate);
        // Drop process.on listener Globals + any tracked unhandled rejections
        // while the isolate is alive — both keep v8::Globals tied to it.
        clear_process_listeners(isolate);
        clear_pending_rejections(isolate);
      }
      isolate->Dispose();
      isolate = nullptr;
    }
  }

  run_result host::run_module_file(const std::filesystem::path& path) {
    std::string resolved = normalize_module_path(path);
    std::string source;
    if (!read_text_file(resolved, source))
      return {false, "module file not found: " + path.string()};
    p_->entry_path = resolved;
    if (is_typescript_path(path)) {
      auto* iso = p_->isolate;
      v8::Isolate::Scope is(iso);
      v8::HandleScope hs(iso);
      auto ts = transpile_typescript(iso, source, resolved);
      if (!ts.ok)
        return {false, "TypeScript transpile error: " + ts.message};
      source = std::move(ts.source);
      register_source_map_for(resolved, source, ts.source_map_line_offset);
    }
    return run_module(source, resolved);
  }

  run_result host::run_module(std::string_view source, std::string_view origin) {
    auto* iso = p_->isolate;
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    auto ctx = p_->context.Get(iso);
    v8::Context::Scope cs(ctx);

    v8::TryCatch tc(iso);

    // Build a module-flavoured ScriptOrigin.
    auto org = str(iso, origin);
    v8::ScriptOrigin sorigin(org, /*line_offset*/ 0, /*col*/ 0,
                             /*shared_cross_origin*/ false, /*script_id*/ -1,
                             source_map_url_value(iso, source), /*opaque*/ false,
                             /*is_wasm*/ false, /*is_module*/ true);
    std::string cache_id;
    if (origin != "<inline>")
      cache_id = "fxe-mod:" + std::string(origin);

    v8::Local<v8::Module> mod;
    if (!v8_code_cache::compile_module(iso, cache_id, source, sorigin).ToLocal(&mod)) {
      std::string msg = "module compile error: " + to_std_string(iso, tc.Exception());
      if (auto m = tc.Message(); !m.IsEmpty()) {
        msg += " @ " + to_std_string(iso, m->Get());
      }
      return {false, std::move(msg)};
    }

    if (origin != "<inline>") {
      auto path = normalize_module_path(std::filesystem::path(std::string(origin)));
      erase_cached_module(iso, p_.get(), path, false);
      p_->clear_import_edges(path);
      std::error_code ec;
      auto mtime = std::filesystem::last_write_time(path, ec);
      p_->module_cache[path] = host::impl::module_cache_entry{
          v8::Global<v8::Module>(iso, mod), ec ? std::filesystem::file_time_type{} : mtime};
      module_path_table()[*mod] = path;
    }

    v8::Maybe<bool> instantiated = mod->InstantiateModule(ctx, &resolve_module);
    if (instantiated.IsNothing() || !instantiated.FromJust()) {
      std::string msg = "module instantiation failed";
      if (tc.HasCaught()) {
        msg += ": " + to_std_string(iso, tc.Exception());
      }
      return {false, std::move(msg)};
    }

    v8::Local<v8::Value> eval_result;
    if (!mod->Evaluate(ctx).ToLocal(&eval_result)) {
      std::string msg = "module evaluation error: " + to_std_string(iso, tc.Exception());
      v8::Local<v8::Value> stack;
      if (tc.StackTrace(ctx).ToLocal(&stack))
        msg += "\n" + to_std_string(iso, stack);
      return {false, std::move(msg)};
    }

    // Module::Evaluate returns a Promise. Drain microtasks so any synchronous
    // side effects (and short top-level awaits) settle before we report status.
    if (eval_result->IsPromise()) {
      auto promise = eval_result.As<v8::Promise>();
      auto state = pump_until_settled(iso, ctx, promise);
      if (state == v8::Promise::kRejected) {
        std::string msg = "module top-level rejection: " +
                          console_arg_to_string(iso, ctx, promise->Result(), true);
        return {false, std::move(msg)};
      }
      if (state == v8::Promise::kPending) {
        return {false, "module top-level await did not settle within budget"};
      }
    }
    return {true, std::string{}};
  }

  run_result host::run_preload_file(const std::filesystem::path& path, bool as_module) {
    std::string resolved = normalize_module_path(path);
    std::string source;
    if (!read_text_file(resolved, source))
      return {false, "preload file not found: " + path.string()};
    if (is_typescript_path(path)) {
      auto* iso = p_->isolate;
      v8::Isolate::Scope is(iso);
      v8::HandleScope hs(iso);
      auto ts = transpile_typescript(iso, source, resolved);
      if (!ts.ok)
        return {false, "TypeScript transpile error: " + ts.message};
      source = std::move(ts.source);
      register_source_map_for(resolved, source, ts.source_map_line_offset);
    }
    return as_module ? run_module(source, resolved) : run_script(source, resolved);
  }

  run_result host::run_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return {false, "file not found: " + path.string()};
    std::ostringstream buf;
    buf << in.rdbuf();
    auto src = buf.str();
    const auto origin = path.lexically_normal().string();
    if (is_typescript_path(path)) {
      auto* iso = p_->isolate;
      v8::Isolate::Scope is(iso);
      v8::HandleScope hs(iso);
      auto ts = transpile_typescript(iso, src, origin);
      if (!ts.ok)
        return {false, "TypeScript transpile error: " + ts.message};
      src = std::move(ts.source);
      register_source_map_for(origin, src, ts.source_map_line_offset);
    }
    if (looks_like_module(src))
      return run_module(src, origin);
    return run_script(src, origin);
  }
} // namespace fxe::js
