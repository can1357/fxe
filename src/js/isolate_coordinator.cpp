#include "isolate_coordinator.hpp"

#include <fxe/v8_host.hpp>

#include <cstdio>
#include <exception>
#include <vector>

namespace {
  thread_local uint64_t g_current_runtime_id = 0;
}

namespace fxe::js {
  void isolate_coordinator::runtime_thread_main(runtime_state* state) {
    g_current_runtime_id = state ? state->id : 0;
    if (!state || state->id == 0)
      return;

    try {
      host runtime_host(host::bootstrap_mode::window_thread, state->id);
      {
        std::lock_guard lock(state->mu);
        state->started = true;
        state->init_ok = true;
      }
      state->cv.notify_all();

      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock lock(state->mu);
          state->cv.wait(lock, [&] { return state->stopping.load() || !state->queue.empty(); });
          if (state->stopping.load() && state->queue.empty())
            break;
          task = std::move(state->queue.front());
          state->queue.pop_front();
        }
        try {
          if (task)
            task();
        } catch (...) {
          // v1: keep the runtime alive even if a queued task throws.
        }
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "fxe: failed to start window isolate runtime %llu: %s\n",
                   static_cast<unsigned long long>(state->id), e.what());
      {
        std::lock_guard lock(state->mu);
        state->started = true;
        state->init_ok = false;
        state->stopping.store(true, std::memory_order_release);
      }
      state->cv.notify_all();
    } catch (...) {
      std::fprintf(stderr, "fxe: failed to start window isolate runtime %llu\n",
                   static_cast<unsigned long long>(state->id));
      {
        std::lock_guard lock(state->mu);
        state->started = true;
        state->init_ok = false;
        state->stopping.store(true, std::memory_order_release);
      }
      state->cv.notify_all();
    }

    g_current_runtime_id = 0;
  }

  isolate_coordinator& isolate_coordinator::get() {
    static isolate_coordinator coordinator;
    return coordinator;
  }

  isolate_coordinator::~isolate_coordinator() {
    stop_all_runtimes();
  }

  uint64_t isolate_coordinator::current_isolate_id() const {
    if (g_current_runtime_id != 0)
      return g_current_runtime_id;
    thread_local const uint64_t isolate_id = next_id_.fetch_add(1, std::memory_order_relaxed);
    return isolate_id;
  }

  uint64_t isolate_coordinator::spawn_window_runtime() {
    auto state = std::make_unique<runtime_state>();
    state->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    auto* raw = state.get();
    {
      std::lock_guard lock(runtimes_mu_);
      runtimes_.emplace(state->id, std::move(state));
    }

    try {
      raw->thread = std::thread([raw] { runtime_thread_main(raw); });
    } catch (const std::exception& e) {
      std::fprintf(stderr, "fxe: failed to spawn window isolate thread %llu: %s\n",
                   static_cast<unsigned long long>(raw->id), e.what());
      std::lock_guard lock(runtimes_mu_);
      runtimes_.erase(raw->id);
      return 0;
    } catch (...) {
      std::fprintf(stderr, "fxe: failed to spawn window isolate thread %llu\n",
                   static_cast<unsigned long long>(raw->id));
      std::lock_guard lock(runtimes_mu_);
      runtimes_.erase(raw->id);
      return 0;
    }

    bool init_ok = false;
    {
      std::unique_lock lock(raw->mu);
      raw->cv.wait(lock, [&] { return raw->started; });
      init_ok = raw->init_ok;
    }
    if (init_ok)
      return raw->id;

    stop_runtime(raw->id);
    return 0;
  }

  bool isolate_coordinator::post_task(uint64_t runtime_id, std::function<void()> task) {
    runtime_state* state = nullptr;
    {
      std::lock_guard lock(runtimes_mu_);
      auto it = runtimes_.find(runtime_id);
      if (it == runtimes_.end())
        return false;
      state = it->second.get();
    }

    {
      std::lock_guard lock(state->mu);
      if (state->stopping.load(std::memory_order_acquire))
        return false;
      state->queue.push_back(std::move(task));
    }
    state->cv.notify_one();
    return true;
  }

  bool isolate_coordinator::stop_runtime(uint64_t runtime_id) {
    std::unique_ptr<runtime_state> state;
    {
      std::lock_guard lock(runtimes_mu_);
      auto it = runtimes_.find(runtime_id);
      if (it == runtimes_.end())
        return false;
      state = std::move(it->second);
      runtimes_.erase(it);
    }

    state->stopping.store(true, std::memory_order_release);
    state->cv.notify_all();
    if (state->thread.joinable())
      state->thread.join();
    return true;
  }

  void isolate_coordinator::stop_all_runtimes() {
    std::vector<uint64_t> runtime_ids;
    {
      std::lock_guard lock(runtimes_mu_);
      runtime_ids.reserve(runtimes_.size());
      for (const auto& [id, _] : runtimes_)
        runtime_ids.push_back(id);
    }
    for (uint64_t id : runtime_ids)
      (void)stop_runtime(id);
  }
} // namespace fxe::js
