#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fxe::os::a11y {
  // Cached accessibility snapshot for a window. Immutable shared_ptr.
  struct snapshot {
    std::string json;
    uint64_t generation = 0;
    std::string focused_id;
  };

  // Install or replace the snapshot for a window. Thread-safe.
  void install_snapshot(void* native_window, std::shared_ptr<const snapshot> next);

  // Read the current snapshot. May be null.
  std::shared_ptr<const snapshot> snapshot_for_window(void* native_window);

  // Update focused id (no full snapshot replacement).
  void set_focused_node(void* native_window, std::string_view id);
} // namespace fxe::os::a11y
