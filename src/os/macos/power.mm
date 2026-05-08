#include "../../../include/fxe/power.hpp"

#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <Network/Network.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <dispatch/dispatch.h>

#include <netinet/in.h>
#include <fxe/types.hpp>

namespace fxe::os {
  namespace {
    std::mutex g_power_mu;
    std::function<void(power_event)> g_power_cb;
    std::mutex g_network_mu;
    std::function<void(network_event)> g_network_cb;
    std::atomic<bool> g_workspace_registered{false};
    std::atomic<bool> g_power_source_registered{false};
    std::atomic<bool> g_network_registered{false};
    std::atomic<bool> g_last_network_online{true};
    nw_path_monitor_t g_monitor = nullptr;
    std::mutex g_inhibit_mu;
    std::unordered_map<u64, IOPMAssertionID> g_sleep_inhibits;
    std::atomic<u64> g_next_inhibit_id{1};

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

    bool power_source_is_battery() {
      CFTypeRef info = IOPSCopyPowerSourcesInfo();
      if (!info)
        return false;
      CFStringRef source = IOPSGetProvidingPowerSourceType(info);
      bool battery = source && CFEqual(source, kIOPSBatteryPowerValue);
      CFRelease(info);
      return battery;
    }

    void power_source_changed(void*) {
      emit_power(power_source_is_battery() ? power_event::on_battery : power_event::on_ac);
    }

    void ensure_workspace_notifications() {
      bool expected = false;
      if (!g_workspace_registered.compare_exchange_strong(expected, true))
        return;

      NSNotificationCenter* center = [[NSWorkspace sharedWorkspace] notificationCenter];
      [center addObserverForName:NSWorkspaceWillSleepNotification
                          object:nil
                           queue:nil
                      usingBlock:^(__unused NSNotification* note) {
                        emit_power(power_event::suspend);
                      }];
      [center addObserverForName:NSWorkspaceDidWakeNotification
                          object:nil
                           queue:nil
                      usingBlock:^(__unused NSNotification* note) {
                        emit_power(power_event::resume);
                      }];
      [center addObserverForName:NSWorkspaceScreensDidSleepNotification
                          object:nil
                           queue:nil
                      usingBlock:^(__unused NSNotification* note) {
                        emit_power(power_event::lock_screen);
                      }];
      [center addObserverForName:NSWorkspaceScreensDidWakeNotification
                          object:nil
                           queue:nil
                      usingBlock:^(__unused NSNotification* note) {
                        emit_power(power_event::unlock_screen);
                      }];
    }

    void ensure_power_source_notifications() {
      bool expected = false;
      if (!g_power_source_registered.compare_exchange_strong(expected, true))
        return;
      CFRunLoopSourceRef source =
          IOPSNotificationCreateRunLoopSource(power_source_changed, nullptr);
      if (!source)
        return;
      CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
      CFRelease(source);
    }

    void ensure_network_monitor() {
      bool expected = false;
      if (!g_network_registered.compare_exchange_strong(expected, true))
        return;
      g_monitor = nw_path_monitor_create();
      if (!g_monitor)
        return;
      nw_path_monitor_set_queue(g_monitor, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
      nw_path_monitor_set_update_handler(g_monitor, ^(nw_path_t path) {
        bool online = nw_path_get_status(path) == nw_path_status_satisfied;
        bool previous = g_last_network_online.exchange(online);
        if (previous != online)
          emit_network(online ? network_event::online : network_event::offline);
      });
      nw_path_monitor_start(g_monitor);
    }

    bool reachability_online() {
      sockaddr_in address{};
      address.sin_len = sizeof(address);
      address.sin_family = AF_INET;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      SCNetworkReachabilityRef reachability = SCNetworkReachabilityCreateWithAddress(
          nullptr, reinterpret_cast<const sockaddr*>(&address));
      if (!reachability)
        return g_last_network_online.load();
      SCNetworkReachabilityFlags flags = 0;
      bool ok = SCNetworkReachabilityGetFlags(reachability, &flags);
#pragma clang diagnostic pop
      CFRelease(reachability);
      if (!ok)
        return g_last_network_online.load();
      bool reachable = (flags & kSCNetworkReachabilityFlagsReachable) != 0;
      bool connection_required = (flags & kSCNetworkReachabilityFlagsConnectionRequired) != 0;
      bool online = reachable && !connection_required;
      g_last_network_online.store(online);
      return online;
    }

    power_inhibit_handle create_power_assertion(std::string_view reason, sleep_inhibit_kind what) {
      const char* fallback = "fxe requested sleep inhibit";
      const char* reason_data = reason.empty() ? fallback : reason.data();
      const usize reason_size = reason.empty() ? std::strlen(fallback) : reason.size();
      CFStringRef cf_reason = CFStringCreateWithBytes(
          nullptr, reinterpret_cast<const UInt8*>(reason_data), static_cast<CFIndex>(reason_size),
          kCFStringEncodingUTF8, false);
      if (!cf_reason)
        return {};

      IOPMAssertionID assertion = kIOPMNullAssertionID;
      CFStringRef assertion_type = what == sleep_inhibit_kind::idle
                                      ? kIOPMAssertionTypePreventUserIdleSystemSleep
                                      : kIOPMAssertionTypePreventSystemSleep;
      IOReturn rc = IOPMAssertionCreateWithName(assertion_type, kIOPMAssertionLevelOn,
                                                cf_reason, &assertion);
      CFRelease(cf_reason);
      if (rc != kIOReturnSuccess || assertion == kIOPMNullAssertionID)
        return {};

      const u64 id = g_next_inhibit_id.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(g_inhibit_mu);
        g_sleep_inhibits.emplace(id, assertion);
      }
      return power_inhibit_handle{id};
    }
  } // namespace

  void power_register(std::function<void(power_event)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_power_mu);
      g_power_cb = std::move(cb);
    }
    ensure_workspace_notifications();
    ensure_power_source_notifications();
  }

  void network_register(std::function<void(network_event)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_network_mu);
      g_network_cb = std::move(cb);
    }
    ensure_network_monitor();
  }

  bool is_on_battery() {
    return power_source_is_battery();
  }

  bool is_network_online() {
    return reachability_online();
  }

  int system_idle_seconds() {
    CFTimeInterval seconds = CGEventSourceSecondsSinceLastEventType(
        kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    if (seconds < 0)
      return 0;
    return static_cast<int>(seconds);
  }

  power_inhibit_handle inhibit_sleep(std::string_view reason, sleep_inhibit_kind what) {
    return create_power_assertion(reason, what);
  }

  void release_sleep_inhibit(power_inhibit_handle handle) {
    if (!handle)
      return;
    IOPMAssertionID assertion = kIOPMNullAssertionID;
    {
      std::lock_guard<std::mutex> lock(g_inhibit_mu);
      auto it = g_sleep_inhibits.find(handle.id);
      if (it == g_sleep_inhibits.end())
        return;
      assertion = it->second;
      g_sleep_inhibits.erase(it);
    }
    if (assertion != kIOPMNullAssertionID)
      IOPMAssertionRelease(assertion);
  }
} // namespace fxe::os
