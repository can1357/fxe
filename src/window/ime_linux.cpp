// v0 Linux IME bridge over the session D-Bus.
// Limitations: keystroke routing through ProcessKeyEvent remains deferred, so
// the IBus and Fcitx paths only forward preedit/commit signals through the
// existing callback surface.

#include "../os/os.hpp"

#if FXE_OS_DBUS && defined(__linux__) && !defined(__APPLE__)
#include <dbus/dbus.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fxe::os {
  namespace {
    constexpr const char* kDbusBus = "org.freedesktop.DBus";
    constexpr const char* kDbusPath = "/org/freedesktop/DBus";
    constexpr const char* kDbusIface = "org.freedesktop.DBus";
    constexpr const char* kIBusBus = "org.freedesktop.IBus";
    constexpr const char* kIBusPath = "/org/freedesktop/IBus";
    constexpr const char* kIBusIface = "org.freedesktop.IBus";
    constexpr const char* kIBusInputContextIface = "org.freedesktop.IBus.InputContext";
    constexpr const char* kFcitx5Bus = "org.fcitx.Fcitx5";
    constexpr const char* kFcitx5InputMethodPath = "/org/fcitx";
    constexpr const char* kFcitx5InputMethodIface = "org.fcitx.Fcitx.InputMethod1";
    constexpr const char* kFcitx5InputContextIface = "org.fcitx.Fcitx.InputContext1";
    constexpr const char* kFcitxBus = "org.fcitx.Fcitx";
    constexpr const char* kFcitxInputMethodPath = "/inputmethod";
    constexpr const char* kFcitxInputMethodIface = "org.fcitx.Fcitx.InputMethod";
    constexpr const char* kFcitxInputContextIface = "org.fcitx.Fcitx.InputContext";
    constexpr dbus_uint32_t kIBusCapabilities = 1u | 2u | 4u | 8u;

    enum class linux_ime_backend { ibus, fcitx5, fcitx4, none };

    struct linux_ime_bridge {
      void* owner = nullptr;
      linux_ime_emit_fn emit = nullptr;
      DBusConnection* conn = nullptr;
      linux_ime_backend backend = linux_ime_backend::none;
      std::string bus_name;
      std::string ic_path;
      std::string input_context_iface;
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

    linux_ime_backend probe_backend(DBusConnection* conn) {
      if (!conn)
        return linux_ime_backend::none;
      if (name_has_owner(conn, kIBusBus))
        return linux_ime_backend::ibus;
      if (name_has_owner(conn, kFcitx5Bus))
        return linux_ime_backend::fcitx5;
      if (name_has_owner(conn, kFcitxBus))
        return linux_ime_backend::fcitx4;
      return linux_ime_backend::none;
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

    bool call_void_method(DBusConnection* conn, const char* bus_name, const std::string& path,
                          const char* iface, const char* member) {
      if (!conn || !bus_name || path.empty() || !iface || !member)
        return false;
      DBusMessage* msg = dbus_message_new_method_call(bus_name, path.c_str(), iface, member);
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

    bool call_uint32_method(DBusConnection* conn, const char* bus_name, const std::string& path,
                            const char* iface, const char* member, dbus_uint32_t value) {
      if (!conn || !bus_name || path.empty() || !iface || !member)
        return false;
      DBusMessage* msg = dbus_message_new_method_call(bus_name, path.c_str(), iface, member);
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

    std::string create_ibus_input_context(DBusConnection* conn, const char* client_name) {
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

    std::string create_fcitx5_input_context(DBusConnection* conn) {
      if (!conn)
        return {};
      DBusMessage* msg = dbus_message_new_method_call(
          kFcitx5Bus, kFcitx5InputMethodPath, kFcitx5InputMethodIface, "CreateInputContext");
      if (!msg)
        return {};
      DBusMessageIter iter;
      dbus_message_iter_init_append(msg, &iter);
      DBusMessageIter array_iter;
      dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ss)", &array_iter);
      DBusMessageIter struct_iter;
      dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, nullptr, &struct_iter);
      const char* key = "program";
      const char* value = "fxe";
      dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &key);
      dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &value);
      dbus_message_iter_close_container(&array_iter, &struct_iter);
      dbus_message_iter_close_container(&iter, &array_iter);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        free_error(error);
        return {};
      }
      DBusMessageIter reply_iter;
      std::string path;
      if (dbus_message_iter_init(reply, &reply_iter) &&
          dbus_message_iter_get_arg_type(&reply_iter) == DBUS_TYPE_OBJECT_PATH) {
        const char* object_path = nullptr;
        dbus_message_iter_get_basic(&reply_iter, &object_path);
        if (object_path)
          path = object_path;
      }
      dbus_message_unref(reply);
      free_error(error);
      return path;
    }

    std::string create_fcitx4_input_context(DBusConnection* conn) {
      if (!conn)
        return {};
      DBusMessage* msg = dbus_message_new_method_call(kFcitxBus, kFcitxInputMethodPath,
                                                      kFcitxInputMethodIface, "CreateICv3");
      if (!msg)
        return {};
      const char* app_name = "fxe";
      const dbus_int32_t pid = static_cast<dbus_int32_t>(getpid());
      dbus_message_append_args(msg, DBUS_TYPE_STRING, &app_name, DBUS_TYPE_INT32, &pid,
                               DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        free_error(error);
        return {};
      }
      dbus_int32_t id = 0;
      dbus_message_get_args(reply, &error, DBUS_TYPE_INT32, &id, DBUS_TYPE_INVALID);
      dbus_message_unref(reply);
      free_error(error);
      if (id <= 0)
        return {};
      return "/inputcontext_" + std::to_string(id);
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

    std::string extract_fcitx_formatted_text(DBusMessageIter* iter) {
      if (!iter || dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
        return {};
      DBusMessageIter array_iter;
      dbus_message_iter_recurse(iter, &array_iter);
      std::string text;
      for (; dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID;
           dbus_message_iter_next(&array_iter)) {
        if (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_STRUCT)
          continue;
        DBusMessageIter struct_iter;
        dbus_message_iter_recurse(&array_iter, &struct_iter);
        if (dbus_message_iter_get_arg_type(&struct_iter) != DBUS_TYPE_STRING)
          continue;
        const char* chunk = nullptr;
        dbus_message_iter_get_basic(&struct_iter, &chunk);
        if (chunk)
          text.append(chunk);
      }
      return text;
    }

    void handle_fcitx5_signal(const std::shared_ptr<linux_ime_bridge>& bridge, DBusMessage* msg) {
      if (!bridge || !bridge->emit || !msg)
        return;
      if (dbus_message_is_signal(msg, kFcitx5InputContextIface, "UpdateFormattedPreedit")) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter))
          return;
        std::string preedit = extract_fcitx_formatted_text(&iter);
        dbus_int32_t cursor = 0;
        if (dbus_message_iter_next(&iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32)
          dbus_message_iter_get_basic(&iter, &cursor);
        bridge->emit(bridge->owner, preedit.c_str(), static_cast<int>(cursor), "");
        return;
      }
      if (dbus_message_is_signal(msg, kFcitx5InputContextIface, "CommitString")) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter))
          return;
        const char* committed = "";
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
          dbus_message_iter_get_basic(&iter, &committed);
        bridge->emit(bridge->owner, "", 0, committed ? committed : "");
      }
    }

    void handle_fcitx4_signal(const std::shared_ptr<linux_ime_bridge>& bridge, DBusMessage* msg) {
      if (!bridge || !bridge->emit || !msg)
        return;
      if (dbus_message_is_signal(msg, kFcitxInputContextIface, "UpdateFormattedPreedit") ||
          dbus_message_is_signal(msg, kFcitxInputContextIface, "UpdatePreedit")) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter))
          return;
        std::string preedit;
        dbus_int32_t cursor = 0;
        if (dbus_message_is_signal(msg, kFcitxInputContextIface, "UpdateFormattedPreedit")) {
          preedit = extract_fcitx_formatted_text(&iter);
          if (dbus_message_iter_next(&iter) &&
              dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32)
            dbus_message_iter_get_basic(&iter, &cursor);
        } else {
          if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            const char* raw = nullptr;
            dbus_message_iter_get_basic(&iter, &raw);
            if (raw)
              preedit = raw;
          }
          if (dbus_message_iter_next(&iter) &&
              dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32)
            dbus_message_iter_get_basic(&iter, &cursor);
        }
        bridge->emit(bridge->owner, preedit.c_str(), static_cast<int>(cursor), "");
        return;
      }
      if (dbus_message_is_signal(msg, kFcitxInputContextIface, "CommitString")) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter))
          return;
        const char* committed = "";
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
          dbus_message_iter_get_basic(&iter, &committed);
        bridge->emit(bridge->owner, "", 0, committed ? committed : "");
      }
    }

    void handle_signal(const std::shared_ptr<linux_ime_bridge>& bridge, DBusMessage* msg) {
      if (!bridge || !bridge->emit || !msg)
        return;
      if (bridge->backend == linux_ime_backend::ibus) {
        if (dbus_message_is_signal(msg, kIBusInputContextIface, "UpdatePreeditText")) {
          DBusMessageIter iter;
          if (!dbus_message_iter_init(msg, &iter))
            return;
          std::string preedit = extract_ibus_text(&iter);
          dbus_uint32_t cursor = 0;
          dbus_bool_t visible = TRUE;
          if (dbus_message_iter_next(&iter) &&
              dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&iter, &cursor);
          }
          if (dbus_message_iter_next(&iter) &&
              dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_BOOLEAN) {
            dbus_message_iter_get_basic(&iter, &visible);
          }
          bridge->emit(bridge->owner, visible ? preedit.c_str() : "", static_cast<int>(cursor), "");
          return;
        }
        if (dbus_message_is_signal(msg, kIBusInputContextIface, "CommitText")) {
          DBusMessageIter iter;
          if (!dbus_message_iter_init(msg, &iter))
            return;
          std::string committed = extract_ibus_text(&iter);
          bridge->emit(bridge->owner, "", 0, committed.c_str());
        }
        return;
      }
      if (bridge->backend == linux_ime_backend::fcitx5) {
        handle_fcitx5_signal(bridge, msg);
        return;
      }
      if (bridge->backend == linux_ime_backend::fcitx4)
        handle_fcitx4_signal(bridge, msg);
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
      if (dbus_connection_get_is_connected(bridge->conn) == FALSE)
        return;
      if (bridge->backend == linux_ime_backend::ibus) {
        (void)call_void_method(bridge->conn, bridge->bus_name.c_str(), bridge->ic_path,
                               bridge->input_context_iface.c_str(), "FocusOut");
      } else if (bridge->backend == linux_ime_backend::fcitx5 ||
                 bridge->backend == linux_ime_backend::fcitx4) {
        (void)call_void_method(bridge->conn, bridge->bus_name.c_str(), bridge->ic_path,
                               bridge->input_context_iface.c_str(), "FocusOut");
      }
    }

    bool connect_ibus_bridge(DBusConnection* conn, linux_ime_bridge& bridge) {
      std::string ic_path = create_ibus_input_context(conn, "fxe");
      if (ic_path.empty() ||
          !call_uint32_method(conn, kIBusBus, ic_path, kIBusInputContextIface, "SetCapabilities",
                              kIBusCapabilities) ||
          !call_void_method(conn, kIBusBus, ic_path, kIBusInputContextIface, "FocusIn")) {
        return false;
      }
      if (!add_match_rule(conn,
                          "type='signal',interface='org.freedesktop.IBus.InputContext',path='" +
                              ic_path + "',member='UpdatePreeditText'") ||
          !add_match_rule(conn,
                          "type='signal',interface='org.freedesktop.IBus.InputContext',path='" +
                              ic_path + "',member='CommitText'")) {
        return false;
      }
      bridge.backend = linux_ime_backend::ibus;
      bridge.bus_name = kIBusBus;
      bridge.ic_path = std::move(ic_path);
      bridge.input_context_iface = kIBusInputContextIface;
      return true;
    }

    bool connect_fcitx_bridge(DBusConnection* conn, linux_ime_bridge& bridge,
                              linux_ime_backend backend) {
      std::string ic_path;
      const char* bus_name = nullptr;
      const char* input_context_iface = nullptr;
      if (backend == linux_ime_backend::fcitx5) {
        bus_name = kFcitx5Bus;
        input_context_iface = kFcitx5InputContextIface;
        ic_path = create_fcitx5_input_context(conn);
      } else if (backend == linux_ime_backend::fcitx4) {
        bus_name = kFcitxBus;
        input_context_iface = kFcitxInputContextIface;
        ic_path = create_fcitx4_input_context(conn);
      } else {
        return false;
      }
      if (!bus_name || !input_context_iface || ic_path.empty() ||
          !call_void_method(conn, bus_name, ic_path, input_context_iface, "FocusIn")) {
        return false;
      }
      const char* preedit_member =
          backend == linux_ime_backend::fcitx5 ? "UpdateFormattedPreedit" : "UpdatePreedit";
      if (!add_match_rule(conn, "type='signal',interface='" + std::string(input_context_iface) +
                                    "',path='" + ic_path + "',member='" + preedit_member + "'") ||
          !add_match_rule(conn, "type='signal',interface='" + std::string(input_context_iface) +
                                    "',path='" + ic_path + "',member='CommitString'")) {
        return false;
      }
      if (backend == linux_ime_backend::fcitx4) {
        (void)add_match_rule(conn, "type='signal',interface='" + std::string(input_context_iface) +
                                       "',path='" + ic_path + "',member='UpdateFormattedPreedit'");
      }
      bridge.backend = backend;
      bridge.bus_name = bus_name;
      bridge.ic_path = std::move(ic_path);
      bridge.input_context_iface = input_context_iface;
      return true;
    }
  } // namespace

  bool install_linux_ime_bridge(void* owner, linux_ime_emit_fn emit) {
    if (!owner || !emit)
      return false;

    {
      std::lock_guard<std::mutex> lock(bridge_mutex());
      for (const auto& bridge : bridge_registry()) {
        if (bridge && bridge->owner == owner) {
          bridge->emit = emit;
          return bridge->conn != nullptr;
        }
      }
    }

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

    auto bridge = std::make_shared<linux_ime_bridge>();
    bridge->owner = owner;
    bridge->emit = emit;
    bridge->conn = conn;

    const linux_ime_backend backend = probe_backend(conn);
    bool ok = false;
    if (backend == linux_ime_backend::ibus) {
      ok = connect_ibus_bridge(conn, *bridge);
    } else if (backend == linux_ime_backend::fcitx5 || backend == linux_ime_backend::fcitx4) {
      ok = connect_fcitx_bridge(conn, *bridge, backend);
    }
    if (!ok) {
      close_connection(conn);
      return false;
    }

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
