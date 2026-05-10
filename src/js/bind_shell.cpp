#include "bind_shell.hpp"
#include "../os/os.hpp"
#include "runtime/capabilities.hpp"

#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <string>
#include <string_view>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {

    Local<Value> make_permission_denied(Isolate* iso, std::string_view what) {
      std::string msg = "Permission denied: shell access denied for '";
      msg.append(what);
      msg += "'";
      auto err = Exception::Error(to_v8_string(iso, msg)).As<Object>();
      set_prop(iso->GetCurrentContext(), err, "name"_v8, "PermissionDenied"_v8);
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
      auto url = info.Length() > 0 ? to_std_string_strict(iso, info[0]) : std::string{};
      if (!guard_shell(iso, url))
        return;
      info.GetReturnValue().Set(to_v8(iso, fxe::os::open_external(url)));
    }
    void shell_show_item_in_folder(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto p = info.Length() > 0 ? to_std_string_strict(iso, info[0]) : std::string{};
      if (!guard_shell(iso, p))
        return;
      info.GetReturnValue().Set(to_v8(iso, fxe::os::show_item_in_folder(p)));
    }
    void shell_beep(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!guard_shell(iso, "beep"))
        return;
      fxe::os::beep();
    }
    void shell_trash_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto p = info.Length() > 0 ? to_std_string_strict(iso, info[0]) : std::string{};
      if (!guard_shell(iso, p))
        return;
      info.GetReturnValue().Set(to_v8(iso, fxe::os::trash_item(p)));
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
