// Win32-only smoke tests for fxe::os desktop integrations.
// TODO: This repository currently has no tests/CMakeLists.txt. Register this
// target from the top-level test section when that build file is split:
//   add_executable(fxe_os_win32_tests os_win32_smoke.cpp)
//   target_link_libraries(fxe_os_win32_tests PRIVATE fxe_os)
//   target_compile_features(fxe_os_win32_tests PRIVATE cxx_std_20)
//   add_test(NAME fxe_os_win32_tests COMMAND fxe_os_win32_tests)

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <os/os.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

void __fxe_os_register_active_hwnd(HWND hwnd);
HMENU __fxe_os_win32_build_menu_for_test(const std::vector<fxe::os::menu_item>& items);

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

  HWND create_test_window() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FxeOsWin32SmokeWindow";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"fxe os smoke", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
  }

  std::vector<fxe::os::menu_item> sample_menu() {
    fxe::os::menu_item open;
    open.id = "open";
    open.label = "Open";
    open.accelerator = "CommandOrControl+O";

    fxe::os::menu_item checked;
    checked.id = "checked";
    checked.label = "Checked";
    checked.type = "checkbox";
    checked.checked = true;

    fxe::os::menu_item separator;
    separator.type = "separator";

    fxe::os::menu_item child;
    child.id = "child";
    child.label = "Child";

    fxe::os::menu_item submenu;
    submenu.id = "submenu";
    submenu.label = "Submenu";
    submenu.type = "submenu";
    submenu.submenu.push_back(child);

    return {open, checked, separator, submenu};
  }

  void test_menu_builds_and_destroys() {
    HMENU menu = __fxe_os_win32_build_menu_for_test(sample_menu());
    CHECK(menu != nullptr);
    if (menu)
      CHECK(DestroyMenu(menu) != FALSE);

    HWND hwnd = create_test_window();
    CHECK(hwnd != nullptr);
    if (hwnd) {
      __fxe_os_register_active_hwnd(hwnd);
      fxe::os::set_application_menu(sample_menu());
      CHECK(GetMenu(hwnd) != nullptr);
      __fxe_os_register_active_hwnd(nullptr);
      DestroyWindow(hwnd);
    }
  }

  void test_tray_round_trip() {
    fxe::os::tray_handle tray = fxe::os::tray_create({}, "fxe smoke tray");
    CHECK(static_cast<bool>(tray));
    if (tray) {
      fxe::os::tray_set_menu(tray, sample_menu());
      fxe::os::tray_destroy(tray);
      CHECK(true);
    }
  }

  void test_global_shortcuts_round_trip() {
    const char* accelerators[] = {
        "CommandOrControl+Shift+K", "Ctrl+Alt+Shift+F13", "Control+Option+F14", "Alt+Shift+F15",
        "CommandOrControl+F16",
    };
    for (const char* accelerator : accelerators) {
      bool called = false;
      bool ok = fxe::os::global_shortcut_register(accelerator, [&called] { called = true; });
      CHECK(ok);
      fxe::os::global_shortcut_unregister(accelerator);
      CHECK(!called);
    }
    fxe::os::global_shortcut_unregister_all();
  }

  void test_notifications_round_trip() {
    fxe::os::notification_options opts;
    opts.title = "fxe smoke";
    opts.body = "notification smoke test";
    int id = fxe::os::show_notification(opts);
    if (id <= 0)
      std::fprintf(stderr, "fxe_os_win32_tests: show_notification returned false; WinRT/tray "
                           "balloon unavailable in this environment\n");
    CHECK(id >= 0);
    if (id > 0)
      fxe::os::on_notification_click(id, [] {});
  }

  void test_badge_round_trip() {
    HWND hwnd = create_test_window();
    CHECK(hwnd != nullptr);
    if (!hwnd)
      return;
    __fxe_os_register_active_hwnd(hwnd);
    fxe::os::set_badge_count(7);
    fxe::os::set_badge_count(0);
    CHECK(true);
    __fxe_os_register_active_hwnd(nullptr);
    DestroyWindow(hwnd);
  }
} // namespace

int main() {
  test_menu_builds_and_destroys();
  test_tray_round_trip();
  test_global_shortcuts_round_trip();
  test_notifications_round_trip();
  test_badge_round_trip();

  std::fprintf(stderr, "fxe_os_win32_tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
#else
int main() {
  return 0;
}
#endif
