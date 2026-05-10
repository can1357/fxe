#include "bind_timers.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/log.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <utility>
#include <v8.h>
#include <vector>

#include <GLFW/glfw3.h>
#include <fxe/types.hpp>

namespace fxe::js {
  namespace {
    using namespace v8;
    using clock = std::chrono::steady_clock;

    struct timer_entry {
      u64 id = 0;
      clock::time_point deadline{};
      double interval_ms = 0.0; // 0 = one-shot
      bool repeat = false;
      Global<Function> fn;
      std::vector<Global<Value>> args;
    };

    struct heap_node {
      clock::time_point deadline;
      u64 id;
      bool operator>(const heap_node& o) const {
        return deadline > o.deadline;
      }
    };

    struct iso_state {
      // Timers here do not own libuv timer handles. JS-visible work is ref'd
      // by V8 Globals while present in `active`/`raf_queue` and unref'd by
      // Reset() on clear, one-shot completion, or frame dispatch.
      u64 next_id = 1;
      std::unordered_map<u64, std::unique_ptr<timer_entry>> active;
      std::priority_queue<heap_node, std::vector<heap_node>, std::greater<heap_node>> heap;

      // requestAnimationFrame queue. Callbacks queued during dispatch land in
      // `pending`; they migrate to `current` at the start of the next drain.
      u64 next_raf_id = 1;
      std::vector<std::pair<u64, Global<Function>>> raf_queue;
      // Set of cancelled raf ids (in case they're cancelled mid-frame).
      std::vector<u64> raf_cancelled;
    };

    std::mutex g_states_mu;
    std::unordered_map<Isolate*, std::unique_ptr<iso_state>> g_states;

    iso_state& state_for(Isolate* iso) {
      std::lock_guard<std::mutex> lk(g_states_mu);
      auto& slot = g_states[iso];
      if (!slot)
        slot = std::make_unique<iso_state>();
      return *slot;
    }

    void reset_timer_entry(timer_entry& entry) {
      entry.fn.Reset();
      for (auto& arg : entry.args)
        arg.Reset();
      entry.args.clear();
    }

    void timers_reset_for_isolate(Isolate* iso) {
      std::unique_ptr<iso_state> state;
      {
        std::lock_guard<std::mutex> lk(g_states_mu);
        auto it = g_states.find(iso);
        if (it == g_states.end())
          return;
        state = std::move(it->second);
        g_states.erase(it);
      }
      if (!state)
        return;
      for (auto& [_, entry] : state->active) {
        if (entry)
          reset_timer_entry(*entry);
      }
      state->active.clear();
      while (!state->heap.empty())
        state->heap.pop();
      for (auto& [_, fn] : state->raf_queue)
        fn.Reset();
      state->raf_queue.clear();
      state->raf_cancelled.clear();
    }

