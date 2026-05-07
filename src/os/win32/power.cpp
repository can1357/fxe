#include "../../../include/fxe/power.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <wtsapi32.h>

namespace fxe::os {
  namespace {
    constexpr wchar_t kPowerWindowClass[] = L"fxe_power_monitor_window";
    constexpr GUID kGuidAcDcPowerSource = {
        0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};

    std::mutex g_power_mu;
    std::function<void(power_event)> g_power_cb;
    std::mutex g_network_mu;
    std::function<void(network_event)> g_network_cb;
    std::once_flag g_window_once;
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_last_online{true};
    std::mutex g_inhibit_mu;
    std::unordered_map<std::uint64_t, HANDLE> g_sleep_inhibits;
    std::atomic<std::uint64_t> g_next_inhibit_id{1};

    void emit_power(power_event event) {
      std::function<void(power_event)> cb;
      {
        std::lock_guard<std::mutex> lock(g_power_mu);
        cb = g_power_cb;
      }
      if (cb)
        cb(event);
    }

    void emit_network(network_event event) {
      std::function<void(network_event)> cb;
      {
        std::lock_guard<std::mutex> lock(g_network_mu);
        cb = g_network_cb;
      }
      if (cb)
        cb(event);
    }

    bool query_online() {
      DWORD flags = 0;
      return InternetGetConnectedState(&flags, 0) == TRUE;
    }

    void poll_network() {
      bool previous = query_online();
      g_last_online.store(previous);
      while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        bool online = query_online();
        bool old = g_last_online.exchange(online);
        if (old != online)
          emit_network(online ? network_event::online : network_event::offline);
      }
    }

    void handle_power_setting(const POWERBROADCAST_SETTING* setting) {
      if (!setting ||
          std::memcmp(&setting->PowerSetting, &kGuidAcDcPowerSource, sizeof(GUID)) != 0 ||
          setting->DataLength < sizeof(DWORD))
        return;
      DWORD source = *reinterpret_cast<const DWORD*>(setting->Data);
      if (source == 0)
        emit_power(power_event::on_ac);
      else if (source == 1)
        emit_power(power_event::on_battery);
    }

    LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
      switch (msg) {
      case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND)
          emit_power(power_event::suspend);
        else if (wparam == PBT_APMRESUMESUSPEND || wparam == PBT_APMRESUMEAUTOMATIC)
          emit_power(power_event::resume);
        else if (wparam == PBT_POWERSETTINGCHANGE)
          handle_power_setting(reinterpret_cast<const POWERBROADCAST_SETTING*>(lparam));
        return TRUE;
      case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOCK)
          emit_power(power_event::lock_screen);
        else if (wparam == WTS_SESSION_UNLOCK)
          emit_power(power_event::unlock_screen);
        return 0;
      case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
        g_running.store(false);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
      }
    }

    void message_thread() {
      HINSTANCE instance = GetModuleHandleW(nullptr);
      WNDCLASSW wc{};
      wc.lpfnWndProc = wndproc;
      wc.hInstance = instance;
      wc.lpszClassName = kPowerWindowClass;
      RegisterClassW(&wc);

      HWND hwnd = CreateWindowExW(0, kPowerWindowClass, L"fxe power monitor", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, instance, nullptr);
      if (!hwnd)
        return;
      WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
      HPOWERNOTIFY power_notify = RegisterPowerSettingNotification(hwnd, &kGuidAcDcPowerSource,
                                                                   DEVICE_NOTIFY_WINDOW_HANDLE);
      MSG msg;
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (power_notify)
        UnregisterPowerSettingNotification(power_notify);
      DestroyWindow(hwnd);
    }

    std::wstring widen(std::string_view s) {
      if (s.empty())
        return {};
      int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
      if (n <= 0)
        return {};
      std::wstring out(static_cast<size_t>(n), L'\0');
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                              out.data(), n) != n)
        return {};
      return out;
    }

    power_inhibit_handle create_power_request(std::string_view reason, sleep_inhibit_kind what) {
      std::wstring reason_text =
          widen(reason.empty() ? std::string_view("fxe requested sleep inhibit") : reason);
      if (reason_text.empty())
        reason_text = L"fxe requested sleep inhibit";

      REASON_CONTEXT context{};
      context.Version = POWER_REQUEST_CONTEXT_VERSION;
      context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
      context.Reason.SimpleReasonString = reason_text.data();
      HANDLE request = PowerCreateRequest(&context);
      if (!request || request == INVALID_HANDLE_VALUE)
        return {};

      POWER_REQUEST_TYPE request_type = what == sleep_inhibit_kind::idle
                                            ? PowerRequestDisplayRequired
                                            : PowerRequestSystemRequired;
      if (!PowerSetRequest(request, request_type)) {
        CloseHandle(request);
        return {};
      }

      const std::uint64_t id = g_next_inhibit_id.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(g_inhibit_mu);
        g_sleep_inhibits.emplace(id, request);
      }
      return power_inhibit_handle{id};
    }
    void ensure_window_thread() {
      std::call_once(g_window_once, [] {
        g_running.store(true);
        std::thread(message_thread).detach();
        std::thread(poll_network).detach();
      });
    }
  } // namespace

  void power_register(std::function<void(power_event)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_power_mu);
      g_power_cb = std::move(cb);
    }
    ensure_window_thread();
  }

  void network_register(std::function<void(network_event)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_network_mu);
      g_network_cb = std::move(cb);
    }
    ensure_window_thread();
  }

  bool is_on_battery() {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status))
      return false;
    return status.ACLineStatus == 0;
  }

  bool is_network_online() {
    bool online = query_online();
    g_last_online.store(online);
    return online;
  }

  int system_idle_seconds() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info))
      return 0;
    DWORD now = GetTickCount();
    return static_cast<int>((now - info.dwTime) / 1000);
  }

  power_inhibit_handle inhibit_sleep(std::string_view reason, sleep_inhibit_kind what) {
    return create_power_request(reason, what);
  }

  void release_sleep_inhibit(power_inhibit_handle handle) {
    if (!handle)
      return;
    HANDLE request = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_inhibit_mu);
      auto it = g_sleep_inhibits.find(handle.id);
      if (it == g_sleep_inhibits.end())
        return;
      request = it->second;
      g_sleep_inhibits.erase(it);
    }
    POWER_REQUEST_TYPE display_type = PowerRequestDisplayRequired;
    POWER_REQUEST_TYPE system_type = PowerRequestSystemRequired;
    (void)PowerClearRequest(request, display_type);
    (void)PowerClearRequest(request, system_type);
    CloseHandle(request);
  }
} // namespace fxe::os
