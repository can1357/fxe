#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace fxe::os {
  enum class power_event {
    suspend,
    resume,
    lock_screen,
    unlock_screen,
    on_battery,
    on_ac,
    idle,
    active
  };
  enum class network_event { online, offline };

  enum class sleep_inhibit_kind { idle, sleep };

  struct power_inhibit_handle {
    std::uint64_t id = 0;
    explicit operator bool() const noexcept {
      return id != 0;
    }
  };

  void power_register(std::function<void(power_event)>);
  void network_register(std::function<void(network_event)>);
  bool is_on_battery();
  bool is_network_online();
  int system_idle_seconds();
  power_inhibit_handle inhibit_sleep(std::string_view reason, sleep_inhibit_kind what);
  void release_sleep_inhibit(power_inhibit_handle handle);
} // namespace fxe::os
