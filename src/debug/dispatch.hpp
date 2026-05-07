// Dispatch layer: maps method names to handler functions that run on the
// render thread. Handlers receive a context with pointers to the attached
// host/window/renderer plus the parsed request, and produce a `result`
// (success payload) or throw `dispatch_error` to surface a JSON-RPC error.

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace fxe {
  class window;
  class renderer;
} // namespace fxe

namespace fxe::js {
  class host;
}

namespace fxe::debug {
  using json = nlohmann::ordered_json;
  class server;

  // JSON-RPC standard codes plus engine-defined range -32000..-32099.
  enum class err_code : int {
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602,
    internal = -32603,
    script_throw = -32000,
    capture_failed = -32001,
    service_unavailable = -32002,
    detached = service_unavailable,
    server_busy = -32003,
  };

  struct dispatch_error {
    err_code code;
    std::string message;
    std::string data; // optional
  };

  struct dispatch_context {
    server* srv = nullptr;
    js::host* host = nullptr;
    window* win = nullptr;
    renderer* rdr = nullptr;
  };

  // Looks up the method, runs the handler, returns the result payload as a
  // JSON value. Throws `dispatch_error` on failure.
  json dispatch(dispatch_context& cx, std::string_view method, const json& params);

  // True iff `method` is a known method (used to validate before queueing).
  bool method_exists(std::string_view method);

  // V8-dependent handlers are installed by fxe_js at startup so fxe_debug
  // doesn't link against libv8.
  struct runtime_handlers {
    json (*evaluate)(dispatch_context&, const json&) = nullptr;
    json (*get_globals)(dispatch_context&, const json&) = nullptr;
    json (*fire_hmr)(dispatch_context&, const json&) = nullptr;
    json (*invalidate_module)(dispatch_context&, const json&) = nullptr;
    json (*reimport_module)(dispatch_context&, const json&) = nullptr;
    // Optional: returns the merged Performance.timeline + RenderStats payload.
    json (*performance_snapshot)(dispatch_context&, const json&) = nullptr;
    // Optional: invokes globalThis.__fxeReconcilerSnapshot() when fxe-ui is loaded.
    json (*reconciler_snapshot)(dispatch_context&, const json&) = nullptr;
  };
  void set_runtime_handlers(runtime_handlers h) noexcept;
  runtime_handlers get_runtime_handlers() noexcept;

  // V8 Profiler handlers are installed by the V8 host when available. This keeps
  // fxe_debug independent of V8 headers while letting dispatch.cpp expose the
  // CDP method table. src/js/v8_host.cpp must call set_profiler_handlers() at
  // host init to provide real v8::CpuProfiler-backed implementations.
  struct profiler_handlers {
    std::function<json(dispatch_context&, const json&)> enable;
    std::function<json(dispatch_context&, const json&)> disable;
    std::function<json(dispatch_context&, const json&)> start;
    std::function<json(dispatch_context&, const json&)> stop;
    std::function<json(dispatch_context&, const json&)> start_precise_coverage;
    std::function<json(dispatch_context&, const json&)> take_precise_coverage;
  };
  void set_profiler_handlers(profiler_handlers h) noexcept;
  // V8 HeapProfiler handlers are separate from CPU Profiler handlers but use the
  // same dispatch registration pattern. Snapshot chunks are streamed as
  // HeapProfiler.addHeapSnapshotChunk events by the handler implementation.
  struct heap_profiler_handlers {
    std::function<json(dispatch_context&, const json&)> enable;
    std::function<json(dispatch_context&, const json&)> disable;
    std::function<json(dispatch_context&, const json&)> take_heap_snapshot;
    std::function<json(dispatch_context&, const json&)> collect_garbage;
  };
  void set_heap_profiler_handlers(heap_profiler_handlers h) noexcept;
  heap_profiler_handlers get_heap_profiler_handlers();
  profiler_handlers get_profiler_handlers();

  // Bridge for non-dispatch C++ surfaces (window/fetch/fs bindings) that want
  // to push protocol events without holding a server pointer. No-op when no
  // server is currently running. Subscriptions are managed via Window.subscribe
  // / Fetch.subscribe / Fs.subscribe; events for unsubscribed channels are
  // dropped here so callers can fire unconditionally.
  enum class event_channel : int {
    window = 0,
    fetch = 1,
    fs = 2,
    perf = 3,
  };
  void emit_event_if_attached(event_channel channel, std::string_view method, json params);
  bool channel_enabled(event_channel channel);
  void set_channel_enabled(event_channel channel, bool enabled);
} // namespace fxe::debug
