#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fxe::js {
  class isolate_coordinator {
  public:
    static isolate_coordinator& get();
    ~isolate_coordinator();

    // Returns a stable ID for the current isolate.
    uint64_t current_isolate_id() const;

    // Spawn a per-window runtime thread that owns its own V8 isolate.
    // Returns 0 when the runtime could not be created.
    uint64_t spawn_window_runtime();

    // Post a task onto the target runtime's queue. Tasks run on the runtime thread.
    bool post_task(uint64_t runtime_id, std::function<void()> task);

    // Stop a runtime cleanly: pending queued tasks drain before the thread exits.
    bool stop_runtime(uint64_t runtime_id);

  private:
    struct runtime_state {
      std::thread thread;
      std::mutex mu;
      std::condition_variable cv;
      std::deque<std::function<void()>> queue;
      std::atomic<bool> stopping{false};
      bool started = false;
      bool init_ok = false;
      uint64_t id = 0;
    };

    isolate_coordinator() = default;
    void stop_all_runtimes();
    static void runtime_thread_main(runtime_state* state);

    mutable std::atomic<uint64_t> next_id_{1};
    std::mutex runtimes_mu_;
    std::unordered_map<uint64_t, std::unique_ptr<runtime_state>> runtimes_;
  };
} // namespace fxe::js
