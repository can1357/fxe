#include "bind_tray.hpp"
#include "../os/os.hpp"
#include "bind_menu.hpp"

#include <fxe/v8_strings.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <v8.h>
#include <vector>

namespace fxe::js {
  using namespace v8;
  namespace {
    Local<String> s(Isolate* iso, const char* str) {
      return String::NewFromUtf8(iso, str, NewStringType::kNormal).ToLocalChecked();
    }
    std::string to_str(Isolate* iso, Local<Value> v) {
      if (v.IsEmpty() || !v->IsString())
        return {};
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    constexpr int kSlotHandle = 0;
    struct holder {
      fxe::os::tray_handle h;
      Global<Object>* persistent = nullptr;
    };

    struct listener_binding {
      fxe::os::tray_handle h;
      Global<Function> fn;
    };

    struct registry {
      std::unordered_map<int, listener_binding> by_token;
    };

    registry& reg(Isolate* iso) {
      static thread_local registry r;
      (void)iso;
      return r;
    }

    void cleanup_tray_listeners(Isolate* iso, fxe::os::tray_handle h) {
      auto& r = reg(iso);
      for (auto it = r.by_token.begin(); it != r.by_token.end();) {
        if (it->second.h.id == h.id) {
          fxe::os::tray_off(it->second.h, it->first);
          it->second.fn.Reset();
          it = r.by_token.erase(it);
        } else {
          ++it;
        }
      }
    }

    void finalizer_cb(const WeakCallbackInfo<holder>& d) {
      auto* hp = d.GetParameter();
      cleanup_tray_listeners(d.GetIsolate(), hp->h);
      fxe::os::tray_destroy(hp->h);
      if (hp->persistent) {
        hp->persistent->Reset();
        delete hp->persistent;
      }
      delete hp;
    }

    holder* unwrap(Local<Object> self) {
      auto v = self->GetInternalField(kSlotHandle);
      return static_cast<holder*>(v.As<External>()->Value(v8::kExternalPointerTypeTagDefault));
    }

    void tray_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        iso->ThrowException(Exception::TypeError("Tray must be called with new"_v8(iso)));
        return;
      }
      std::string icon = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      std::string tip = info.Length() > 1 ? to_str(iso, info[1]) : std::string{};
      auto* h = new holder();
      h->h = fxe::os::tray_create(icon, tip);
      auto self = info.This();
      self->SetInternalField(kSlotHandle,
                             External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      auto* gp = new Global<Object>(iso, self);
      h->persistent = gp;
      gp->SetWeak(h, finalizer_cb, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(self);
    }

    void tray_set_menu_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap(info.This());
      if (!h)
        return;
      std::vector<fxe::os::menu_item> items;
      if (info.Length() > 0)
        parse_menu_items(iso, ctx, info[0], items);
      fxe::os::tray_set_menu(h->h, items);
    }

    void tray_destroy_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap(info.This());
      if (!h)
        return;
      cleanup_tray_listeners(iso, h->h);
      fxe::os::tray_destroy(h->h);
      h->h = fxe::os::tray_handle{};
    }

    void tray_set_image_cb(const FunctionCallbackInfo<Value>& info) {
      auto* h = unwrap(info.This());
      if (!h) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(fxe::os::tray_set_image(
          h->h, info.Length() > 0 ? to_str(info.GetIsolate(), info[0]) : std::string{}));
    }

    void tray_set_title_cb(const FunctionCallbackInfo<Value>& info) {
      auto* h = unwrap(info.This());
      if (!h) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(fxe::os::tray_set_title(
          h->h, info.Length() > 0 ? to_str(info.GetIsolate(), info[0]) : std::string{}));
    }

    void tray_set_tooltip_cb(const FunctionCallbackInfo<Value>& info) {
      auto* h = unwrap(info.This());
      if (!h) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(fxe::os::tray_set_tooltip(
          h->h, info.Length() > 0 ? to_str(info.GetIsolate(), info[0]) : std::string{}));
    }

    bool parse_tray_event(std::string_view name, fxe::os::tray_event_kind& out) {
      if (name == "click") {
        out = fxe::os::tray_event_kind::click;
        return true;
      }
      if (name == "right-click") {
        out = fxe::os::tray_event_kind::right_click;
        return true;
      }
      if (name == "double-click") {
        out = fxe::os::tray_event_kind::double_click;
        return true;
      }
      return false;
    }

    void tray_disposer_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      int token = info.Data()->Int32Value(ctx).FromMaybe(-1);
      auto& r = reg(iso);
      auto it = r.by_token.find(token);
      if (it == r.by_token.end())
        return;
      fxe::os::tray_off(it->second.h, token);
      it->second.fn.Reset();
      r.by_token.erase(it);
    }

    void tray_on_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap(info.This());
      if (!h || info.Length() < 2 || !info[1]->IsFunction()) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      fxe::os::tray_event_kind kind;
      if (!parse_tray_event(to_str(iso, info[0]), kind)) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }

      auto token_ref = std::make_shared<int>(-1);
      Isolate* captured = iso;
      int token = fxe::os::tray_on(h->h, kind, [captured, token_ref]() {
        Isolate::Scope is(captured);
        HandleScope hs(captured);
        auto ctx2 = captured->GetCurrentContext();
        auto& r = reg(captured);
        auto it = r.by_token.find(*token_ref);
        if (it == r.by_token.end() || it->second.fn.IsEmpty())
          return;
        auto cb = it->second.fn.Get(captured);
        TryCatch tc(captured);
        (void)cb->Call(ctx2, ctx2->Global(), 0, nullptr);
      });
      if (token < 0) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      *token_ref = token;
      auto& binding = reg(iso).by_token[token];
      binding.h = h->h;
      binding.fn.Reset(iso, info[1].As<Function>());
      info.GetReturnValue().Set(
          Function::New(ctx, tray_disposer_cb, Integer::New(iso, token)).ToLocalChecked());
    }
  } // namespace

  void install_tray_global(Isolate* iso, Local<ObjectTemplate> global) {
    auto tpl = FunctionTemplate::New(iso, tray_constructor);
    tpl->SetClassName("Tray"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "setMenu", FunctionTemplate::New(iso, tray_set_menu_cb));
    proto->Set(iso, "setImage", FunctionTemplate::New(iso, tray_set_image_cb));
    proto->Set(iso, "setTitle", FunctionTemplate::New(iso, tray_set_title_cb));
    proto->Set(iso, "setToolTip", FunctionTemplate::New(iso, tray_set_tooltip_cb));
    proto->Set(iso, "on", FunctionTemplate::New(iso, tray_on_cb));
    proto->Set(iso, "destroy", FunctionTemplate::New(iso, tray_destroy_cb));
    global->Set(iso, "Tray", tpl);
  }
} // namespace fxe::js
