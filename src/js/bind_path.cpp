#include <fxe/js_bindings.hpp>
#include <fxe/v8_strings.hpp>

#include "bind_path.hpp"

#include <filesystem>
#include <string>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;
    namespace fs = std::filesystem;

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      if (*u)
        return std::string(*u, u.length());
      return {};
    }

    Local<String> str(Isolate* iso, const std::string& s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    void path_join(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      fs::path p;
      for (int i = 0; i < info.Length(); ++i) {
        auto seg = utf8(iso, info[i]);
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
      info.GetReturnValue().Set(str(iso, out));
    }

    void path_resolve(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      fs::path p = fs::current_path();
      for (int i = 0; i < info.Length(); ++i) {
        auto seg = utf8(iso, info[i]);
        if (seg.empty())
          continue;
        fs::path s(seg);
        if (s.is_absolute())
          p = s;
        else
          p /= s;
      }
      info.GetReturnValue().Set(str(iso, p.lexically_normal().generic_string()));
    }

    void path_dirname(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = utf8(iso, info[0]);
      auto out = fs::path(p).parent_path().generic_string();
      if (out.empty())
        out = ".";
      info.GetReturnValue().Set(str(iso, out));
    }

    void path_basename(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = utf8(iso, info[0]);
      auto base = fs::path(p).filename().generic_string();
      if (info.Length() >= 2) {
        auto ext = utf8(iso, info[1]);
        if (!ext.empty() && base.size() >= ext.size() &&
            base.compare(base.size() - ext.size(), ext.size(), ext) == 0) {
          base.resize(base.size() - ext.size());
        }
      }
      info.GetReturnValue().Set(str(iso, base));
    }

    void path_extname(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = utf8(iso, info[0]);
      auto out = fs::path(p).extension().generic_string();
      info.GetReturnValue().Set(str(iso, out));
    }

    void path_relative(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto from = utf8(iso, info[0]);
      auto to = utf8(iso, info[1]);
      std::error_code ec;
      auto rel = fs::relative(fs::path(to), fs::path(from), ec);
      info.GetReturnValue().Set(str(iso, ec ? to : rel.generic_string()));
    }

    void path_normalize(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = utf8(iso, info[0]);
      auto out = fs::path(p).lexically_normal().generic_string();
      if (out.empty())
        out = ".";
      info.GetReturnValue().Set(str(iso, out));
    }

    void path_is_absolute(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = utf8(iso, info[0]);
      info.GetReturnValue().Set(fs::path(p).is_absolute());
    }
  } // namespace

  void install_path_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto t = ObjectTemplate::New(iso);
    t->Set(iso, "join", FunctionTemplate::New(iso, path_join));
    t->Set(iso, "resolve", FunctionTemplate::New(iso, path_resolve));
    t->Set(iso, "dirname", FunctionTemplate::New(iso, path_dirname));
    t->Set(iso, "basename", FunctionTemplate::New(iso, path_basename));
    t->Set(iso, "extname", FunctionTemplate::New(iso, path_extname));
    t->Set(iso, "relative", FunctionTemplate::New(iso, path_relative));
    t->Set(iso, "normalize", FunctionTemplate::New(iso, path_normalize));
    t->Set(iso, "isAbsolute", FunctionTemplate::New(iso, path_is_absolute));
#if defined(_WIN32)
    t->Set(iso, "sep", "\\"_v8(iso));
    t->Set(iso, "delimiter", ";"_v8(iso));
#else
    t->Set(iso, "sep", "/"_v8(iso));
    t->Set(iso, "delimiter", ":"_v8(iso));
#endif
    global->Set(iso, "path", t);
  }
} // namespace fxe::js
