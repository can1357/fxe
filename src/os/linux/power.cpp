#include "../../../include/fxe/power.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#if defined(FXE_HAS_XSS) && FXE_HAS_XSS
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#endif

#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
#include <dbus/dbus.h>
#include <unistd.h>
#endif

namespace fxe::os {
  namespace {
    std::mutex g_power_mu;
    std::function<void(power_event)> g_power_cb;
    std::mutex g_network_mu;
    std::function<void(network_event)> g_network_cb;
    std::once_flag g_dbus_once;
#if !defined(FXE_HAS_XSS) || !FXE_HAS_XSS
    std::once_flag g_xss_warning_once;
#endif
    std::atomic<bool> g_last_network_online{true};
    std::atomic<bool> g_last_on_battery{false};
    std::mutex g_inhibit_mu;
    std::unordered_map<std::uint64_t, int> g_sleep_inhibits;
    std::atomic<std::uint64_t> g_next_inhibit_id{1};

    [[maybe_unused]] void emit_power(power_event event) {
      std::function<void(power_event)> cb;
      {
        std::lock_guard<std::mutex> lock(g_power_mu);
        cb = g_power_cb;
      }
      if (cb)
        cb(event);
    }

    [[maybe_unused]] void emit_network(network_event event) {
      std::function<void(network_event)> cb;
      {
        std::lock_guard<std::mutex> lock(g_network_mu);
        cb = g_network_cb;
      }
      if (cb)
        cb(event);
    }

    std::string read_first_line(const std::filesystem::path& path) {
      std::ifstream in(path);
      std::string line;
      std::getline(in, line);
      while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
        line.pop_back();
      return line;
    }

    bool sysfs_on_battery() {
      namespace fs = std::filesystem;
      bool saw_battery = false;
      bool battery_discharging = false;
      bool saw_mains_online = false;
      std::error_code ec;
      const fs::path root("/sys/class/power_supply");
      for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec)
          break;
        std::string type = read_first_line(entry.path() / "type");
        std::string status = read_first_line(entry.path() / "status");
        std::string online = read_first_line(entry.path() / "online");
        if (type == "Battery") {
          saw_battery = true;
          if (status == "Discharging")
            battery_discharging = true;
        } else if (type == "Mains" || type == "USB" || type == "USB_C" || type == "USB_PD") {
          if (online == "1")
            saw_mains_online = true;
        }
      }
      if (battery_discharging)
        return true;
      if (saw_mains_online)
        return false;
      return saw_battery && !saw_mains_online;
    }

    bool sysfs_network_online() {
      namespace fs = std::filesystem;
      std::error_code ec;
      const fs::path root("/sys/class/net");
      for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec)
          break;
        std::string name = entry.path().filename().string();
        if (name == "lo")
          continue;
        std::string carrier = read_first_line(entry.path() / "carrier");
        std::string operstate = read_first_line(entry.path() / "operstate");
        if (carrier == "1" || operstate == "up")
          return true;
      }
      return false;
    }

