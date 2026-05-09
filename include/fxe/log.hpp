// Thin spdlog wrapper for category-based logging across fxe.
//
// Categories are dotted strings ("font.atlas", "font.cache", "wgpu.renderer").
// The first call to `get(name)` registers a logger with the global stderr
// sink and inherits the level from spdlog's environment configuration (see
// SPDLOG_LEVEL below). Subsequent calls return the cached logger handle.
//
// Configuration is via the SPDLOG_LEVEL env var, parsed once on first use:
//
//   SPDLOG_LEVEL=info                          # global default
//   SPDLOG_LEVEL=warn,font=debug               # global warn, font=debug
//   SPDLOG_LEVEL=off,font.atlas=trace          # silent except font.atlas
//   SPDLOG_LEVEL=info,font=debug,wgpu=trace    # mix and match
//
// FXE_LOG_LEVELS is a shorter alias for SPDLOG_LEVEL recognized at init
// time. If both are set, SPDLOG_LEVEL wins.
//
// Usage at call sites:
//
//   #include <fxe/log.hpp>
//   FXE_TRACE("font.cache", "new_glyph face={} gid={}", face_id, gid);
//   FXE_DEBUG("font.atlas", "atlas_grow w={} h={}", w, h);
//   FXE_WARN ("wgpu.renderer", "blur intermediate alloc failed");
//   FXE_ERROR("wgpu.renderer", "RequestDevice failed: {}", msg);
//
// Each macro caches the resolved logger in a function-local static at the
// call site (C++17 magic statics; thread-safe one-shot init), then defers
// to spdlog's runtime level gate. When the level is disabled, formatting
// of the args is skipped — only their evaluation cost remains, which for
// the typical "category, fmt, atomic-loads" pattern is essentially free.
//
// Levels (verbosity): trace < debug < info < warn < err < critical.
// Pick `debug` for opt-in diagnostic streams (eviction, repacks, frame
// begin/end); reserve `info` for one-shot startup messages and `warn`/
// `err` for unexpected states. Keep hot-loop logs at `trace`.

#pragma once

// Compile in every level so the runtime env-var routing controls visibility.
// Release builds can override this on the command line with
// -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO to elide trace/debug entirely.
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

#include <string_view>

namespace fxe::log {

  // Resolve (or lazily register) the logger for `category`. Always returns
  // a valid logger; never null. Thread-safe after first call. Hits a mutex
  // + hashmap lookup; prefer the FXE_<level>(category, ...) macros at call
  // sites for hot paths.
  spdlog::logger& get(std::string_view category);

  // Force-initialise the logging subsystem. Optional — `get()` will lazily
  // initialise on first call. Safe to call multiple times; idempotent.
  // Reads SPDLOG_LEVEL / FXE_LOG_LEVELS from the environment.
  void init();

} // namespace fxe::log

// Cached logger handle for a given category. Resolved once per call site
// via a function-local static; subsequent uses are a relaxed load. INTERNAL
// — call sites should use FXE_TRACE / FXE_DEBUG / etc. instead of this.
#define FXE_LOG_HANDLE_(category)                                                                  \
  ([]() -> ::spdlog::logger& {                                                                     \
    static ::spdlog::logger& _fxe_log_ref = ::fxe::log::get(category);                             \
    return _fxe_log_ref;                                                                           \
  }())

// Level-tagged logging macros. `category` MUST be a string literal so the
// per-call-site cache collapses to one static. spdlog's own SPDLOG_LOGGER_*
// macros handle the should_log() gate (skipping argument formatting when
// the level is off) and respect SPDLOG_ACTIVE_LEVEL for compile-time
// elision in release builds.
//
//   FXE_TRACE("font.cache", "new_glyph gid={}", gid);
//   FXE_WARN ("wgpu.renderer", "fallback path: {}", reason);
#define FXE_TRACE(category, ...) SPDLOG_LOGGER_TRACE(&FXE_LOG_HANDLE_(category), __VA_ARGS__)
#define FXE_DEBUG(category, ...) SPDLOG_LOGGER_DEBUG(&FXE_LOG_HANDLE_(category), __VA_ARGS__)
#define FXE_INFO(category, ...) SPDLOG_LOGGER_INFO(&FXE_LOG_HANDLE_(category), __VA_ARGS__)
#define FXE_WARN(category, ...) SPDLOG_LOGGER_WARN(&FXE_LOG_HANDLE_(category), __VA_ARGS__)
#define FXE_ERROR(category, ...) SPDLOG_LOGGER_ERROR(&FXE_LOG_HANDLE_(category), __VA_ARGS__)
#define FXE_CRITICAL(category, ...) SPDLOG_LOGGER_CRITICAL(&FXE_LOG_HANDLE_(category), __VA_ARGS__)
