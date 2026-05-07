#include "os/os.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
} // namespace

#if !defined(__APPLE__) && !defined(_WIN32)
namespace fxe::os::linux_smoke_test {
  std::string normalize_accelerator_for_portal(std::string_view accelerator);
  bool dbus_menu_model_round_trip();
} // namespace fxe::os::linux_smoke_test

namespace {
  void test_accelerator_parser() {
    using fxe::os::linux_smoke_test::normalize_accelerator_for_portal;
    CHECK(normalize_accelerator_for_portal("Cmd+Shift+P") == "Meta+Shift+P");
    CHECK(normalize_accelerator_for_portal("Ctrl+Alt+Delete") == "Ctrl+Alt+Delete");
    CHECK(normalize_accelerator_for_portal("CommandOrControl+Q") == "Ctrl+Q");
    CHECK(normalize_accelerator_for_portal("Super+Space") == "Meta+Space");
    CHECK(normalize_accelerator_for_portal("Option+Return") == "Alt+Enter");
    CHECK(normalize_accelerator_for_portal("Control+Shift+plus") == "Ctrl+Shift++");
  }

  void test_dbus_unavailable_fallbacks() {
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:abstract=fxe-test-nonexistent", 1);

    fxe::os::notification_options notification;
    notification.title = "fxe smoke";
    notification.body = "dbus unavailable";
    CHECK(fxe::os::show_notification(notification) == 0);

    fxe::os::tray_handle tray = fxe::os::tray_create("", "fxe smoke");
    CHECK(!tray);
    fxe::os::tray_set_menu(tray, {});
    fxe::os::tray_destroy(tray);
  }

  void test_dbus_menu_model() {
    CHECK(fxe::os::linux_smoke_test::dbus_menu_model_round_trip());
    fxe::os::set_application_menu({});
  }
} // namespace
#endif

int main() {
#if !defined(__APPLE__) && !defined(_WIN32)
  test_accelerator_parser();
  test_dbus_unavailable_fallbacks();
  test_dbus_menu_model();
#endif
  std::printf("os_linux_smoke: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
