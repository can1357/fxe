// v0 IBus IME bridge over the session D-Bus.
// Limitations: keystroke routing through ProcessKeyEvent is deferred, so this
// only forwards UpdatePreeditText/CommitText signals; Fcitx is not supported.
// Extend by wiring focus/key events from the window backend and by adding an
// alternate Fcitx client path when we need broader Linux IME coverage.

#include "../os/os.hpp"

#if FXE_OS_DBUS && defined(__linux__) && !defined(__APPLE__)
#include <dbus/dbus.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fxe::os {
  namespace {
    constexpr const char* kDbusBus = "org.freedesktop.DBus";
    constexpr const char* kDbusPath = "/org/freedesktop/DBus";
    constexpr const char* kDbusIface = "org.freedesktop.DBus";
    constexpr const char* kIBusBus = "org.freedesktop.IBus";
    constexpr const char* kIBusPath = "/org/freedesktop/IBus";
    constexpr const char* kIBusIface = "org.freedesktop.IBus";
    constexpr const char* kInputContextIface = "org.freedesktop.IBus.InputContext";
    constexpr dbus_uint32_t kCapabilities = 1u | 2u | 4u | 8u;

    struct linux_ime_bridge {
      void* owner = nullptr;
      linux_ime_emit_fn emit = nullptr;
      DBusConnection* conn = nullptr;
      std::string ic_path;
      std::thread thread;
      std::atomic<bool> running{true};
    };

    void free_error(DBusError& error) {
      if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    }

    void close_connection(DBusConnection* conn) {
      if (!conn)
        return;
      if (dbus_connection_get_is_connected(conn) != FALSE)
        dbus_connection_close(conn);
      dbus_connection_unref(conn);
    }

    std::mutex& bridge_mutex() {
      static std::mutex mu;
      return mu;
    }

    std::vector<std::shared_ptr<linux_ime_bridge>>& bridge_registry() {
      static std::vector<std::shared_ptr<linux_ime_bridge>> bridges;
      return bridges;
    }

    void shutdown_linux_ime_bridges() {
      std::vector<std::shared_ptr<linux_ime_bridge>> bridges;
      {
        std::lock_guard<std::mutex> lock(bridge_mutex());
        bridges = bridge_registry();
        bridge_registry().clear();
      }
      for (const auto& bridge : bridges) {
        bridge->running.store(false, std::memory_order_release);
        if (bridge->conn && dbus_connection_get_is_connected(bridge->conn) != FALSE)
          dbus_connection_close(bridge->conn);
      }
      for (const auto& bridge : bridges) {
        if (bridge->thread.joinable())
          bridge->thread.join();
        close_connection(bridge->conn);
        bridge->conn = nullptr;
      }
    }

    void ensure_shutdown_registered() {
      static std::once_flag once;
      std::call_once(once, [] { std::atexit(shutdown_linux_ime_bridges); });
    }

    void ensure_dbus_threads() {
      static std::once_flag once;
      std::call_once(once, [] { dbus_threads_init_default(); });
    }

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
        free_error(error);
        return false;
      }
      dbus_bool_t has_owner = FALSE;
      dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &has_owner, DBUS_TYPE_INVALID);
      dbus_message_unref(reply);
      free_error(error);
      return has_owner != FALSE;
    }

    bool add_match_rule(DBusConnection* conn, const std::string& rule) {
      if (!conn || rule.empty())
        return false;
      DBusError error;
      dbus_error_init(&error);
      dbus_bus_add_match(conn, rule.c_str(), &error);
      const bool ok = !dbus_error_is_set(&error);
      free_error(error);
      if (ok)
        dbus_connection_flush(conn);
      return ok;
    }

    bool call_void_method(DBusConnection* conn, const std::string& path, const char* iface,
                          const char* member) {
      if (!conn || path.empty() || !iface || !member)
        return false;
      DBusMessage* msg = dbus_message_new_method_call(kIBusBus, path.c_str(), iface, member);
      if (!msg)
        return false;
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      const bool ok = reply != nullptr;
      if (reply)
        dbus_message_unref(reply);
      free_error(error);
      return ok;
    }

    bool call_uint32_method(DBusConnection* conn, const std::string& path, const char* iface,
                            const char* member, dbus_uint32_t value) {
      if (!conn || path.empty() || !iface || !member)
        return false;
      DBusMessage* msg = dbus_message_new_method_call(kIBusBus, path.c_str(), iface, member);
      if (!msg)
        return false;
      DBusMessageIter iter;
      dbus_message_iter_init_append(msg, &iter);
      dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &value);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      const bool ok = reply != nullptr;
      if (reply)
        dbus_message_unref(reply);
      free_error(error);
      return ok;
    }

    std::string create_input_context(DBusConnection* conn, const char* client_name) {
      if (!conn || !client_name)
        return {};
      DBusMessage* msg =
          dbus_message_new_method_call(kIBusBus, kIBusPath, kIBusIface, "CreateInputContext");
      if (!msg)
        return {};
      dbus_message_append_args(msg, DBUS_TYPE_STRING, &client_name, DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        free_error(error);
        return {};
      }
      const char* object_path = nullptr;
      dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &object_path, DBUS_TYPE_INVALID);
      std::string path = object_path ? std::string(object_path) : std::string{};
      dbus_message_unref(reply);
      free_error(error);
      return path;
    }

    std::string extract_ibus_text(DBusMessageIter* iter) {
      if (!iter)
        return {};
      const int type = dbus_message_iter_get_arg_type(iter);
      if (type == DBUS_TYPE_STRING) {
        const char* raw = nullptr;
        dbus_message_iter_get_basic(iter, &raw);
        return raw ? std::string(raw) : std::string{};
      }
      if (type != DBUS_TYPE_ARRAY && type != DBUS_TYPE_VARIANT && type != DBUS_TYPE_STRUCT &&
          type != DBUS_TYPE_DICT_ENTRY) {
        return {};
      }

      DBusMessageIter child;
      dbus_message_iter_recurse(iter, &child);
      if (type == DBUS_TYPE_STRUCT) {
        DBusMessageIter second = child;
        if (dbus_message_iter_get_arg_type(&second) != DBUS_TYPE_INVALID &&
            dbus_message_iter_next(&second) &&
            dbus_message_iter_get_arg_type(&second) == DBUS_TYPE_STRING) {
          const char* raw = nullptr;
          dbus_message_iter_get_basic(&second, &raw);
          return raw ? std::string(raw) : std::string{};
        }
      }
      for (DBusMessageIter sub = child; dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID;
           dbus_message_iter_next(&sub)) {
        std::string text = extract_ibus_text(&sub);
        if (!text.empty() || dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING)
          return text;
      }
      return {};
    }

    void handle_signal(const std::shared_ptr<linux_ime_bridge>& bridge, DBusMessage* msg) {
      if (!bridge || !bridge->emit || !msg)
        return;
      if (dbus_message_is_signal(msg, kInputContextIface, "UpdatePreeditText")) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter))
          return;
        std::string preedit = extract_ibus_text(&iter);
        dbus_uint32_t cursor = 0;
        dbus_bool_t visible = TRUE;
        if (dbus_message_iter_next(&iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
          dbus_message_iter_get_basic(&iter, &cursor);
        if (dbus_message_iter_next(&iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_BOOLEAN)
          dbus_message_iter_get_basic(&iter, &visible);
        bridge->emit(bridge->owner, visible ? preedit.c_str() : "", static_cast<int>(cursor), "");
        return;
      }
      if (dbus_message_is_signal(msg, kInputContextIface, "CommitText")) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter))
          return;
        std::string committed = extract_ibus_text(&iter);
        bridge->emit(bridge->owner, "", 0, committed.c_str());
      }
    }

    void bridge_thread_main(const std::shared_ptr<linux_ime_bridge>& bridge) {
      if (!bridge || !bridge->conn)
        return;
      while (bridge->running.load(std::memory_order_acquire)) {
        if (dbus_connection_read_write(bridge->conn, 100) == FALSE)
          break;
        for (;;) {
          DBusMessage* msg = dbus_connection_pop_message(bridge->conn);
          if (!msg)
            break;
          handle_signal(bridge, msg);
          dbus_message_unref(msg);
        }
        if (dbus_connection_get_is_connected(bridge->conn) == FALSE)
          break;
      }
      if (dbus_connection_get_is_connected(bridge->conn) != FALSE)
        (void)call_void_method(bridge->conn, bridge->ic_path, kInputContextIface, "FocusOut");
    }
  } // namespace

  bool install_linux_ime_bridge(void* owner, linux_ime_emit_fn emit) {
    if (!owner || !emit)
      return false;

    ensure_dbus_threads();
    ensure_shutdown_registered();

    DBusError error;
    dbus_error_init(&error);
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!conn) {
      free_error(error);
      return false;
    }
    free_error(error);
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    if (!name_has_owner(conn, kIBusBus)) {
      close_connection(conn);
      return false;
    }

    std::string ic_path = create_input_context(conn, "fxe");
    if (ic_path.empty() ||
        !call_uint32_method(conn, ic_path, kInputContextIface, "SetCapabilities", kCapabilities) ||
        !call_void_method(conn, ic_path, kInputContextIface, "FocusIn")) {
      close_connection(conn);
      return false;
    }

    if (!add_match_rule(conn, "type='signal',interface='org.freedesktop.IBus.InputContext',path='" +
                                  ic_path + "',member='UpdatePreeditText'") ||
        !add_match_rule(conn, "type='signal',interface='org.freedesktop.IBus.InputContext',path='" +
                                  ic_path + "',member='CommitText'")) {
      close_connection(conn);
      return false;
    }

    auto bridge = std::make_shared<linux_ime_bridge>();
    bridge->owner = owner;
    bridge->emit = emit;
    bridge->conn = conn;
    bridge->ic_path = std::move(ic_path);
    try {
      bridge->thread = std::thread([bridge] { bridge_thread_main(bridge); });
    } catch (...) {
      close_connection(conn);
      return false;
    }

    std::lock_guard<std::mutex> lock(bridge_mutex());
    bridge_registry().push_back(std::move(bridge));
    return true;
  }
} // namespace fxe::os
#endif
