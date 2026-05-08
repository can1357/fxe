#pragma once

// Platform-agnostic OS shims used by the JS host bindings (App, shell, dialog,
// notification, menu, tray, globalShortcut). Implementations live under
// src/os/<platform>/. macOS is the primary host; Win32 and Linux ship as
// stubs that return `false` / empty / runtime errors and are tagged with
// TODO markers for future work.

#include <cstdint>
#include <functional>
#include <fxe/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fxe::os {

  // -------- Paths / app helpers -------------------------------------------
  // Supported kinds: "userData", "documents", "downloads", "temp", "home".
  // Returns empty string on unknown kind or platform error.
  std::string get_path(std::string_view kind);

  bool open_external(std::string_view url);
  bool show_item_in_folder(std::string_view path);
  void beep();
  bool trash_item(std::string_view path);

  bool request_single_instance_lock(std::string_view app_id);
  void on_second_instance(std::function<void(std::vector<std::string> argv)> cb);
  void on_second_instance(std::function<void(std::vector<std::string> argv, std::string cwd)> cb);
  void set_badge_count(int n);
  void relaunch();
  std::optional<std::string> bookmark_persist(std::string_view path);
  std::optional<std::pair<std::string, bool>> bookmark_resolve(std::string_view blob);
  bool bookmark_start_access(std::string_view blob);
  void bookmark_stop_access(std::string_view blob);

  namespace app {
    bool add_recent_document(std::string_view path);
    std::vector<std::string> recent_documents();
    bool clear_recent_documents();
  } // namespace app
  // -------- System accessibility / appearance -----------------------------
  bool system_prefers_reduced_motion();
  bool system_prefers_high_contrast();
  double system_font_scale();        // 1.0 = default
  std::string system_color_scheme(); // "light" | "dark" | "no-preference"
  std::string system_accent_color(); // lowercase RRGGBB hex, empty if unknown
  // Install a change observer for system preferences. Callback fires on the
  // main thread when a known preference flips. Returns true if any observers
  // installed; false means callers should fall back to polling.
  bool install_system_change_observer(std::function<void(const char* kind)> cb);

  // -------- Dialogs --------------------------------------------------------
  struct dialog_filter {
    std::string name;
    std::vector<std::string> extensions; // without leading dots
  };

  struct open_dialog_options {
    std::string title;
    std::string default_path;
    std::vector<dialog_filter> filters;
    bool multiple = false;
    bool directories = false;
  };
  std::vector<std::string> show_open_dialog(const open_dialog_options&);

  struct save_dialog_options {
    std::string title;
    std::string default_path;
    std::vector<dialog_filter> filters;
  };
  std::optional<std::string> show_save_dialog(const save_dialog_options&);

  struct message_box_options {
    std::string title;
    std::string message;
    std::string detail;
    std::vector<std::string> buttons;
    std::string type = "info"; // info | warning | error | question
  };
  // Returns the index of the clicked button (0-based). -1 on error.
  int show_message_box(const message_box_options&);

  // -------- Notifications --------------------------------------------------
  enum class notification_action_kind { button, input };

  struct notification_action {
    std::string id;
    std::string title;
    notification_action_kind kind = notification_action_kind::button;
  };

  struct notification_options {
    std::string title;
    std::string body;
    std::string icon_path;
    std::vector<notification_action> actions;
    std::optional<std::string> image_path;
    std::optional<std::string> attachment_path;
  };
  // Returns a positive id, or 0 on failure. The click handler (if any) is set
  // separately so callers can use a fresh handler per show().
  int show_notification(const notification_options&);
  int show_notification(
      const notification_options&,
      std::function<void(const std::string& action_id, std::optional<std::string> input)>
          on_action);
  void on_notification_click(int id, std::function<void()> cb);
  void on_notification_action(
      int id,
      std::function<void(const std::string& action_id, std::optional<std::string> input)> cb);

  // -------- Menus ----------------------------------------------------------
  struct menu_item {
    std::string id;
    std::string label;
    std::string accelerator; // e.g. "Cmd+Shift+P"
    bool enabled = true;
    bool checked = false;
    std::string type = "normal"; // normal | separator | checkbox | submenu
    std::vector<menu_item> submenu;
  };

  void set_application_menu(const std::vector<menu_item>& items);
  void show_context_menu(const std::vector<menu_item>& items, int x, int y,
                         std::function<void(const std::string& id)> on_select);
  // Per-item mutation. menu_item is identified by its `id` (which must be unique per
  // menu, including submenus). Returns false when the id is not found OR the change
  // is not supported on the current platform.
  struct menu_item_patch {
    std::optional<std::string> label;
    std::optional<bool> enabled;
    std::optional<bool> checked;
    std::optional<bool> visible;
    std::optional<std::string> accelerator;
  };
  bool update_menu_item(std::string_view id, const menu_item_patch& patch);
  bool menu_item_exists(std::string_view id);

  // -------- Tray -----------------------------------------------------------
  struct tray_handle {
    int id = -1;
    explicit operator bool() const noexcept {
      return id >= 0;
    }
  };
  tray_handle tray_create(std::string_view icon_path, std::string_view tooltip);
  void tray_set_menu(tray_handle, const std::vector<menu_item>&);
  void tray_destroy(tray_handle);

  // Tray extensions.
  bool tray_set_image(tray_handle, std::string_view icon_path);
  bool tray_set_title(tray_handle, std::string_view title);
  bool tray_set_tooltip(tray_handle, std::string_view tip);

  enum class tray_event_kind { click, right_click, double_click };
  // Register a per-tray event listener. Returns a token usable for unsubscribe.
  // Multiple listeners per (tray, kind) are allowed; they fire in registration order.
  // Returns -1 on invalid handle.
  int tray_on(tray_handle, tray_event_kind, std::function<void()> cb);
  void tray_off(tray_handle, int token);

  // -------- Global shortcuts ----------------------------------------------
  bool global_shortcut_register(std::string_view accelerator, std::function<void()> cb);
  void global_shortcut_unregister(std::string_view accelerator);
  void global_shortcut_unregister_all();

  // -------- Main-thread dispatch ------------------------------------------
  // Some platform callbacks (notification clicks, hotkeys, menu selections,
  // tray clicks) arrive on the platform main loop or auxiliary threads. They
  // are queued via post_main_thread_dispatch() and drained by the host's
  // app_run_loop on each iteration through pump_main_thread_dispatches().
  void post_main_thread_dispatch(std::function<void()> fn);
  void pump_main_thread_dispatches();

  // -------- Clipboard formats ----------------------------------------------
  bool clipboard_set_html(std::string_view utf8);
  std::optional<std::string> clipboard_get_html();
  bool clipboard_set_rtf(std::string_view rtf);
  std::optional<std::string> clipboard_get_rtf();
  bool clipboard_set_mime(std::string_view mime, const std::vector<u8>& bytes);
  std::optional<std::vector<u8>> clipboard_get_mime(std::string_view mime);

} // namespace fxe::os
