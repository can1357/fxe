// Performance.timeline binding — exposes `performance.timeline.{beginMark,
// endMark, snapshot}` to JavaScript and a C++ accessor used by the debug
// protocol's `Performance.timeline` method and the per-frame counter sampler.
//
// Wired by v8_host AFTER the bare `performance` object is installed:
//
//   install_performance_global(iso, ctx->Global());
//
// The store lives in process-global state guarded by a mutex; both threads
// (render & dispatch) read snapshots, JS only ever writes via beginMark /
// endMark on the render thread.

#pragma once

#include <fxe/types.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include <v8.h>

namespace fxe::js {

  // Install / overwrite `performance.timeline` on the live global object.
  // Idempotent. Must be called inside the target context's scope.
  void install_performance_global(v8::Isolate* iso, v8::Local<v8::Object> global);

  // Manual record APIs (used by C++ side to log frame timings).
  void perf_record_sample(std::string_view name, double ms) noexcept;

  // Snapshot of the timeline store. Schema:
  //   { marks: { name: { count, totalMs, lastMs, minMs, maxMs } } }
  nlohmann::ordered_json performance_timeline_snapshot();

  // Reset all counters (useful for tests / between runs).
  void performance_timeline_reset() noexcept;

} // namespace fxe::js
