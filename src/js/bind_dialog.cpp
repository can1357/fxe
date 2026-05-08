// JS bindings for the OS dialog shims. Every entry returns a Promise so JS
// callers can await results uniformly even though the macOS implementation is
// synchronous-modal under the hood.

#include "bind_dialog.hpp"
#include "../os/os.hpp"

#include <fxe/v8_strings.hpp>
#include <string>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {
    Local<String> s(Isolate* iso, const char* str) {
      return String::NewFromUtf8(iso, str, NewStringType::kNormal).ToLocalChecked();
    }
    Local<String> s(Isolate* iso, const std::string& str) {
      return String::NewFromUtf8(iso, str.data(), NewStringType::kNormal,
                                 static_cast<int>(str.size()))
          .ToLocalChecked();
    }
    std::string to_str(Isolate* iso, Local<Value> v) {
      if (v.IsEmpty() || !v->IsString())
        return {};
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }
    bool to_bool([[maybe_unused]] Local<Context> ctx, Local<Value> v) {
      return !v.IsEmpty() && v->BooleanValue(v8::Isolate::GetCurrent());
    }

    void parse_filters(Isolate* iso, Local<Context> ctx, Local<Value> v,
                       std::vector<fxe::os::dialog_filter>& out) {
      if (v.IsEmpty() || !v->IsArray())
        return;
      auto arr = v.As<Array>();
      for (uint32_t i = 0; i < arr->Length(); ++i) {
        Local<Value> el;
        if (!arr->Get(ctx, i).ToLocal(&el) || !el->IsObject())
          continue;
        auto obj = el.As<Object>();
        fxe::os::dialog_filter f;
        Local<Value> name, exts;
        if (obj->Get(ctx, "name"_v8(iso)).ToLocal(&name))
          f.name = to_str(iso, name);
        if (obj->Get(ctx, "extensions"_v8(iso)).ToLocal(&exts) && exts->IsArray()) {
          auto ea = exts.As<Array>();
          for (uint32_t j = 0; j < ea->Length(); ++j) {
            Local<Value> e;
            if (ea->Get(ctx, j).ToLocal(&e))
              f.extensions.push_back(to_str(iso, e));
          }
        }
        out.push_back(std::move(f));
      }
    }

    Local<Promise> resolved([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> v) {
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
        Local<Value> v;
        if (obj->Get(ctx, "title"_v8(iso)).ToLocal(&v))
          o.title = to_str(iso, v);
        if (obj->Get(ctx, "defaultPath"_v8(iso)).ToLocal(&v))
          o.default_path = to_str(iso, v);
        if (obj->Get(ctx, "multiple"_v8(iso)).ToLocal(&v))
          o.multiple = to_bool(ctx, v);
        if (obj->Get(ctx, "directories"_v8(iso)).ToLocal(&v))
          o.directories = to_bool(ctx, v);
        if (obj->Get(ctx, "filters"_v8(iso)).ToLocal(&v))
          parse_filters(iso, ctx, v, o.filters);
      }
      auto paths = fxe::os::show_open_dialog(o);
      auto arr = Array::New(iso, static_cast<int>(paths.size()));
      for (size_t i = 0; i < paths.size(); ++i)
        (void)arr->Set(ctx, static_cast<uint32_t>(i), s(iso, paths[i]));
      auto result = Object::New(iso);
      (void)result->Set(ctx, "canceled"_v8(iso), Boolean::New(iso, paths.empty()));
      (void)result->Set(ctx, "filePaths"_v8(iso), arr);
      info.GetReturnValue().Set(resolved(iso, ctx, result));
    }

    void show_save(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      fxe::os::save_dialog_options o;
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        Local<Value> v;
        if (obj->Get(ctx, "title"_v8(iso)).ToLocal(&v))
          o.title = to_str(iso, v);
        if (obj->Get(ctx, "defaultPath"_v8(iso)).ToLocal(&v))
          o.default_path = to_str(iso, v);
        if (obj->Get(ctx, "filters"_v8(iso)).ToLocal(&v))
          parse_filters(iso, ctx, v, o.filters);
      }
      auto p = fxe::os::show_save_dialog(o);
      auto result = Object::New(iso);
      (void)result->Set(ctx, "canceled"_v8(iso), Boolean::New(iso, !p.has_value()));
      (void)result->Set(ctx, "filePath"_v8(iso),
                        p ? static_cast<Local<Value>>(s(iso, *p))
                          : static_cast<Local<Value>>(Undefined(iso)));
      info.GetReturnValue().Set(resolved(iso, ctx, result));
    }

    void show_message(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      fxe::os::message_box_options o;
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        Local<Value> v;
        if (obj->Get(ctx, "title"_v8(iso)).ToLocal(&v))
          o.title = to_str(iso, v);
        if (obj->Get(ctx, "message"_v8(iso)).ToLocal(&v))
          o.message = to_str(iso, v);
        if (obj->Get(ctx, "detail"_v8(iso)).ToLocal(&v))
          o.detail = to_str(iso, v);
        if (obj->Get(ctx, "type"_v8(iso)).ToLocal(&v))
          o.type = to_str(iso, v);
        if (obj->Get(ctx, "buttons"_v8(iso)).ToLocal(&v) && v->IsArray()) {
          auto a = v.As<Array>();
          for (uint32_t i = 0; i < a->Length(); ++i) {
            Local<Value> e;
            if (a->Get(ctx, i).ToLocal(&e))
              o.buttons.push_back(to_str(iso, e));
          }
        }
      }
      int idx = fxe::os::show_message_box(o);
      auto result = Object::New(iso);
      (void)result->Set(ctx, "response"_v8(iso), Integer::New(iso, idx));
      info.GetReturnValue().Set(resolved(iso, ctx, result));
    }
  } // namespace

  void install_dialog_global(Isolate* iso, Local<ObjectTemplate> global) {
    auto t = ObjectTemplate::New(iso);
    t->Set(iso, "showOpenDialog", FunctionTemplate::New(iso, show_open));
    t->Set(iso, "showSaveDialog", FunctionTemplate::New(iso, show_save));
    t->Set(iso, "showMessageBox", FunctionTemplate::New(iso, show_message));
    global->Set(iso, "dialog", t);
  }
} // namespace fxe::js
