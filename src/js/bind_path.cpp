#include "bind_path.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <filesystem>
#include <string>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;
    namespace fs = std::filesystem;

    void path_join(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      fs::path p;
      for (int i = 0; i < info.Length(); ++i) {
        auto seg = to_std_string(iso, info[i]);
        if (seg.empty())
          continue;
        if (p.empty())
          p = seg;
        else
          p /= seg;
      }
      auto out = p.empty() ? std::string(".") : p.lexically_normal().generic_string();
      // Strip trailing slash unless it's just "/" or root.
      if (out.size() > 1 && out.back() == '/')
        out.pop_back();
      info.GetReturnValue().Set(to_v8_string(iso, out));
    }

    void path_resolve(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      fs::path p = fs::current_path();
      for (int i = 0; i < info.Length(); ++i) {
        auto seg = to_std_string(iso, info[i]);
        if (seg.empty())
          continue;
        fs::path s(seg);
        if (s.is_absolute())
          p = s;
        else
          p /= s;
      }
      info.GetReturnValue().Set(to_v8_string(iso, p.lexically_normal().generic_string()));
    }

    void path_dirname(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = to_std_string(iso, info[0]);
      auto out = fs::path(p).parent_path().generic_string();
      if (out.empty())
        out = ".";
      info.GetReturnValue().Set(to_v8_string(iso, out));
    }

    void path_basename(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = to_std_string(iso, info[0]);
      auto base = fs::path(p).filename().generic_string();
      if (info.Length() >= 2) {
        auto ext = to_std_string(iso, info[1]);
        if (!ext.empty() && base.size() >= ext.size() &&
            base.compare(base.size() - ext.size(), ext.size(), ext) == 0) {
          base.resize(base.size() - ext.size());
        }
      }
      info.GetReturnValue().Set(to_v8_string(iso, base));
    }

    void path_extname(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = to_std_string(iso, info[0]);
      auto out = fs::path(p).extension().generic_string();
      info.GetReturnValue().Set(to_v8_string(iso, out));
    }

    void path_relative(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto from = to_std_string(iso, info[0]);
      auto to = to_std_string(iso, info[1]);
      std::error_code ec;
      auto rel = fs::relative(fs::path(to), fs::path(from), ec);
      info.GetReturnValue().Set(to_v8_string(iso, ec ? to : rel.generic_string()));
    }

    void path_normalize(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = to_std_string(iso, info[0]);
      auto out = fs::path(p).lexically_normal().generic_string();
      if (out.empty())
        out = ".";
      info.GetReturnValue().Set(to_v8_string(iso, out));
    }

    void path_is_absolute(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = to_std_string(iso, info[0]);
      info.GetReturnValue().Set(fs::path(p).is_absolute());
    }
    void path_namespace_getter(Local<Name> /*name*/, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto ns = Object::New(iso);
      (void)ns->Set(ctx, "join"_v8(iso), Function::New(ctx, path_join).ToLocalChecked());
      (void)ns->Set(ctx, "resolve"_v8(iso), Function::New(ctx, path_resolve).ToLocalChecked());
      (void)ns->Set(ctx, "dirname"_v8(iso), Function::New(ctx, path_dirname).ToLocalChecked());
      (void)ns->Set(ctx, "basename"_v8(iso), Function::New(ctx, path_basename).ToLocalChecked());
      (void)ns->Set(ctx, "extname"_v8(iso), Function::New(ctx, path_extname).ToLocalChecked());
      (void)ns->Set(ctx, "relative"_v8(iso), Function::New(ctx, path_relative).ToLocalChecked());
      (void)ns->Set(ctx, "normalize"_v8(iso), Function::New(ctx, path_normalize).ToLocalChecked());
      (void)ns->Set(ctx, "isAbsolute"_v8(iso),
                    Function::New(ctx, path_is_absolute).ToLocalChecked());
#if defined(_WIN32)
      (void)ns->Set(ctx, "sep"_v8(iso), "\\"_v8(iso));
      (void)ns->Set(ctx, "delimiter"_v8(iso), ";"_v8(iso));
#else
      (void)ns->Set(ctx, "sep"_v8(iso), "/"_v8(iso));
      (void)ns->Set(ctx, "delimiter"_v8(iso), ":"_v8(iso));
#endif
      info.GetReturnValue().Set(ns);
    }
  } // namespace

  void install_path_global(Isolate* iso, Local<ObjectTemplate> global) {
    global->SetLazyDataProperty("path"_v8(iso), path_namespace_getter);
  }
} // namespace fxe::js
