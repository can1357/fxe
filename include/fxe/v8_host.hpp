#pragma once

#include <filesystem>
#include <functional>
#include <fxe/types.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fxe {
  class window;
  class renderer;
} // namespace fxe

namespace fxe::js {
  struct run_result {
    bool ok = false;
    std::string message;
  };

  // One-time process initialiser. Pass argv[0] when known so V8 can find ICU /
  // snapshot blobs that ship next to the executable. Optionally point V8 at a
  // specific icudtl.dat (overrides the argv0-relative search).
  void initialize(std::string_view argv0 = {}, std::string_view icudtl_path = {});
  void shutdown() noexcept;

  // Free-standing helpers callable from any thread that already holds the
  // isolate. `idle_notification` gives V8 a deadline (seconds, monotonic) to
  // perform incremental GC work; call from frame-loop slack time. `notify_*`
  // forward to v8::Isolate::MemoryPressureNotification — invoke when the
  // window is hidden / minimised so V8 trims the heap.
  void idle_notification(double budget_seconds);
  void notify_memory_pressure_moderate();
  void notify_memory_pressure_critical();

  // ----- Debug protocol surface ---------------------------------------------
  // These run on the same thread as the rest of the host API (the render
  // thread). Provided here so the fxe_debug library can poke V8 state without
  // having to know about the v8.h API.
  struct exception_position {
    bool has_position = false;
    std::string url;
    int line_number = 0;   // CDP-style 0-based
    int column_number = 0; // CDP-style 0-based
    bool has_original_position = false;
    std::string original_url;
    int original_line_number = 0;   // CDP-style 0-based
    int original_column_number = 0; // CDP-style 0-based
  };

  struct eval_result {
    std::string json_value; // JSON representation of the result, "" for null
    std::string exception;  // empty when no exception
    exception_position position;
  };

  class host {
  public:
    enum class bootstrap_mode { main_thread, worker_thread, window_thread };

    // Main-thread host bootstrap installs the full renderer/window binding set.
    // Worker bootstrap owns an isolate/context on the calling worker thread and
    // deliberately skips Window/Renderer/App/GLFW-facing bindings.
    // Window-thread bootstrap is the B1 per-window runtime: it owns a dedicated
    // isolate on the calling thread, but still installs the same bindings as
    // main_thread until GLFW/render marshaling moves off the shared path.
    explicit host(bootstrap_mode mode = bootstrap_mode::main_thread);
    host(bootstrap_mode mode, uint64_t runtime_id);
    ~host();
    host(const host&) = delete;
    host& operator=(const host&) = delete;

    // Make these globals available inside the script's global object before run.
    void install_window_global(window& win);
    void install_renderer_global(renderer& r);

    // Classic-script entry points. Top-level `import`/`export` are NOT supported.
    run_result run_script_file(const std::filesystem::path& path);
    run_result run_script(std::string_view source, std::string_view origin = "<inline>");

    // ES-module entry points. The synthetic `fxe` specifier resolves to a built-in
    // module that re-exports `Window`, `Renderer`, `Primitives`, `CommandBuffer`.
    // Other specifiers throw at instantiation time.
    run_result run_module_file(const std::filesystem::path& path);
    run_result run_module(std::string_view source, std::string_view origin = "<inline>");
    // Preload files run in the current isolate/context without changing the host entry path.
    run_result run_preload_file(const std::filesystem::path& path, bool as_module);

    // Auto-detects classic vs module by scanning the source for top-level
    // `import`/`export`. Convenience entry point used by the CLI runner.
    run_result run_file(const std::filesystem::path& path);

    // Evaluate `expression` in the host's context. When `return_by_value` is
    // true, the result is converted to JSON; otherwise an opaque preview
    // string is returned.
    eval_result debug_evaluate(std::string_view expression, bool return_by_value = true);

    // CPU profile payload serialized in V8/CDP JSON form.
    struct cpu_profile_result {
      bool ok = false;
      std::string profile_json;
      std::string message;
    };

