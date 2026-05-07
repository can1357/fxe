#pragma once

#ifndef FXE_HAS_LIBUV
#define FXE_HAS_LIBUV 0
#endif

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#if FXE_HAS_LIBUV
#include <uv.h>
#else
struct uv_loop_s;
using uv_loop_t = uv_loop_s;
#endif

namespace fxe::runtime {

  class uv_loop_runtime final {
  public:
    static uv_loop_runtime& instance() noexcept;

    uv_loop_runtime(const uv_loop_runtime&) = delete;
    uv_loop_runtime& operator=(const uv_loop_runtime&) = delete;

    // Native loop exposed for libuv-backed adapters (node:net, timers, DNS).
    // Callers must not block on it directly; app/frame loops should use
    // pump_nowait() so JS microtasks and registered polling clients remain in
    // control of scheduling.
    uv_loop_t* loop() noexcept;

    // Run pending libuv work without blocking. Return value mirrors
    // uv_run(UV_RUN_NOWAIT): >0 while active handles remain, 0 when idle, or
    // the libuv initialisation error when the loop is unavailable.
    int pump_nowait() noexcept;

    // Bounded helper for app loops that want to drain a short burst without
    // risking an unbounded frame. max_iterations == 0 performs one nowait pass.
    int pump_nowait(unsigned max_iterations) noexcept;
    bool available() const noexcept;
    int init_status() const noexcept;

    // Register short, non-blocking work that should advance on each runtime
    // loop pump. Callbacks run on the pumping thread before uv_run(); they must
    // not call pump_nowait() recursively. Returns 0 when unavailable.
    using pump_callback = std::function<void()>;
    std::size_t register_pump_callback(pump_callback cb);
    using microtask_checkpoint = std::function<void()>;
    std::size_t register_microtask_checkpoint(microtask_checkpoint cb);
    void unregister_microtask_checkpoint(std::size_t id) noexcept;
    void unregister_pump_callback(std::size_t id) noexcept;
    void run_pump_callbacks() noexcept;
    void run_microtask_checkpoint() noexcept;
    void post(pump_callback cb);
    void shutdown() noexcept;
    void drain_posted_callbacks() noexcept;
#if FXE_HAS_LIBUV
    static void async_dispatch(uv_async_t* handle) noexcept;
#endif

  private:
    uv_loop_runtime() noexcept;
    ~uv_loop_runtime() noexcept;

#if FXE_HAS_LIBUV
    uv_loop_t loop_{};
    uv_async_t async_{};
    bool loop_ready_ = false;
    bool async_ready_ = false;
    std::atomic_bool stopping_{false};
    std::atomic_bool closed_{false};
    std::mutex work_mu_;
    std::deque<pump_callback> work_;
#endif
    int init_status_ = 0;
    std::mutex callbacks_mu_;
    std::vector<std::pair<std::size_t, pump_callback>> callbacks_;
    std::size_t next_callback_id_ = 1;
    std::mutex microtask_mu_;
    std::vector<std::pair<std::size_t, microtask_checkpoint>> microtask_checkpoints_;
    std::size_t next_microtask_checkpoint_id_ = 1;
  };

#if FXE_HAS_LIBUV
  uv_loop_t* default_loop();
  void unref_by_default(uv_handle_t* handle) noexcept;
  void set_handle_ref(uv_handle_t* handle, bool ref) noexcept;
  void pump_nonblocking();
  void post_to_loop(std::function<void()> cb);
  void shutdown_loop();
#else
  inline uv_loop_t* default_loop() {
    return nullptr;
  }
  inline void pump_nonblocking() {}
  inline void post_to_loop(std::function<void()>) {}
  inline void shutdown_loop() {}
#endif

} // namespace fxe::runtime
