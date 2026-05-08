// JS Notification class. `new Notification({title, body, icon}).show()`
// returns a Promise that resolves when the user clicks. `Notification.permission`
// is always "granted"; requestPermission() resolves to "granted".

#include "bind_notification.hpp"
#include "../os/os.hpp"

#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_strings.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {
    Local<String> s(Isolate* iso, const char* str) {
      return String::NewFromUtf8(iso, str, NewStringType::kNormal).ToLocalChecked();
    }
    bool get_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                  Local<Value>* out) {
      return obj->Get(ctx, s(iso, name)).ToLocal(out);
    }
    std::string to_str(Isolate* iso, Local<Value> v) {
      if (v.IsEmpty() || !v->IsString())
        return {};
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    struct action_callback_state {
      Isolate* isolate = nullptr;
      Global<Context> context;
      Global<Function> callback;
    };

    void invoke_action_callback(const std::shared_ptr<action_callback_state>& state,
                                const std::string& action_id, std::optional<std::string> input) {
      if (!state || state->callback.IsEmpty() || state->context.IsEmpty())
        return;
      Isolate* iso = state->isolate;
      Isolate::Scope is(iso);
      HandleScope hs(iso);
      Local<Context> cb_ctx = state->context.Get(iso);
      Context::Scope cs(cb_ctx);
      Local<Object> event = Object::New(iso);
      (void)event->Set(
          cb_ctx, "id"_v8(iso),
          String::NewFromUtf8(iso, action_id.c_str(), NewStringType::kNormal).ToLocalChecked());
      if (input) {
        (void)event->Set(
            cb_ctx, "input"_v8(iso),
            String::NewFromUtf8(iso, input->c_str(), NewStringType::kNormal).ToLocalChecked());
      }
      Local<Value> argv[] = {event};
      TryCatch try_catch(iso);
      (void)state->callback.Get(iso)->Call(cb_ctx, cb_ctx->Global(), 1, argv);
    }

    constexpr int kSlotOpts = 0;
    struct opts_holder {
      Isolate* isolate = nullptr;
      Global<Context> context;
      Global<Function> on_action;
      fxe::os::notification_options opts;
      v8::Global<v8::Object>* persistent = nullptr;
    };

    void finalizer_cb(const WeakCallbackInfo<opts_holder>& data) {
      auto* h = data.GetParameter();
      if (h && h->isolate) {
        h->context.Reset();
        h->on_action.Reset();
      }
      if (h && h->persistent) {
        h->persistent->Reset();
        delete h->persistent;
      }
      delete h;
    }

    void notif_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Notification must be called with new");
        return;
      }
      auto* h = new opts_holder();
      h->isolate = iso;
      h->context.Reset(iso, ctx);
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        Local<Value> v;
        if (get_prop(iso, ctx, obj, "title", &v))
          h->opts.title = to_str(iso, v);
        if (get_prop(iso, ctx, obj, "body", &v))
          h->opts.body = to_str(iso, v);
        if (get_prop(iso, ctx, obj, "icon", &v))
          h->opts.icon_path = to_str(iso, v);
        if (get_prop(iso, ctx, obj, "image", &v) || get_prop(iso, ctx, obj, "imagePath", &v))
          h->opts.image_path = to_str(iso, v);
        if (get_prop(iso, ctx, obj, "attachmentPath", &v))
          h->opts.attachment_path = to_str(iso, v);
        if (get_prop(iso, ctx, obj, "onAction", &v) && v->IsFunction())
          h->on_action.Reset(iso, v.As<Function>());
        if (get_prop(iso, ctx, obj, "actions", &v) && v->IsArray()) {
          auto arr = v.As<Array>();
          u32 len = arr->Length();
          h->opts.actions.reserve(len);
          for (u32 i = 0; i < len; ++i) {
            Local<Value> item_value;
            if (!arr->Get(ctx, i).ToLocal(&item_value) || !item_value->IsObject())
              continue;
            auto action_obj = item_value.As<Object>();
            Local<Value> field;
            fxe::os::notification_action action;
            if (get_prop(iso, ctx, action_obj, "id", &field))
              action.id = to_str(iso, field);
            if (get_prop(iso, ctx, action_obj, "title", &field))
              action.title = to_str(iso, field);
            if (get_prop(iso, ctx, action_obj, "kind", &field) && to_str(iso, field) == "input")
              action.kind = fxe::os::notification_action_kind::input;
            if (!action.id.empty() && !action.title.empty())
              h->opts.actions.push_back(std::move(action));
          }
        }
      }
      auto self = info.This();
      self->SetInternalField(kSlotOpts, make_external(iso, h));
      auto* gp = new Global<Object>(iso, self);
      h->persistent = gp;
      gp->SetWeak(h, finalizer_cb, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(self);
    }

    opts_holder* unwrap_self(Local<Object> self) {
      auto v = self->GetInternalField(kSlotOpts);
      return external_ptr<opts_holder>(v);
    }

    void notif_show(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      auto* h = unwrap_self(info.This());
      if (!h) {
        (void)resolver->Reject(ctx, Exception::Error("invalid Notification this"_v8(iso)));
        return;
      }
      std::shared_ptr<action_callback_state> action_state;
      if (!h->on_action.IsEmpty()) {
        action_state = std::make_shared<action_callback_state>();
        action_state->isolate = iso;
        action_state->context.Reset(iso, h->context.Get(iso));
        action_state->callback.Reset(iso, h->on_action.Get(iso));
      }
      int id =
          action_state
              ? fxe::os::show_notification(
                    h->opts,
                    [action_state](const std::string& action_id, std::optional<std::string> input) {
                      invoke_action_callback(action_state, action_id, std::move(input));
                    })
              : fxe::os::show_notification(h->opts);
      if (id <= 0) {
        (void)resolver->Resolve(ctx, Undefined(iso));
        return;
      }
      auto persistent = std::make_shared<Global<Promise::Resolver>>(iso, resolver);
      Isolate* captured_iso = iso;
      fxe::os::on_notification_click(id, [captured_iso, persistent]() {
        Isolate::Scope is(captured_iso);
        HandleScope hs(captured_iso);
        auto ctx2 = captured_iso->GetCurrentContext();
        auto r = persistent->Get(captured_iso);
        (void)r->Resolve(ctx2, Undefined(captured_iso));
      });
    }

    void notif_permission_getter(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(s(info.GetIsolate(), "granted"));
    }
    void notif_request_permission(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto r = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)r->Resolve(ctx, "granted"_v8(iso));
      info.GetReturnValue().Set(r->GetPromise());
    }
  } // namespace

  void install_notification_global(Isolate* iso, Local<ObjectTemplate> global) {
    auto tpl = FunctionTemplate::New(iso, notif_constructor);
    tpl->SetClassName("Notification"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "show", FunctionTemplate::New(iso, notif_show));
    tpl->SetNativeDataProperty("permission"_v8(iso), notif_permission_getter);
    tpl->Set(iso, "requestPermission", FunctionTemplate::New(iso, notif_request_permission));
    global->Set(iso, "Notification", tpl);
  }
} // namespace fxe::js
