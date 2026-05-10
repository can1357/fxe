#include "bind_power.hpp"
#include "../../include/fxe/power.hpp"
#include "../os/os.hpp"

#include <algorithm>
#include <atomic>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <v8.h>
#include <vector>

namespace fxe::js {
  using namespace v8;

  namespace {
    struct listener {
      std::string event;
      Isolate* isolate = nullptr;
      Global<Context> context;
      Global<Function> callback;
    };

    std::mutex g_mu;
    std::unordered_map<u32, listener> g_listeners;
    std::atomic<u32> g_next_id{1};
    std::once_flag g_hooks_once;
    std::mutex g_inhibit_mu;
    std::unordered_map<u32, fxe::os::power_inhibit_handle> g_inhibits;
    std::atomic<u32> g_next_inhibit{1};

    const char* power_event_name(fxe::os::power_event event) {
      switch (event) {
      case fxe::os::power_event::suspend:
        return "suspend";
      case fxe::os::power_event::resume:
        return "resume";
      case fxe::os::power_event::lock_screen:
        return "lock-screen";
      case fxe::os::power_event::unlock_screen:
        return "unlock-screen";
      case fxe::os::power_event::on_battery:
        return "on-battery";
      case fxe::os::power_event::on_ac:
        return "on-ac";
      case fxe::os::power_event::idle:
        return "idle";
      case fxe::os::power_event::active:
        return "active";
      }
      return "";
    }

    const char* network_event_name(fxe::os::network_event event) {
      switch (event) {
      case fxe::os::network_event::online:
        return "online";
      case fxe::os::network_event::offline:
        return "offline";
      }
      return "";
    }

    bool valid_event(const std::string& event) {
      static const char* names[] = {"suspend",    "resume", "lock-screen", "unlock-screen",
                                    "on-battery", "on-ac",  "idle",        "active",
                                    "online",     "offline"};
      return std::any_of(std::begin(names), std::end(names),
                         [&](const char* name) { return event == name; });
    }

    void dispatch_event(const char* event) {
      std::vector<u32> ids;
      {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const auto& [id, entry] : g_listeners) {
          if (entry.event == event)
            ids.push_back(id);
        }
      }

      for (u32 id : ids) {
        Isolate* iso = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_mu);
          auto it = g_listeners.find(id);
          if (it == g_listeners.end() || it->second.callback.IsEmpty() ||
              it->second.context.IsEmpty())
            continue;
          iso = it->second.isolate;
        }

