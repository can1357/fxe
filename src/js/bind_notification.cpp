// JS Notification class. `new Notification({title, body, icon}).show()`
// returns a Promise that resolves when the user clicks. `Notification.permission`
// is always "granted"; requestPermission() resolves to "granted".

#include "bind_notification.hpp"
#include "../os/os.hpp"

#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {
    std::optional<std::string> to_image_src(Isolate* iso, Local<Context> ctx, Local<Value> v) {
      if (v.IsEmpty())
        return std::nullopt;
      if (v->IsString())
        return to_std_string(iso, v);
      if (!v->IsObject())
        return std::nullopt;
      if (auto src = get_prop<Local<Value>>(ctx, v.As<Object>(), "src"_v8(iso)))
        return to_std_string(iso, *src);
      return std::nullopt;
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
      set_prop(cb_ctx, event, "id"_v8, action_id);
      if (input)
        set_prop(cb_ctx, event, "input"_v8, *input);
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
        if (auto value = get_prop<Local<Value>>(ctx, obj, "title"_v8(iso)))
          h->opts.title = to_std_string(iso, *value);
        if (auto value = get_prop<Local<Value>>(ctx, obj, "body"_v8(iso)))
          h->opts.body = to_std_string(iso, *value);
        if (auto value = get_prop<Local<Value>>(ctx, obj, "icon"_v8(iso)))
          h->opts.icon_path = to_std_string(iso, *value);
        if (auto image = get_prop<Local<Value>>(ctx, obj, "image"_v8(iso))) {
          h->opts.image_path = to_image_src(iso, ctx, *image);
        } else if (auto image_path = get_prop<Local<Value>>(ctx, obj, "imagePath"_v8(iso))) {
          h->opts.image_path = to_image_src(iso, ctx, *image_path);
        }
        if (auto value = get_prop<Local<Value>>(ctx, obj, "heroImage"_v8(iso)))
          h->opts.hero_image_path = to_image_src(iso, ctx, *value);
        if (auto value = get_prop<Local<Value>>(ctx, obj, "appLogo"_v8(iso)))
          h->opts.app_logo_image_path = to_image_src(iso, ctx, *value);
        if (auto value = get_prop<Local<Value>>(ctx, obj, "attachmentPath"_v8(iso)))
          h->opts.attachment_path = to_std_string(iso, *value);
        if (auto value = get_prop<Local<Value>>(ctx, obj, "onAction"_v8(iso));
            value && (*value)->IsFunction())
          h->on_action.Reset(iso, (*value).As<Function>());
        if (auto value = get_prop<Local<Value>>(ctx, obj, "actions"_v8(iso));
            value && (*value)->IsArray()) {
          auto arr = (*value).As<Array>();
          u32 len = arr->Length();
          h->opts.actions.reserve(len);
          for (u32 i = 0; i < len; ++i) {
            auto item_value = get_index<Local<Value>>(ctx, arr, i);
            if (!item_value || !(*item_value)->IsObject())
              continue;
            auto action_obj = (*item_value).As<Object>();
            fxe::os::notification_action action;
            if (auto field = get_prop<Local<Value>>(ctx, action_obj, "id"_v8(iso)))
              action.id = to_std_string(iso, *field);
            if (auto field = get_prop<Local<Value>>(ctx, action_obj, "title"_v8(iso)))
              action.title = to_std_string(iso, *field);
            if (auto field = get_prop<Local<Value>>(ctx, action_obj, "kind"_v8(iso));
                field && to_std_string(iso, *field) == "input") {
              action.kind = fxe::os::notification_action_kind::input;
            }
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
      info.GetReturnValue().Set("granted"_v8(info.GetIsolate()));
    }
    void notif_request_permission(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto r = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)r->Resolve(ctx, "granted"_v8(iso));
      info.GetReturnValue().Set(r->GetPromise());
    }
    void notification_getter(Local<Name> /*name*/, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto tpl = FunctionTemplate::New(iso, notif_constructor);
      tpl->SetClassName("Notification"_v8(iso));
      tpl->InstanceTemplate()->SetInternalFieldCount(1);
      auto proto = tpl->PrototypeTemplate();
      proto->Set("show"_v8(iso), FunctionTemplate::New(iso, notif_show));
      tpl->SetNativeDataProperty("permission"_v8(iso), notif_permission_getter);
      tpl->Set("requestPermission"_v8(iso), FunctionTemplate::New(iso, notif_request_permission));
      info.GetReturnValue().Set(tpl->GetFunction(ctx).ToLocalChecked());
    }
  } // namespace

  void install_notification_global(Isolate* iso, Local<ObjectTemplate> global) {
    global->SetLazyDataProperty("Notification"_v8(iso), notification_getter);
  }
} // namespace fxe::js
