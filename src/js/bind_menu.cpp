#include "bind_menu.hpp"
#include "../os/os.hpp"

#include <fxe/types.hpp>
#include <fxe/v8_strings.hpp>
#include <memory>
#include <string>
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
  } // namespace

  void parse_menu_items(Isolate* iso, Local<Context> ctx, Local<Value> arr_v,
                        std::vector<fxe::os::menu_item>& out) {
    if (arr_v.IsEmpty() || !arr_v->IsArray())
      return;
    auto arr = arr_v.As<Array>();
    for (u32 i = 0; i < arr->Length(); ++i) {
      Local<Value> el;
      if (!arr->Get(ctx, i).ToLocal(&el) || !el->IsObject())
        continue;
      auto obj = el.As<Object>();
      fxe::os::menu_item it;
      Local<Value> v;
      if (obj->Get(ctx, "id"_v8(iso)).ToLocal(&v))
        it.id = to_str(iso, v);
      if (obj->Get(ctx, "label"_v8(iso)).ToLocal(&v))
        it.label = to_str(iso, v);
      if (obj->Get(ctx, "accelerator"_v8(iso)).ToLocal(&v))
        it.accelerator = to_str(iso, v);
      if (obj->Get(ctx, "type"_v8(iso)).ToLocal(&v) && v->IsString())
        it.type = to_str(iso, v);
      if (obj->Get(ctx, "enabled"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
        it.enabled = v->BooleanValue(iso);
      if (obj->Get(ctx, "checked"_v8(iso)).ToLocal(&v))
        it.checked = v->BooleanValue(iso);
      if (obj->Get(ctx, "submenu"_v8(iso)).ToLocal(&v))
        parse_menu_items(iso, ctx, v, it.submenu);
      out.push_back(std::move(it));
    }
  }

  bool parse_menu_item_patch(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                             fxe::os::menu_item_patch& out) {
    Local<Value> v;
    if (obj->Get(ctx, "label"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
      out.label = to_str(iso, v);
    if (obj->Get(ctx, "enabled"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
      out.enabled = v->BooleanValue(iso);
    if (obj->Get(ctx, "checked"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
      out.checked = v->BooleanValue(iso);
    if (obj->Get(ctx, "visible"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
      out.visible = v->BooleanValue(iso);
    if (obj->Get(ctx, "accelerator"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
      out.accelerator = to_str(iso, v);
    return true;
  }

  namespace {
    void menu_set_application_menu(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::vector<fxe::os::menu_item> items;
      if (info.Length() > 0)
        parse_menu_items(iso, ctx, info[0], items);
      fxe::os::set_application_menu(items);
    }

    void menu_popup(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      std::vector<fxe::os::menu_item> items;
      if (info.Length() > 0)
        parse_menu_items(iso, ctx, info[0], items);
      int x = 0, y = 0;
      if (info.Length() > 1 && info[1]->IsNumber())
        x = info[1]->Int32Value(ctx).FromMaybe(0);
      if (info.Length() > 2 && info[2]->IsNumber())
        y = info[2]->Int32Value(ctx).FromMaybe(0);

      auto persistent = std::make_shared<Global<Promise::Resolver>>(iso, resolver);
      Isolate* captured = iso;
      fxe::os::show_context_menu(items, x, y, [captured, persistent](const std::string& id) {
        Isolate::Scope is(captured);
        HandleScope hs(captured);
        auto ctx2 = captured->GetCurrentContext();
        auto r = persistent->Get(captured);
        if (id.empty()) {
          (void)r->Resolve(ctx2, Null(captured));
        } else {
          (void)r->Resolve(ctx2, String::NewFromUtf8(captured, id.data(), NewStringType::kNormal,
                                                     static_cast<int>(id.size()))
                                     .ToLocalChecked());
        }
      });
    }

    void menu_update_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[1]->IsObject()) {
        info.GetReturnValue().Set(false);
        return;
      }
      std::string id = to_str(iso, info[0]);
      fxe::os::menu_item_patch patch;
      parse_menu_item_patch(iso, ctx, info[1].As<Object>(), patch);
      info.GetReturnValue().Set(fxe::os::update_menu_item(id, patch));
    }

    std::string menu_handle_id(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      Local<Value> id_v;
      if (!info.This()->Get(ctx, "id"_v8(iso)).ToLocal(&id_v))
        return {};
      return to_str(iso, id_v);
    }

    void menu_item_set_label(const FunctionCallbackInfo<Value>& info) {
      fxe::os::menu_item_patch patch;
      patch.label = info.Length() > 0 ? to_str(info.GetIsolate(), info[0]) : std::string{};
      (void)fxe::os::update_menu_item(menu_handle_id(info), patch);
    }

    void menu_item_set_enabled(const FunctionCallbackInfo<Value>& info) {
      fxe::os::menu_item_patch patch;
      patch.enabled = info.Length() > 0 && info[0]->BooleanValue(info.GetIsolate());
      (void)fxe::os::update_menu_item(menu_handle_id(info), patch);
    }

    void menu_item_set_checked(const FunctionCallbackInfo<Value>& info) {
      fxe::os::menu_item_patch patch;
      patch.checked = info.Length() > 0 && info[0]->BooleanValue(info.GetIsolate());
      (void)fxe::os::update_menu_item(menu_handle_id(info), patch);
    }

    void menu_item_set_visible(const FunctionCallbackInfo<Value>& info) {
      fxe::os::menu_item_patch patch;
      patch.visible = info.Length() > 0 && info[0]->BooleanValue(info.GetIsolate());
      (void)fxe::os::update_menu_item(menu_handle_id(info), patch);
    }

    void menu_item_set_accelerator(const FunctionCallbackInfo<Value>& info) {
      fxe::os::menu_item_patch patch;
      patch.accelerator = info.Length() > 0 ? to_str(info.GetIsolate(), info[0]) : std::string{};
      (void)fxe::os::update_menu_item(menu_handle_id(info), patch);
    }

    void menu_find_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::string id = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      if (!fxe::os::menu_item_exists(id)) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto obj = Object::New(iso);
      (void)obj->Set(
          ctx, "id"_v8(iso),
          String::NewFromUtf8(iso, id.data(), NewStringType::kNormal, static_cast<int>(id.size()))
              .ToLocalChecked());
      (void)obj->Set(ctx, "setLabel"_v8(iso),
                     Function::New(ctx, menu_item_set_label).ToLocalChecked());
      (void)obj->Set(ctx, "setEnabled"_v8(iso),
                     Function::New(ctx, menu_item_set_enabled).ToLocalChecked());
      (void)obj->Set(ctx, "setChecked"_v8(iso),
                     Function::New(ctx, menu_item_set_checked).ToLocalChecked());
      (void)obj->Set(ctx, "setVisible"_v8(iso),
                     Function::New(ctx, menu_item_set_visible).ToLocalChecked());
      (void)obj->Set(ctx, "setAccelerator"_v8(iso),
                     Function::New(ctx, menu_item_set_accelerator).ToLocalChecked());
      info.GetReturnValue().Set(obj);
    }

    // Persistent JS callback for application-menu activations. Single-slot:
    // re-registering replaces the previous handler. The OS handler bridges
    // to V8 inside an Isolate/HandleScope.
    Global<Function>* g_menu_command_callback = nullptr;
    Isolate* g_menu_command_isolate = nullptr;

    void invoke_menu_command_callback(const std::string& id) {
      if (!g_menu_command_callback || !g_menu_command_isolate)
        return;
      Isolate* iso = g_menu_command_isolate;
      Isolate::Scope is(iso);
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (ctx.IsEmpty())
        return;
      Context::Scope cs(ctx);
      auto fn = g_menu_command_callback->Get(iso);
      if (fn.IsEmpty())
        return;
      Local<String> arg_str;
      if (!String::NewFromUtf8(iso, id.data(), NewStringType::kNormal,
                               static_cast<int>(id.size()))
               .ToLocal(&arg_str)) {
        return;
      }
      Local<Value> argv[1] = {arg_str};
      TryCatch tc(iso);
      (void)fn->Call(ctx, ctx->Global(), 1, argv);
      if (tc.HasCaught())
        tc.ReThrow();
    }

    void menu_on_command(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      // Clear: Menu.onCommand(null) or Menu.onCommand() removes the handler.
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        if (g_menu_command_callback) {
          g_menu_command_callback->Reset();
          delete g_menu_command_callback;
          g_menu_command_callback = nullptr;
        }
        g_menu_command_isolate = nullptr;
        fxe::os::set_application_menu_handler({});
        return;
      }
      if (g_menu_command_callback) {
        g_menu_command_callback->Reset();
        delete g_menu_command_callback;
      }
      g_menu_command_callback = new Global<Function>(iso, info[0].As<Function>());
      g_menu_command_isolate = iso;
      fxe::os::set_application_menu_handler(
          [](const std::string& id) { invoke_menu_command_callback(id); });
    }
  } // namespace

  void install_menu_global(Isolate* iso, Local<ObjectTemplate> global) {
    auto t = ObjectTemplate::New(iso);
    t->Set(iso, "setApplicationMenu", FunctionTemplate::New(iso, menu_set_application_menu));
    t->Set(iso, "popup", FunctionTemplate::New(iso, menu_popup));
    t->Set(iso, "updateItem", FunctionTemplate::New(iso, menu_update_item));
    t->Set(iso, "findItem", FunctionTemplate::New(iso, menu_find_item));
    t->Set(iso, "onCommand", FunctionTemplate::New(iso, menu_on_command));
    global->Set(iso, "Menu", t);
  }
} // namespace fxe::js
