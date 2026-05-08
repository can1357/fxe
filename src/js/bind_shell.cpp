#include "bind_shell.hpp"
#include "../os/os.hpp"
#include "runtime/capabilities.hpp"

#include <fxe/v8_strings.hpp>
#include <string>
#include <string_view>
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

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    Local<Value> make_permission_denied(Isolate* iso, std::string_view what) {
      std::string msg = "Permission denied: shell access denied for '";
      msg.append(what);
      msg += "'";
      auto err = Exception::Error(str(iso, msg)).As<Object>();
      (void)err->Set(iso->GetCurrentContext(), "name"_v8(iso), "PermissionDenied"_v8(iso));
      return err;
    }

    bool guard_shell(Isolate* iso, std::string_view what) {
      if (fxe::runtime::shell_allowed())
        return true;
      iso->ThrowException(make_permission_denied(iso, what));
      return false;
    }

    void shell_open_external(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto url = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      if (!guard_shell(iso, url))
        return;
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::open_external(url)));
    }
    void shell_show_item_in_folder(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto p = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      if (!guard_shell(iso, p))
        return;
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::show_item_in_folder(p)));
    }
    void shell_beep(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!guard_shell(iso, "beep"))
        return;
      fxe::os::beep();
    }
    void shell_trash_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto p = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      if (!guard_shell(iso, p))
        return;
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::trash_item(p)));
    }
  } // namespace

  void install_shell_global(Isolate* iso, Local<ObjectTemplate> global) {
    auto t = ObjectTemplate::New(iso);
    t->Set(iso, "openExternal", FunctionTemplate::New(iso, shell_open_external));
    t->Set(iso, "showItemInFolder", FunctionTemplate::New(iso, shell_show_item_in_folder));
    t->Set(iso, "beep", FunctionTemplate::New(iso, shell_beep));
    t->Set(iso, "trashItem", FunctionTemplate::New(iso, shell_trash_item));
    global->Set(iso, "shell", t);
  }
} // namespace fxe::js