#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    power_inhibit_handle login1_inhibit_sleep(std::string_view reason, sleep_inhibit_kind what) {
      DBusError err;
      dbus_error_init(&err);
      DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
      if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return {};
      }
      if (!conn)
        return {};
      dbus_connection_set_exit_on_disconnect(conn, false);

      DBusMessage* msg =
          dbus_message_new_method_call("org.freedesktop.login1", "/org/freedesktop/login1",
                                       "org.freedesktop.login1.Manager", "Inhibit");
      if (!msg) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return {};
      }

      const char* inhibit_what = what == sleep_inhibit_kind::idle ? "idle" : "sleep";
      const char* who = "fxe";
      std::string why = reason.empty() ? "fxe requested sleep inhibit" : std::string(reason);
      const char* why_ptr = why.c_str();
      const char* mode = "block";
      if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &inhibit_what, DBUS_TYPE_STRING, &who,
                                    DBUS_TYPE_STRING, &why_ptr, DBUS_TYPE_STRING, &mode,
                                    DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return {};
      }

      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
      dbus_message_unref(msg);
      dbus_connection_close(conn);
      dbus_connection_unref(conn);

      if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return {};
      }
      if (!reply)
        return {};

      DBusMessageIter iter;
      int fd = -1;
      if (dbus_message_iter_init(reply, &iter) &&
          dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
        dbus_message_iter_get_basic(&iter, &fd);
      }
      dbus_message_unref(reply);
      if (fd < 0)
        return {};

      const std::uint64_t id = g_next_inhibit_id.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(g_inhibit_mu);
        g_sleep_inhibits.emplace(id, fd);
      }
      return power_inhibit_handle{id};
    }

    void add_match(DBusConnection* conn, const char* rule) {
      DBusError err;
      dbus_error_init(&err);
      dbus_bus_add_match(conn, rule, &err);
      dbus_connection_flush(conn);
      if (dbus_error_is_set(&err))
        dbus_error_free(&err);
    }

    void emit_battery_if_changed() {
      bool battery = sysfs_on_battery();
      bool previous = g_last_on_battery.exchange(battery);
      if (previous != battery)
        emit_power(battery ? power_event::on_battery : power_event::on_ac);
    }

    void emit_network_if_changed(bool online) {
      bool previous = g_last_network_online.exchange(online);
      if (previous != online)
        emit_network(online ? network_event::online : network_event::offline);
    }

    void dbus_monitor_thread() {
      DBusError err;
      dbus_error_init(&err);
      DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
      if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return;
      }
      if (!conn)
        return;

      add_match(conn, "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop."
                      "login1.Manager'");
      add_match(conn, "type='signal',sender='org.freedesktop.UPower'");
      add_match(conn, "type='signal',sender='org.freedesktop.NetworkManager',interface='org."
                      "freedesktop.NetworkManager'");

      while (dbus_connection_read_write(conn, -1)) {
        DBusMessage* msg = dbus_connection_pop_message(conn);
        if (!msg)
          continue;
        if (dbus_message_is_signal(msg, "org.freedesktop.login1.Manager", "PrepareForSleep")) {
          dbus_bool_t sleeping = false;
          if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_BOOLEAN, &sleeping, DBUS_TYPE_INVALID))
            emit_power(sleeping ? power_event::suspend : power_event::resume);
        } else if (dbus_message_is_signal(msg, "org.freedesktop.login1.Session", "Lock")) {
          emit_power(power_event::lock_screen);
        } else if (dbus_message_is_signal(msg, "org.freedesktop.login1.Session", "Unlock")) {
          emit_power(power_event::unlock_screen);
        } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged")) {
          const char* path = dbus_message_get_path(msg);
          if (path && std::string(path).find("/org/freedesktop/UPower") == 0)
            emit_battery_if_changed();
        } else if (dbus_message_is_signal(msg, "org.freedesktop.NetworkManager", "StateChanged")) {
          uint32_t state = 0;
          if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &state, DBUS_TYPE_INVALID))
            emit_network_if_changed(state >= 50 && state <= 70);
        }
        dbus_message_unref(msg);
      }
      dbus_connection_close(conn);
      dbus_connection_unref(conn);
    }

    void ensure_dbus_monitor() {
      std::call_once(g_dbus_once, [] { std::thread(dbus_monitor_thread).detach(); });
    }
#else
    power_inhibit_handle login1_inhibit_sleep(std::string_view, sleep_inhibit_kind) {
      return {};
    }
    void ensure_dbus_monitor() {
      std::call_once(g_dbus_once, [] {});
    }
#endif

#if !defined(FXE_HAS_XSS) || !FXE_HAS_XSS
    void warn_xss_disabled_once() {
      std::call_once(g_xss_warning_once, [] {
        std::fprintf(stderr, "fxe.os: systemIdleSeconds requires XScreenSaver (Xss) on Linux\n");
      });
    }
#endif
  } // namespace

  void power_register(std::function<void(power_event)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_power_mu);
      g_power_cb = std::move(cb);
    }
    g_last_on_battery.store(sysfs_on_battery());
    ensure_dbus_monitor();
  }

  void network_register(std::function<void(network_event)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_network_mu);
      g_network_cb = std::move(cb);
    }
    g_last_network_online.store(sysfs_network_online());
    ensure_dbus_monitor();
  }

  bool is_on_battery() {
    bool battery = sysfs_on_battery();
    g_last_on_battery.store(battery);
    return battery;
  }

  bool is_network_online() {
    bool online = sysfs_network_online();
    g_last_network_online.store(online);
    return online;
  }

  int system_idle_seconds() {
#if defined(FXE_HAS_XSS) && FXE_HAS_XSS
    Display* display = XOpenDisplay(nullptr);
    if (!display)
      return 0;

    int event_base = 0;
    int error_base = 0;
    if (!XScreenSaverQueryExtension(display, &event_base, &error_base)) {
      XCloseDisplay(display);
      return 0;
    }

    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    if (!info) {
      XCloseDisplay(display);
      return 0;
    }

    const bool ok = XScreenSaverQueryInfo(display, DefaultRootWindow(display), info) != 0;
    const unsigned long idle_ms = ok ? info->idle : 0;
    XFree(info);
    XCloseDisplay(display);

    const unsigned long idle_seconds = idle_ms / 1000UL;
    if (idle_seconds > static_cast<unsigned long>(std::numeric_limits<int>::max()))
      return std::numeric_limits<int>::max();
    return static_cast<int>(idle_seconds);
#else
    warn_xss_disabled_once();
    return 0;
#endif
  }

  power_inhibit_handle inhibit_sleep(std::string_view reason, sleep_inhibit_kind what) {
    return login1_inhibit_sleep(reason, what);
  }

  void release_sleep_inhibit(power_inhibit_handle handle) {
    if (!handle)
      return;
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(g_inhibit_mu);
      auto it = g_sleep_inhibits.find(handle.id);
      if (it == g_sleep_inhibits.end())
        return;
      fd = it->second;
      g_sleep_inhibits.erase(it);
    }
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (fd >= 0)
      ::close(fd);
#else
    (void)fd;
#endif
  }
} // namespace fxe::os