    void pump_microtasks(Isolate* iso) {
      iso->PerformMicrotaskCheckpoint();
    }
    void schedule_impl(const FunctionCallbackInfo<Value>& info, bool repeat) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "setTimeout(fn, ms, ...args)");
        return;
      }
      double ms = 0.0;
      if (info.Length() >= 2)
        ms = info[1]->NumberValue(ctx).FromMaybe(0.0);
      if (ms < 0.0 || std::isnan(ms))
        ms = 0.0;
      auto& s = state_for(iso);
      auto entry = std::make_unique<timer_entry>();
      entry->id = s.next_id++;
      entry->interval_ms = ms;
      entry->repeat = repeat;
      entry->deadline = clock::now() + std::chrono::microseconds(static_cast<i64>(ms * 1000.0));
      entry->fn.Reset(iso, info[0].As<Function>());
      for (int i = 2; i < info.Length(); ++i)
        entry->args.emplace_back(iso, info[i]);

      u64 id = entry->id;
      s.heap.push({entry->deadline, id});
      s.active.emplace(id, std::move(entry));
      wake_event_loop();
      info.GetReturnValue().Set(to_v8(iso, static_cast<double>(id)));
    }

    void set_timeout_cb(const FunctionCallbackInfo<Value>& info) {
      schedule_impl(info, false);
    }
    void set_interval_cb(const FunctionCallbackInfo<Value>& info) {
      schedule_impl(info, true);
    }
    void set_immediate_cb(const FunctionCallbackInfo<Value>& info) {
      // Equivalent to setTimeout(fn, 0, ...args). Reuse schedule_impl by
      // forging info with ms=0 — easier to just inline.
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "setImmediate(fn, ...args)");
        return;
      }
      auto& s = state_for(iso);
      auto entry = std::make_unique<timer_entry>();
      entry->id = s.next_id++;
      entry->deadline = clock::now();
      entry->fn.Reset(iso, info[0].As<Function>());
      for (int i = 1; i < info.Length(); ++i)
        entry->args.emplace_back(iso, info[i]);
      u64 id = entry->id;
      s.heap.push({entry->deadline, id});
      s.active.emplace(id, std::move(entry));
      wake_event_loop();
      info.GetReturnValue().Set(to_v8(iso, static_cast<double>(id)));
    }

    void clear_timer_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsNumber())
        return;
      u64 id = static_cast<u64>(info[0]->NumberValue(iso->GetCurrentContext()).FromMaybe(0.0));
      auto& s = state_for(iso);
      auto it = s.active.find(id);
      if (it != s.active.end()) {
        // Reset Globals so we don't leak.
        it->second->fn.Reset();
        for (auto& g : it->second->args)
          g.Reset();
        s.active.erase(it);
      }
    }

    void queue_microtask_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "queueMicrotask(fn)");
        return;
      }
      iso->EnqueueMicrotask(info[0].As<Function>());
      iso->PerformMicrotaskCheckpoint();
    }

    void raf_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "requestAnimationFrame(fn)");
        return;
      }
      auto& s = state_for(iso);
      u64 id = s.next_raf_id++;
      s.raf_queue.emplace_back(id, Global<Function>(iso, info[0].As<Function>()));
      info.GetReturnValue().Set(to_v8(iso, static_cast<double>(id)));
    }

    void cancel_raf_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsNumber())
        return;
      u64 id = static_cast<u64>(info[0]->NumberValue(iso->GetCurrentContext()).FromMaybe(0.0));
      auto& s = state_for(iso);
      // Remove pending if still in raf_queue (not yet dispatched).
      auto it = std::remove_if(s.raf_queue.begin(), s.raf_queue.end(),
                               [&](auto& kv) { return kv.first == id; });
      for (auto i = it; i != s.raf_queue.end(); ++i)
        i->second.Reset();
      s.raf_queue.erase(it, s.raf_queue.end());
      // If currently mid-dispatch, also record cancellation.
      s.raf_cancelled.push_back(id);
    }

    struct timers_resetter_register {
      timers_resetter_register() {
        register_template_resetter(&timers_reset_for_isolate);
      }
    };
    static timers_resetter_register s_timers_resetter_register;
  } // namespace

  void install_timers_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    iso->SetMicrotasksPolicy(MicrotasksPolicy::kExplicit);
    global->Set(iso, "setTimeout", FunctionTemplate::New(iso, set_timeout_cb));
    global->Set(iso, "setInterval", FunctionTemplate::New(iso, set_interval_cb));
    global->Set(iso, "setImmediate", FunctionTemplate::New(iso, set_immediate_cb));
    global->Set(iso, "clearTimeout", FunctionTemplate::New(iso, clear_timer_cb));
    global->Set(iso, "clearInterval", FunctionTemplate::New(iso, clear_timer_cb));
    global->Set(iso, "queueMicrotask", FunctionTemplate::New(iso, queue_microtask_cb));
    global->Set(iso, "requestAnimationFrame", FunctionTemplate::New(iso, raf_cb));
    global->Set(iso, "cancelAnimationFrame", FunctionTemplate::New(iso, cancel_raf_cb));
  }

  double next_timer_deadline_seconds(v8::Isolate* iso) {
    auto& s = state_for(iso);
    // Drop heap entries whose timer has been cancelled.
    while (!s.heap.empty() && s.active.find(s.heap.top().id) == s.active.end())
      s.heap.pop();
    if (s.heap.empty())
      return std::numeric_limits<double>::infinity();
    auto now = clock::now();
    auto dl = s.heap.top().deadline;
    if (dl <= now)
      return 0.0;
    return std::chrono::duration<double>(dl - now).count();
  }

  void drain_due_timers(v8::Isolate* iso) {
    HandleScope hs(iso);
    auto ctx = iso->GetCurrentContext();
    auto& s = state_for(iso);
    auto now = clock::now();
    // Collect all due ids first to avoid mutating the heap mid-callback.
    std::vector<u64> due;
    while (!s.heap.empty()) {
      auto top = s.heap.top();
      if (s.active.find(top.id) == s.active.end()) {
        s.heap.pop();
        continue;
      }
      if (top.deadline > now)
        break;
      s.heap.pop();
      due.push_back(top.id);
    }
    for (auto id : due) {
      auto it = s.active.find(id);
      if (it == s.active.end())
        continue;
      auto& e = *it->second;
      auto fn = e.fn.Get(iso);
      std::vector<Local<Value>> argv;
      argv.reserve(e.args.size());
      for (auto& g : e.args)
        argv.push_back(g.Get(iso));
      bool repeat = e.repeat;
      double interval = e.interval_ms;
      // Move the entry out before invoking, so a callback that calls
      // clearInterval(self_id) sees a consistent state.
      std::unique_ptr<timer_entry> entry_owned;
      if (!repeat) {
        entry_owned = std::move(it->second);
        s.active.erase(it);
      }
      bool callback_failed = false;
      {
        TryCatch tc(iso);
        {
          MicrotasksScope microtasks(iso, ctx->GetMicrotaskQueue(),
                                     MicrotasksScope::kRunMicrotasks);
          Local<Value> ignored;
          if (!fn->Call(ctx, ctx->Global(), static_cast<int>(argv.size()),
                        argv.empty() ? nullptr : argv.data())
                   .ToLocal(&ignored)) {
            callback_failed = true;
          }
        }
        if (callback_failed || tc.HasCaught()) {
          if (tc.HasCaught())
            tc.ReThrow();
          else
            (void)throw_error(iso, "timer callback failed");
          return;
        }
      }
      pump_microtasks(iso);
      if (repeat) {
        // Re-find: callback may have cleared it.
        auto it2 = s.active.find(id);
        if (it2 != s.active.end()) {
          it2->second->deadline =
              clock::now() + std::chrono::microseconds(static_cast<i64>(interval * 1000.0));
          s.heap.push({it2->second->deadline, id});
        }
      } else {
        if (entry_owned) {
          entry_owned->fn.Reset();
          for (auto& g : entry_owned->args)
            g.Reset();
        }
      }
    }
  }

  void drain_animation_frames(v8::Isolate* iso) {
    HandleScope hs(iso);
    auto ctx = iso->GetCurrentContext();
    auto& s = state_for(iso);
    if (s.raf_queue.empty())
      return;
    // Snapshot current frame's callbacks; new ones registered during
    // dispatch land in raf_queue (which is now empty after the swap) and
    // fire next frame.
    std::vector<std::pair<u64, Global<Function>>> current;
    current.swap(s.raf_queue);
    auto cancelled = std::move(s.raf_cancelled);
    s.raf_cancelled.clear();
    auto is_cancelled = [&](u64 id) {
      return std::find(cancelled.begin(), cancelled.end(), id) != cancelled.end();
    };
    double now_ms =
        std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
    for (auto& kv : current) {
      if (is_cancelled(kv.first)) {
        kv.second.Reset();
        continue;
      }
      auto fn = kv.second.Get(iso);
      Local<Value> argv[1] = {to_v8(iso, now_ms)};
      bool callback_failed = false;
      {
        TryCatch tc(iso);
        {
          MicrotasksScope microtasks(iso, ctx->GetMicrotaskQueue(),
                                     MicrotasksScope::kRunMicrotasks);
          Local<Value> ignored;
          if (!fn->Call(ctx, ctx->Global(), 1, argv).ToLocal(&ignored))
            callback_failed = true;
        }
        if (callback_failed || tc.HasCaught()) {
          kv.second.Reset();
          if (tc.HasCaught())
            tc.ReThrow();
          else
            (void)throw_error(iso, "requestAnimationFrame callback failed");
          return;
        }
      }
      kv.second.Reset();
    }
  }

  void wake_event_loop() {
    glfwPostEmptyEvent();
    if (glfwGetError(nullptr) == GLFW_NOT_INITIALIZED) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        FXE_WARN("js.timers",
                 "timer fired before the window subsystem was ready; wake_event_loop was a "
                 "no-op");
      }
    }
  }
} // namespace fxe::js
