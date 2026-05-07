#include "runtime/uv_loop.hpp"

#if FXE_HAS_LIBUV
#include <uv.h>
#endif

namespace fxe::runtime {

#if FXE_HAS_LIBUV
  namespace {
    void close_if_open(uv_handle_t* handle, void*) noexcept {
      if (!uv_is_closing(handle)) {
        uv_close(handle, nullptr);
      }
    }

    void run_after_init_unref(uv_handle_t* handle) noexcept {
      if (handle != nullptr) {
        uv_unref(handle);
      }
    }
  } // namespace
#endif

  uv_loop_runtime& uv_loop_runtime::instance() noexcept {
    static uv_loop_runtime runtime;
    return runtime;
  }

  uv_loop_runtime::uv_loop_runtime() noexcept {
#if FXE_HAS_LIBUV
    init_status_ = uv_loop_init(&loop_);
    if (init_status_ != 0) {
      return;
    }
    loop_ready_ = true;

    async_.data = this;
    init_status_ = uv_async_init(&loop_, &async_, &uv_loop_runtime::async_dispatch);
    if (init_status_ != 0) {
      return;
    }
    async_ready_ = true;
    run_after_init_unref(reinterpret_cast<uv_handle_t*>(&async_));
#else
    init_status_ = 0;
#endif
  }

  uv_loop_runtime::~uv_loop_runtime() noexcept {
    shutdown();
  }

  uv_loop_t* uv_loop_runtime::loop() noexcept {
#if FXE_HAS_LIBUV
    return init_status_ == 0 && loop_ready_ && !closed_.load() ? &loop_ : nullptr;
#else
    return nullptr;
#endif
  }

  int uv_loop_runtime::pump_nowait() noexcept {
#if FXE_HAS_LIBUV
    if (init_status_ != 0) {
      return init_status_;
    }
    if (!loop_ready_ || closed_.load() || stopping_.load()) {
      return 0;
    }
    run_pump_callbacks();
    const int rc = uv_run(&loop_, UV_RUN_NOWAIT);
    run_microtask_checkpoint();
    return rc;
#else
    return 0;
#endif
  }

  int uv_loop_runtime::pump_nowait(unsigned max_iterations) noexcept {
    if (max_iterations == 0) {
      return pump_nowait();
    }

    int last = 0;
    for (unsigned i = 0; i < max_iterations; ++i) {
      last = pump_nowait();
      if (last <= 0) {
        break;
      }
    }
    return last;
  }

  bool uv_loop_runtime::available() const noexcept {
#if FXE_HAS_LIBUV
    return init_status_ == 0 && loop_ready_ && !closed_.load();
#else
    return false;
#endif
  }

  std::size_t uv_loop_runtime::register_pump_callback(pump_callback cb) {
    if (!cb || !available()) {
      return 0;
    }
    std::lock_guard<std::mutex> lk(callbacks_mu_);
    const std::size_t id = next_callback_id_++;
    callbacks_.emplace_back(id, std::move(cb));
    return id;
  }

  void uv_loop_runtime::unregister_pump_callback(std::size_t id) noexcept {
    if (id == 0) {
      return;
    }
    std::lock_guard<std::mutex> lk(callbacks_mu_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (it->first == id) {
        callbacks_.erase(it);
        return;
      }
    }
  }

  void uv_loop_runtime::run_pump_callbacks() noexcept {
    std::vector<pump_callback> callbacks;
    {
      std::lock_guard<std::mutex> lk(callbacks_mu_);
      callbacks.reserve(callbacks_.size());
      for (auto& entry : callbacks_) {
        callbacks.push_back(entry.second);
      }
    }
    for (auto& cb : callbacks) {
      try {
        cb();
      } catch (...) {
      }
    }
  }

  std::size_t uv_loop_runtime::register_microtask_checkpoint(microtask_checkpoint cb) {
    if (!cb) {
      return 0;
    }
    std::lock_guard<std::mutex> lk(microtask_mu_);
    const std::size_t id = next_microtask_checkpoint_id_++;
    microtask_checkpoints_.emplace_back(id, std::move(cb));
    return id;
  }

  void uv_loop_runtime::unregister_microtask_checkpoint(std::size_t id) noexcept {
    if (id == 0) {
      return;
    }
    std::lock_guard<std::mutex> lk(microtask_mu_);
    for (auto it = microtask_checkpoints_.begin(); it != microtask_checkpoints_.end(); ++it) {
      if (it->first == id) {
        microtask_checkpoints_.erase(it);
        return;
      }
    }
  }

  void uv_loop_runtime::run_microtask_checkpoint() noexcept {
    std::vector<microtask_checkpoint> callbacks;
    {
      std::lock_guard<std::mutex> lk(microtask_mu_);
      callbacks.reserve(microtask_checkpoints_.size());
      for (auto& entry : microtask_checkpoints_) {
        callbacks.push_back(entry.second);
      }
    }
    for (auto& cb : callbacks) {
      try {
        cb();
      } catch (...) {
      }
    }
  }

  void uv_loop_runtime::post(pump_callback cb) {
#if FXE_HAS_LIBUV
    if (!cb || init_status_ != 0 || !loop_ready_) {
      return;
    }
    std::lock_guard<std::mutex> lk(work_mu_);
    if (stopping_.load() || closed_.load()) {
      return;
    }
    work_.push_back(std::move(cb));
    if (async_ready_) {
      (void)uv_async_send(&async_);
    }
#else
    (void)cb;
#endif
  }

  void uv_loop_runtime::shutdown() noexcept {
#if FXE_HAS_LIBUV
    if (closed_.exchange(true)) {
      return;
    }
    stopping_.store(true);
    {
      std::lock_guard<std::mutex> lk(work_mu_);
      work_.clear();
    }
    if (!loop_ready_) {
      return;
    }

    uv_walk(&loop_, close_if_open, nullptr);
    (void)uv_run(&loop_, UV_RUN_DEFAULT);
    (void)uv_loop_close(&loop_);
    loop_ready_ = false;
    async_ready_ = false;
#endif
  }

  void uv_loop_runtime::drain_posted_callbacks() noexcept {
#if FXE_HAS_LIBUV
    std::deque<pump_callback> callbacks;
    {
      std::lock_guard<std::mutex> lk(work_mu_);
      callbacks.swap(work_);
    }
    for (auto& cb : callbacks) {
      try {
        cb();
      } catch (...) {
      }
    }
#endif
  }

#if FXE_HAS_LIBUV
  void uv_loop_runtime::async_dispatch(uv_async_t* handle) noexcept {
    if (!handle || !handle->data) {
      return;
    }
    static_cast<uv_loop_runtime*>(handle->data)->drain_posted_callbacks();
  }
#endif

  int uv_loop_runtime::init_status() const noexcept {
    return init_status_;
  }

#if FXE_HAS_LIBUV
  uv_loop_t* default_loop() {
    return uv_loop_runtime::instance().loop();
  }

  void unref_by_default(uv_handle_t* handle) noexcept {
    if (handle != nullptr) {
      uv_unref(handle);
    }
  }

  void set_handle_ref(uv_handle_t* handle, bool ref) noexcept {
    if (handle == nullptr) {
      return;
    }
    if (ref) {
      uv_ref(handle);
    } else {
      uv_unref(handle);
    }
  }

  void pump_nonblocking() {
    (void)uv_loop_runtime::instance().pump_nowait();
  }

  void post_to_loop(std::function<void()> cb) {
    uv_loop_runtime::instance().post(std::move(cb));
  }

  void shutdown_loop() {
    uv_loop_runtime::instance().shutdown();
  }
#endif
} // namespace fxe::runtime
