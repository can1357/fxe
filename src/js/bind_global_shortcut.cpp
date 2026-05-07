#include "bind_global_shortcut.hpp"
#include "../os/os.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {
    std::string to_str(Isolate* iso, Local<Value> v) {
      if (v.IsEmpty() || !v->IsString())
        return {};
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

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
      std::string acc = to_str(iso, info[0]);
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
      std::string acc = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
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
  } // namespace

  void install_global_shortcut_global(Isolate* iso, Local<ObjectTemplate> global) {
    auto t = ObjectTemplate::New(iso);
    t->Set(iso, "register", FunctionTemplate::New(iso, gs_register));
    t->Set(iso, "unregister", FunctionTemplate::New(iso, gs_unregister));
    t->Set(iso, "unregisterAll", FunctionTemplate::New(iso, gs_unregister_all));
    global->Set(iso, "globalShortcut", t);
  }
} // namespace fxe::js
