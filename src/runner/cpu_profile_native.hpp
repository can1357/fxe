// Native sampling profiler for the fxe runner.
//
// POSIX (macOS + Linux): a dedicated sampler thread periodically signals the
// target thread (the render/main thread that runs V8) with SIGPROF. The
// signal handler captures a stack trace via backtrace(3) into a fixed-size
// ring; the main thread drains and symbolizes the buffer at stop().
//
// Windows: not yet implemented; start() reports the reason via err and
// stop() returns an empty profile.
//
// This is a debug-only tool. backtrace() on glibc allocates lazily on first
// call; we pre-warm before installing the handler so the steady-state path
// is async-signal-safe in practice.
#pragma once

#include "cpu_profile.hpp"

#include <pthread.h>

#include <atomic>
#include <string>
#include <thread>

namespace fxe::runner {

  class native_profiler {
  public:
    native_profiler() = default;
    ~native_profiler();

    native_profiler(const native_profiler&) = delete;
    native_profiler& operator=(const native_profiler&) = delete;

    // Start sampling the *calling* thread at `hz` samples/second. Returns
    // false on failure (err filled). max_samples bounds the in-memory ring;
    // overflow samples are dropped (counted in profile_data::dropped_samples).
    bool start(int hz, std::size_t max_samples, std::string& err);

    // Stop the sampler thread and return a structured profile. Symbol
    // resolution (dladdr + cxa demangle) happens here.
    profile_data stop();

    // True between successful start() and stop().
    [[nodiscard]] bool running() const noexcept {
      return running_;
    }

  private:
    bool running_ = false;
    int hz_ = 1000;
#if defined(__APPLE__) || defined(__linux__)
    pthread_t target_{};
    std::thread sampler_;
    std::atomic<bool> stop_flag_{false};
#endif
  };

} // namespace fxe::runner
