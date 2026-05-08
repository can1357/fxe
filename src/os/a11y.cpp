#include "os/a11y.hpp"

#include <mutex>
#include <unordered_map>

namespace fxe::os::a11y {
  namespace {
    std::mutex g_mu;
    std::unordered_map<void*, std::shared_ptr<const snapshot>> g_snapshots;
  } // namespace

  void install_snapshot(void* native_window, std::shared_ptr<const snapshot> next) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_snapshots[native_window] = std::move(next);
  }

  std::shared_ptr<const snapshot> snapshot_for_window(void* native_window) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_snapshots.find(native_window);
    return it == g_snapshots.end() ? nullptr : it->second;
  }

  void set_focused_node(void* native_window, std::string_view id) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_snapshots.find(native_window);
    if (it == g_snapshots.end() || !it->second)
      return;
    auto next = std::make_shared<snapshot>(*it->second);
    next->focused_id = std::string{id};
    it->second = std::move(next);
  }
} // namespace fxe::os::a11y
