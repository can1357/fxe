#include "../os/os.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <msctf.h>
#include <windows.h>
#elif defined(__linux__) && FXE_OS_DBUS && !defined(__APPLE__)
#include <dbus/dbus.h>
#endif

namespace fxe::os {
#if defined(_WIN32)
  const char* ime_backend() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
      return "imm32";
    const bool com_initialized = hr == S_OK || hr == S_FALSE;
    ITfThreadMgr* thread_mgr = nullptr;
    const HRESULT create_hr =
        CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                         reinterpret_cast<void**>(&thread_mgr));
    if (thread_mgr)
      thread_mgr->Release();
    if (com_initialized)
      CoUninitialize();
    return SUCCEEDED(create_hr) ? "tsf" : "imm32";
  }
#elif defined(__linux__) && FXE_OS_DBUS && !defined(__APPLE__)
  namespace {
    constexpr const char* kDbusBus = "org.freedesktop.DBus";
    constexpr const char* kDbusPath = "/org/freedesktop/DBus";
    constexpr const char* kDbusIface = "org.freedesktop.DBus";

    bool name_has_owner(DBusConnection* conn, const char* name) {
      if (!conn || !name)
        return false;
      DBusMessage* msg =
          dbus_message_new_method_call(kDbusBus, kDbusPath, kDbusIface, "NameHasOwner");
      if (!msg)
        return false;
      dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        if (dbus_error_is_set(&error))
          dbus_error_free(&error);
        return false;
      }
      dbus_bool_t has_owner = FALSE;
      dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &has_owner, DBUS_TYPE_INVALID);
      dbus_message_unref(reply);
      if (dbus_error_is_set(&error))
        dbus_error_free(&error);
      return has_owner != FALSE;
    }
  } // namespace

  const char* ime_backend() {
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!conn) {
      if (dbus_error_is_set(&error))
        dbus_error_free(&error);
      return "none";
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    const char* backend = "none";
    if (name_has_owner(conn, "org.freedesktop.IBus")) {
      backend = "ibus";
    } else if (name_has_owner(conn, "org.fcitx.Fcitx5")) {
      backend = "fcitx";
    } else if (name_has_owner(conn, "org.fcitx.Fcitx")) {
      backend = "fcitx";
    }
    if (dbus_connection_get_is_connected(conn) != FALSE)
      dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return backend;
  }
#else
  const char* ime_backend() {
    return "none";
  }
#endif
} // namespace fxe::os
