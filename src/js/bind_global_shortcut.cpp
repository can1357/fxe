#include "bind_global_shortcut.hpp"
#include "../os/os.hpp"

#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {
    // Per-isolate registry of accelerator -> persistent JS callback. The
    // callback is invoked on the main thread via the os-shim dispatch pump.
    struct registry {
      std::unordered_map<std::string, Global<Function>> by_acc;
    };
    registry& reg(Isolate* iso) {
      static thread_local registry r;
      (void)iso;
      return r;
    }

    void gs_register(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 2 || !info[1]->IsFunction()) {
        info.GetReturnValue().Set(false);
        return;
      }
      std::string acc = to_std_string_strict(iso, info[0]);
      if (acc.empty()) {
        info.GetReturnValue().Set(false);
        return;
      }
      auto fn = info[1].As<Function>();
      auto& r = reg(iso);
      r.by_acc[acc].Reset(iso, fn);
      Isolate* captured = iso;
      bool ok = fxe::os::global_shortcut_register(acc, [captured, acc]() {
        Isolate::Scope is(captured);
        HandleScope hs(captured);
        auto ctx2 = captured->GetCurrentContext();
        auto& rr = reg(captured);
        auto it = rr.by_acc.find(acc);
        if (it == rr.by_acc.end() || it->second.IsEmpty())
          return;
        auto cb = it->second.Get(captured);
        TryCatch tc(captured);
        (void)cb->Call(ctx2, ctx2->Global(), 0, nullptr);
      });
      if (!ok)
        r.by_acc.erase(acc);
      info.GetReturnValue().Set(ok);
    }

    void gs_unregister(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string acc = info.Length() > 0 ? to_std_string_strict(iso, info[0]) : std::string{};
      if (acc.empty())
        return;
      fxe::os::global_shortcut_unregister(acc);
      auto& r = reg(iso);
      auto it = r.by_acc.find(acc);
      if (it != r.by_acc.end()) {
        it->second.Reset();
        r.by_acc.erase(it);
      }
    }

    void gs_unregister_all(const FunctionCallbackInfo<Value>& info) {
      fxe::os::global_shortcut_unregister_all();
      auto& r = reg(info.GetIsolate());
      for (auto& [_, fn] : r.by_acc)
        fn.Reset();
      r.by_acc.clear();
    }
    void global_shortcut_namespace_getter(Local<Name> /*name*/,
                                          const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto ns = Object::New(iso);
      (void)ns->Set(ctx, "register"_v8(iso), Function::New(ctx, gs_register).ToLocalChecked());
      (void)ns->Set(ctx, "unregister"_v8(iso), Function::New(ctx, gs_unregister).ToLocalChecked());
      (void)ns->Set(ctx, "unregisterAll"_v8(iso),
                    Function::New(ctx, gs_unregister_all).ToLocalChecked());
      info.GetReturnValue().Set(ns);
    }
  } // namespace

  void install_global_shortcut_global(Isolate* iso, Local<ObjectTemplate> global) {
    global->SetLazyDataProperty("globalShortcut"_v8(iso), global_shortcut_namespace_getter);
  }
} // namespace fxe::js