        Isolate::Scope isolate_scope(iso);
        HandleScope handle_scope(iso);
        Local<Context> ctx;
        Local<Function> cb;
        {
          std::lock_guard<std::mutex> lock(g_mu);
          auto it = g_listeners.find(id);
          if (it == g_listeners.end() || it->second.callback.IsEmpty() ||
              it->second.context.IsEmpty())
            continue;
          ctx = it->second.context.Get(iso);
          cb = it->second.callback.Get(iso);
        }
        Context::Scope context_scope(ctx);
        TryCatch try_catch(iso);
        (void)cb->Call(ctx, ctx->Global(), 0, nullptr);
      }
    }

    void ensure_hooks() {
      std::call_once(g_hooks_once, [] {
        fxe::os::power_register([](fxe::os::power_event event) {
          const char* name = power_event_name(event);
          fxe::os::post_main_thread_dispatch(
              [event_name = std::string(name)] { dispatch_event(event_name.c_str()); });
        });
        fxe::os::network_register([](fxe::os::network_event event) {
          const char* name = network_event_name(event);
          fxe::os::post_main_thread_dispatch(
              [event_name = std::string(name)] { dispatch_event(event_name.c_str()); });
        });
      });
    }

    void dispose_listener(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      u32 id = 0;
      if (!info.Data().IsEmpty())
        id = info.Data()->Uint32Value(iso->GetCurrentContext()).FromMaybe(0);
      if (id == 0)
        return;
      std::lock_guard<std::mutex> lock(g_mu);
      auto it = g_listeners.find(id);
      if (it == g_listeners.end())
        return;
      it->second.callback.Reset();
      it->second.context.Reset();
      g_listeners.erase(it);
    }

    void dispose_sleep_inhibit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      u32 id = 0;
      if (!info.Data().IsEmpty())
        id = info.Data()->Uint32Value(iso->GetCurrentContext()).FromMaybe(0);
      if (id == 0)
        return;
      fxe::os::power_inhibit_handle handle;
      {
        std::lock_guard<std::mutex> lock(g_inhibit_mu);
        auto it = g_inhibits.find(id);
        if (it == g_inhibits.end())
          return;
        handle = it->second;
        g_inhibits.erase(it);
      }
      fxe::os::release_sleep_inhibit(handle);
    }

    void power_on(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction()) {
        (void)throw_type_error(iso, "powerMonitor.on requires an event string and callback");
        return;
      }

      std::string event = to_std_string(iso, info[0]);
      if (!valid_event(event)) {
        (void)throw_type_error(iso, "powerMonitor.on received an unknown event");
        return;
      }

      ensure_hooks();
      u32 id = g_next_id.fetch_add(1);
      listener entry;
      entry.event = std::move(event);
      entry.isolate = iso;
      entry.context.Reset(iso, ctx);
      entry.callback.Reset(iso, info[1].As<Function>());
      {
        std::lock_guard<std::mutex> lock(g_mu);
        g_listeners.emplace(id, std::move(entry));
      }

      auto disposer =
          Function::New(ctx, dispose_listener, Integer::NewFromUnsigned(iso, id)).ToLocalChecked();
      info.GetReturnValue().Set(disposer);
    }

    void is_on_battery(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(fxe::os::is_on_battery());
    }

    void is_online(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(fxe::os::is_network_online());
    }

    void system_idle_seconds(const FunctionCallbackInfo<Value>& info) {
      int seconds = fxe::os::system_idle_seconds();
      if (seconds < 0)
        seconds = 0;
      info.GetReturnValue().Set(Integer::New(info.GetIsolate(), seconds));
    }

    void power_inhibit_sleep(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "App.power.inhibitSleep requires an options object");
        return;
      }
      auto options = info[0].As<Object>();
      Local<Value> reason_value;
      Local<Value> what_value;
      if (!options->Get(ctx, "reason"_v8(iso)).ToLocal(&reason_value) ||
          !reason_value->IsString()) {
        (void)throw_type_error(iso, "App.power.inhibitSleep requires options.reason");
        return;
      }
      std::string reason = to_std_string(iso, reason_value);
      if (reason.empty()) {
        (void)throw_type_error(iso, "App.power.inhibitSleep requires a non-empty options.reason");
        return;
      }
      if (!options->Get(ctx, "what"_v8(iso)).ToLocal(&what_value) || !what_value->IsString()) {
        (void)throw_type_error(iso, "App.power.inhibitSleep requires options.what");
        return;
      }
      auto what_str = what_value.As<String>();
      fxe::os::sleep_inhibit_kind kind;
      if (what_str == "idle"_v8) {
        kind = fxe::os::sleep_inhibit_kind::idle;
      } else if (what_str == "sleep"_v8) {
        kind = fxe::os::sleep_inhibit_kind::sleep;
      } else {
        (void)throw_type_error(iso,
                               "App.power.inhibitSleep options.what must be 'idle' or 'sleep'");
        return;
      }

      auto handle = fxe::os::inhibit_sleep(reason, kind);
      if (!handle) {
        (void)throw_error(iso, "App.power.inhibitSleep failed");
        return;
      }
      u32 id = g_next_inhibit.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(g_inhibit_mu);
        g_inhibits.emplace(id, handle);
      }
      auto disposer = Function::New(ctx, dispose_sleep_inhibit, Integer::NewFromUnsigned(iso, id))
                          .ToLocalChecked();
      info.GetReturnValue().Set(disposer);
    }
  } // namespace

  void install_power_monitor_to(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
    HandleScope handle_scope(iso);
    auto power = Object::New(iso);
    auto set_fn = [&](const char* name, FunctionCallback cb) {
      auto fn = Function::New(ctx, cb).ToLocalChecked();
      (void)power->Set(ctx, to_v8(iso, name), fn);
    };
    set_fn("on", power_on);
    set_fn("isOnBattery", is_on_battery);
    set_fn("isOnline", is_online);
    set_fn("systemIdleSeconds", system_idle_seconds);
    (void)appObj->Set(ctx, "powerMonitor"_v8(iso), power);
    (void)ctx->Global()->Set(ctx, "powerMonitor"_v8(iso), power);

    Local<Object> power_api;
    Local<Value> existing_power;
    if (appObj->Get(ctx, "power"_v8(iso)).ToLocal(&existing_power) && existing_power->IsObject()) {
      power_api = existing_power.As<Object>();
    } else {
      power_api = Object::New(iso);
      (void)appObj->Set(ctx, "power"_v8(iso), power_api);
    }
    (void)power_api->Set(ctx, "inhibitSleep"_v8(iso),
                         Function::New(ctx, power_inhibit_sleep).ToLocalChecked());
  }
} // namespace fxe::js