    // CDP Profiler.* backing hooks. enable/disable are idempotent; sampling
    // interval defaults to 1000 µs (1 kHz); pass a different value to override.
    void debug_profiler_enable(int sampling_interval_us = 1000);
    void debug_profiler_disable();
    cpu_profile_result debug_profiler_start(int sampling_interval_us = 1000);
    cpu_profile_result debug_profiler_stop();

    // Returns a sorted list of own enumerable property names on the global.
    std::vector<std::string> debug_global_keys();

    // Force a V8 heap collection for debug tooling. The host enables V8's
    // --expose_gc flag before initialisation so RequestGarbageCollectionForTesting
    // is valid; LowMemoryNotification is also sent as the production-safe signal.
    void debug_collect_garbage();

    // Serialise a V8 heap snapshot as JSON chunks. The callback runs synchronously
    // on the V8/render thread and must not re-enter debug dispatch.
    using heap_snapshot_chunk_sink = std::function<void(std::string_view)>;
    void debug_take_heap_snapshot(const heap_snapshot_chunk_sink& sink);

    // Invoke globalThis.__fxe_hmr.fire(path) and report the number of handlers
    // called. This is the native bridge for future file-watcher integration.
    run_result fire_hmr(std::string_view path, int& handlers_called);
    // File-backed modules currently resident in the module cache. Used by the
    // runner's --watch mode to attach platform file watchers only to modules
    // that can actually participate in HMR.
    [[nodiscard]] std::vector<std::string> loaded_module_paths() const;

    // Evict the module at `path` (and its transitive importers) from the module
    // cache. Returns the list of evicted normalized paths.
    std::vector<std::string> invalidate_module(std::string_view path);

    // Reimport `path` (and re-instantiate transitively). Returns ok=false with a
    // message if the source can't be loaded or instantiated.
    run_result reimport_module(std::string_view path);

    // Forward console output (or any side-channel event). Used by the debug
    // server to push Console.messageAdded events.
    using console_sink = void (*)(void* user, std::string_view level, std::string_view text);
    void set_console_sink(console_sink sink, void* user) noexcept;

    // Attach a debug pump callback. The win.run binding invokes this once per
    // loop iteration to drain any queued debug requests. Pass `nullptr` to
    // detach. The callback is also called while the script is paused at a
    // Debugger.pause / --debug-pause boundary.
    using debug_pump = void (*)(void* user);
    using debug_paused = bool (*)(void* user);
    void attach_debug_pump(debug_pump pump, debug_paused is_paused, void* user) noexcept;

    // Invoked by the bindings; safe to call even when no pump is attached.
    void pump_debug_tasks();
    [[nodiscard]] bool debug_is_paused() const noexcept;

    // Multi-window registry. JS bindings register windows/renderers when
    // `new Window(...)` / `new Renderer(win)` runs and unregister in the GC
    // finaliser or close path. `active_window`/`active_renderer` keep working
    // as legacy single-window helpers — they return the FIRST registered
    // entry (or nullptr).
    [[nodiscard]] window* active_window() const noexcept;
    [[nodiscard]] renderer* active_renderer() const noexcept;

    [[nodiscard]] std::vector<window*> windows() const;
    [[nodiscard]] std::vector<renderer*> renderers() const;
    [[nodiscard]] window* window_at(usize index) const noexcept;
    [[nodiscard]] renderer* renderer_for(window* w) const noexcept;
    [[nodiscard]] usize window_index(window* w) const noexcept;

    void register_window(window* w) noexcept;
    void unregister_window(window* w) noexcept;
    void register_renderer(window* owner, renderer* r) noexcept;
    void unregister_renderer(renderer* r) noexcept;

    // Re-entrancy guard for the JS event loop driver. The first
    // Window.run/App.run sets this to true; nested calls return immediately.
    [[nodiscard]] bool is_app_running() const noexcept;
    void set_app_running(bool running) noexcept;

  public:
    struct impl;

  private:
    std::unique_ptr<impl> p_;
  };
} // namespace fxe::js
