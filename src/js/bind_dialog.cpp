// JS bindings for the OS dialog shims. Every entry returns a Promise so JS
// callers can await results uniformly even though the macOS implementation is
// synchronous-modal under the hood.

#include "bind_dialog.hpp"
#include "../os/os.hpp"

#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <string>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {
    void parse_filters([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> v,
                       std::vector<fxe::os::dialog_filter>& out) {
      if (v.IsEmpty() || !v->IsArray())
        return;
      auto arr = v.As<Array>();
      for (u32 i = 0; i < arr->Length(); ++i) {
        auto el = get_index<Local<Value>>(ctx, arr, i);
        if (!el.has_value() || !(*el)->IsObject())
          continue;
        auto obj = (*el).As<Object>();
        fxe::os::dialog_filter f;
        f.name = get_prop<std::string>(ctx, obj, "name").value_or("");
        if (auto exts = get_prop<Local<Array>>(ctx, obj, "extensions")) {
          auto ea = *exts;
          for (u32 j = 0; j < ea->Length(); ++j) {
            if (auto ext = get_index<std::string>(ctx, ea, j))
              f.extensions.push_back(std::move(*ext));
          }
        }
        out.push_back(std::move(f));
      }
    }

    Local<Promise> resolved(Local<Context> ctx, Local<Value> v) {
      auto r = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)r->Resolve(ctx, v);
      return r->GetPromise();
    }

    void show_open(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      fxe::os::open_dialog_options o;
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        o.title = get_prop<std::string>(ctx, obj, "title").value_or("");
        o.default_path = get_prop<std::string>(ctx, obj, "defaultPath").value_or("");
        o.multiple = get_prop_or<bool>(ctx, obj, "multiple", false);
        o.directories = get_prop_or<bool>(ctx, obj, "directories", false);
        if (auto filters = get_prop<Local<Value>>(ctx, obj, "filters"))
          parse_filters(iso, ctx, *filters, o.filters);
      }
      auto paths = fxe::os::show_open_dialog(o);
      auto arr = Array::New(iso, static_cast<int>(paths.size()));
      for (usize i = 0; i < paths.size(); ++i)
        set_index(ctx, arr, static_cast<u32>(i), paths[i]);
      auto result = Object::New(iso);
      set_prop(ctx, result, "canceled", paths.empty());
      set_prop(ctx, result, "filePaths", arr);
      info.GetReturnValue().Set(resolved(ctx, result));
    }

    void show_save(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      fxe::os::save_dialog_options o;
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        o.title = get_prop<std::string>(ctx, obj, "title").value_or("");
        o.default_path = get_prop<std::string>(ctx, obj, "defaultPath").value_or("");
        if (auto filters = get_prop<Local<Value>>(ctx, obj, "filters"))
          parse_filters(iso, ctx, *filters, o.filters);
      }
      auto p = fxe::os::show_save_dialog(o);
      auto result = Object::New(iso);
      set_prop(ctx, result, "canceled", !p.has_value());
      set_prop(ctx, result, "filePath", p ? to_v8(iso, *p) : to_v8_undefined(iso));
      info.GetReturnValue().Set(resolved(ctx, result));
    }

    void show_message(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      fxe::os::message_box_options o;
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        o.title = get_prop<std::string>(ctx, obj, "title").value_or("");
        o.message = get_prop<std::string>(ctx, obj, "message").value_or("");
        o.detail = get_prop<std::string>(ctx, obj, "detail").value_or("");
        o.type = get_prop<std::string>(ctx, obj, "type").value_or("");
        if (auto buttons = get_prop<Local<Array>>(ctx, obj, "buttons")) {
          auto a = *buttons;
          for (u32 i = 0; i < a->Length(); ++i) {
            if (auto button = get_index<std::string>(ctx, a, i))
              o.buttons.push_back(std::move(*button));
          }
        }
      }
      int idx = fxe::os::show_message_box(o);
      auto result = Object::New(iso);
      set_prop(ctx, result, "response", idx);
      info.GetReturnValue().Set(resolved(ctx, result));
    }
    void dialog_namespace_getter(Local<Name> /*name*/, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto ns = Object::New(iso);
      (void)ns->Set(ctx, "showOpenDialog"_v8(iso), Function::New(ctx, show_open).ToLocalChecked());
      (void)ns->Set(ctx, "showSaveDialog"_v8(iso), Function::New(ctx, show_save).ToLocalChecked());
      (void)ns->Set(ctx, "showMessageBox"_v8(iso),
                    Function::New(ctx, show_message).ToLocalChecked());
      info.GetReturnValue().Set(ns);
    }
  } // namespace

  void install_dialog_global(Isolate* iso, Local<ObjectTemplate> global) {
    global->SetLazyDataProperty("dialog"_v8(iso), dialog_namespace_getter);
  }
} // namespace fxe::js
