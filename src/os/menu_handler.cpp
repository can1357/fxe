// Cross-platform storage for the application-menu command handler.
// Platform menu dispatch (macOS NSMenu, Win32 WM_COMMAND, Linux dbusmenu)
// posts to the main thread and then invokes
// `detail::dispatch_application_menu_command(id)`, which routes to the
// JS-installed handler under a single lock.

#include "os.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace fxe::os {
  namespace {
    std::mutex g_menu_handler_mu;
    std::function<void(const std::string& id)> g_menu_handler;
  } // namespace

  void set_application_menu_handler(std::function<void(const std::string& id)> handler) {
    std::lock_guard<std::mutex> lock(g_menu_handler_mu);
    g_menu_handler = std::move(handler);
  }

  namespace detail {
    void dispatch_application_menu_command(const std::string& id) {
      std::function<void(const std::string&)> cb;
      {
        std::lock_guard<std::mutex> lock(g_menu_handler_mu);
        cb = g_menu_handler;
      }
      if (cb)
        cb(id);
    }
  } // namespace detail
} // namespace fxe::os
