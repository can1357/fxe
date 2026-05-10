// JS bindings for fxe::window. Type tag 'WIND'.
//
// Constructor allocates a unique_ptr<window> via fxe::create_window() and
// stores the raw pointer in internal field 0. The unique_ptr is held inside a
// holder struct that the GC finaliser deletes.

#include "../runtime/capabilities.hpp"
#include "../runtime/v8/fxe_native.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fxe/js_bindings.hpp>
#include <fxe/log.hpp>
#include <fxe/renderer.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/v8_literals.hpp>
#include <fxe/window.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <v8.h>

#include "../os/os.hpp"
#include "bind_fetch.hpp"
#include "bind_timers.hpp"
#include "bind_websocket.hpp"
#include "isolate_coordinator.hpp"
#include "runtime/uv_loop.hpp"
#include "weak_holder.hpp"
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_template_cache.hpp>

namespace fxe::js {
  namespace {
    using namespace v8;
    struct win_tag {};
    using win_tpl_cache = template_isolate_cache<win_tag>;

    struct listener_entry {
      u64 token;
      Global<Function> fn;
    };

    struct monitors_listener_state {
      Isolate* isolate = nullptr;
      Global<Context> context;
      std::vector<Global<Function>> change_listeners;
      bool installed = false;
    };

    monitors_listener_state& monitors_state() {
      static monitors_listener_state state;
      return state;
    }

    void reset_monitors_state() {
      auto& state = monitors_state();
      for (auto& listener : state.change_listeners)
        listener.Reset();
      state.change_listeners.clear();
      state.context.Reset();
      state.isolate = nullptr;
      if (state.installed) {
        uninstall_monitor_change_observer();
        state.installed = false;
      }
    }

    void monitors_reset_for_isolate(Isolate* iso) {
      auto& state = monitors_state();
      if (state.isolate != iso)
        return;
      reset_monitors_state();
    }

    struct monitors_resetter_register {
      monitors_resetter_register() {
        register_template_resetter(&monitors_reset_for_isolate);
      }
    };
    static monitors_resetter_register s_monitors_resetter_register;

    bool monitors_has_change_listeners() {
      return !monitors_state().change_listeners.empty();
    }

    void maybe_uninstall_monitors_observer() {
      auto& state = monitors_state();
      if (!state.installed || monitors_has_change_listeners())
        return;
      uninstall_monitor_change_observer();
      state.installed = false;
    }

    void dispatch_monitor_change_listeners() {
      auto& state = monitors_state();
      auto* iso = state.isolate;
      if (!iso || state.context.IsEmpty() || state.change_listeners.empty())
        return;
      Isolate::Scope isolate_scope(iso);
      HandleScope handle_scope(iso);
      auto ctx = state.context.Get(iso);
      if (ctx.IsEmpty())
        return;
      Context::Scope context_scope(ctx);
      std::vector<Local<Function>> listeners;
      listeners.reserve(state.change_listeners.size());
      for (auto& entry : state.change_listeners) {
        auto fn = entry.Get(iso);
        if (!fn.IsEmpty())
          listeners.push_back(fn);
      }
      for (auto fn : listeners) {
        TryCatch try_catch(iso);
        Local<Value> ignored;
        (void)fn->Call(ctx, ctx->Global(), 0, nullptr).ToLocal(&ignored);
      }
    }

    void ensure_monitors_observer_installed() {
      auto& state = monitors_state();
      if (state.installed)
        return;
      install_monitor_change_observer([] { dispatch_monitor_change_listeners(); });
      state.installed = true;
    }

    bool remove_monitors_listener(Isolate* iso, Local<Function> fn) {
      auto& listeners = monitors_state().change_listeners;
      for (auto it = listeners.begin(); it != listeners.end(); ++it) {
        auto stored = it->Get(iso);
        if (!stored.IsEmpty() && stored->StrictEquals(fn)) {
          it->Reset();
          listeners.erase(it);
          maybe_uninstall_monitors_observer();
          return true;
        }
      }
      maybe_uninstall_monitors_observer();
      return false;
    }

    struct win_holder : weak_holder<win_holder> {
      std::unique_ptr<window> owned;
      std::string title; // backing storage for window_desc::title (string_view).
      std::unordered_map<std::string, std::vector<listener_entry>> listeners;
      u64 next_token = 1;
      Global<Function> on_frame;
      Global<Object> self_strong; // strong ref held by app_run_loop while running
      uint64_t isolate_runtime_id = 0;
      int capability_id = 0;
      bool capabilities_registered = false;

      void on_finalize(Isolate* iso);
    };

    // Map raw window pointer -> holder. Lets free-function helpers (App.run,
    // disposers) recover the listener table from a window* obtained via
    // get_active_window_for_isolate or unwrap.
    std::unordered_map<window*, win_holder*>& holder_map() {
      static thread_local std::unordered_map<window*, win_holder*> m;
      return m;
    }

    win_holder* lookup_holder(window* w) {
      if (!w)
        return nullptr;
      auto& m = holder_map();
      auto it = m.find(w);
      return it == m.end() ? nullptr : it->second;
    }

    void win_holder::on_finalize(Isolate* iso) {
      auto& m = holder_map();
      if (owned) {
        unregister_window_for_isolate(iso, owned.get());
        m.erase(owned.get());
      }
      if (capabilities_registered)
        fxe::runtime::unregister_window_capabilities(capability_id);
      if (isolate_runtime_id != 0)
        (void)isolate_coordinator::get().stop_runtime(isolate_runtime_id);
      for (auto& [_, v] : listeners) {
        for (auto& entry : v)
          entry.fn.Reset();
      }
      listeners.clear();
      on_frame.Reset();
      self_strong.Reset();
    }

    window* unwrap_win(Local<Object> self) {
      return static_cast<window*>(unwrap(self, TAG_WINDOW));
    }

    uint64_t window_runtime_id(Isolate* iso, Local<Object> self) {
      if (self.IsEmpty() || self->InternalFieldCount() < 3)
        return 0;
      auto ctx = iso->GetCurrentContext();
      auto v = self->GetInternalField(2).As<Value>();
      if (v.IsEmpty() || !v->IsNumber())
        return 0;
      auto id = v->IntegerValue(ctx).FromMaybe(0);
      return id > 0 ? static_cast<uint64_t>(id) : 0;
    }

    // ---- option-bag helpers -------------------------------------------------

    int next_window_capability_id() {
      static std::atomic<int> next{1};
      return next.fetch_add(1, std::memory_order_relaxed);
    }

    bool read_string_array(Isolate* iso, Local<Context> ctx, Local<Value> value,
                           std::vector<std::string>& out, const char* name) {
      if (!value->IsArray()) {
        (void)throw_type_error(iso,
                               std::string("permissions.") + name + " must be boolean or string[]");
        return false;
      }
      auto array = value.As<Array>();
      const u32 length = array->Length();
      out.clear();
      out.reserve(length);
      for (u32 i = 0; i < length; ++i) {
        auto item = get_index<Local<Value>>(ctx, array, i);
        if (!item || !(*item)->IsString()) {
          (void)throw_type_error(iso,
                                 std::string("permissions.") + name + " entries must be strings");
          return false;
        }
        out.push_back(to_std_string(iso, *item));
      }
      return true;
    }

    template <typename AllowList>
    bool parse_string_allowlist(Isolate* iso, Local<Context> ctx, Local<Object> perms,
                                Local<String> key, const char* name, AllowList& out) {
      auto value = get_prop<Local<Value>>(ctx, perms, key);
      if (!value || (*value)->IsUndefined())
        return true;
      if ((*value)->IsBoolean()) {
        if ((*value)->BooleanValue(iso))
          out = std::nullopt;
        else
          out = std::vector<std::string>{};
        return true;
      }
      std::vector<std::string> entries;
      if (!read_string_array(iso, ctx, *value, entries, name))
        return false;
      out = std::move(entries);
      return true;
    }

    template <typename BoolAllow>
    bool parse_boolean_allow(Isolate* iso, Local<Context> ctx, Local<Object> perms,
                             Local<String> key, const char* name, BoolAllow& out) {
      auto value = get_prop<Local<Value>>(ctx, perms, key);
      if (!value || (*value)->IsUndefined())
        return true;
      if (!(*value)->IsBoolean()) {
        (void)throw_type_error(iso, std::string("permissions.") + name + " must be boolean");
        return false;
      }
      if ((*value)->BooleanValue(iso))
        out = std::nullopt;
      else
        out = false;
      return true;
    }

    bool is_webauthn_attestation_value(std::string_view value) {
      return value == "none" || value == "indirect" || value == "direct";
    }

    bool is_webauthn_user_verification_value(std::string_view value) {
      return value == "discouraged" || value == "preferred" || value == "required";
    }

    bool
    parse_webauthn_permissions(Isolate* iso, Local<Context> ctx, Local<Object> perms,
                               std::optional<fxe::runtime::capability_set::webauthn_policy>& out) {
      auto value = get_prop<Local<Value>>(ctx, perms, "webauthn"_v8(iso));
      if (!value || (*value)->IsUndefined())
        return true;
      if ((*value)->IsBoolean()) {
        if ((*value)->BooleanValue(iso)) {
          // Phase 1 dev sugar: empty rp_ids + allow_virtual_authenticator means
          // allow any RP ID through the virtual authenticator.
          fxe::runtime::capability_set::webauthn_policy policy;
          policy.allow_virtual_authenticator = true;
          out = std::move(policy);
        } else {
          out = std::nullopt;
        }
        return true;
      }
      if (!(*value)->IsObject()) {
        (void)throw_type_error(iso, "permissions.webauthn must be boolean or an object");
        return false;
      }

      auto obj = (*value).As<Object>();
      fxe::runtime::capability_set::webauthn_policy policy;
      auto rp_ids = get_prop<Local<Value>>(ctx, obj, "rpIds"_v8(iso));
      if (!rp_ids || !(*rp_ids)->IsArray()) {
        (void)throw_type_error(iso, "permissions.webauthn.rpIds must be a string[]");
        return false;
      }
      if (!read_string_array(iso, ctx, *rp_ids, policy.rp_ids, "webauthn.rpIds"))
        return false;

      if (auto field = get_prop<Local<Value>>(ctx, obj, "attestation"_v8(iso));
          field && !(*field)->IsUndefined()) {
        if (!(*field)->IsString()) {
          (void)throw_type_error(iso, "permissions.webauthn.attestation must be a string");
          return false;
        }
        policy.attestation = to_std_string(iso, *field);
        if (!is_webauthn_attestation_value(policy.attestation)) {
          (void)throw_type_error(
              iso, "permissions.webauthn.attestation must be 'none', 'indirect', or 'direct'");
          return false;
        }
      }
      if (auto field = get_prop<Local<Value>>(ctx, obj, "userVerification"_v8(iso));
          field && !(*field)->IsUndefined()) {
        if (!(*field)->IsString()) {
          (void)throw_type_error(iso, "permissions.webauthn.userVerification must be a string");
          return false;
        }
        policy.user_verification = to_std_string(iso, *field);
        if (!is_webauthn_user_verification_value(policy.user_verification)) {
          (void)throw_type_error(iso, "permissions.webauthn.userVerification must be "
                                      "'discouraged', 'preferred', or 'required'");
          return false;
        }
      }
      if (auto field = get_prop<Local<Value>>(ctx, obj, "transports"_v8(iso));
          field && !(*field)->IsUndefined()) {
        if (!read_string_array(iso, ctx, *field, policy.transports, "webauthn.transports"))
          return false;
      }
      if (auto field = get_prop<Local<Value>>(ctx, obj, "allowVirtualAuthenticator"_v8(iso));
          field && !(*field)->IsUndefined()) {
        if (!(*field)->IsBoolean()) {
          (void)throw_type_error(iso,
                                 "permissions.webauthn.allowVirtualAuthenticator must be boolean");
          return false;
        }
        policy.allow_virtual_authenticator = (*field)->BooleanValue(iso);
      }
      if (policy.rp_ids.empty() && !policy.allow_virtual_authenticator) {
        (void)throw_type_error(iso, "permissions.webauthn.rpIds must be non-empty unless "
                                    "allowVirtualAuthenticator is true");
        return false;
      }
      out = std::move(policy);
      return true;
    }

    bool parse_window_permissions(Isolate* iso, Local<Context> ctx, Local<Value> value,
                                  fxe::runtime::capability_set& out) {
      if (value.IsEmpty() || value->IsUndefined() || value->IsNull())
        return true;
      if (!value->IsObject()) {
        (void)throw_type_error(iso, "permissions must be an object");
        return false;
      }
      auto perms = value.As<Object>();
      return parse_string_allowlist(iso, ctx, perms, "fs"_v8(iso), "fs", out.fs_allow) &&
             parse_string_allowlist(iso, ctx, perms, "net"_v8(iso), "net", out.net_allow) &&
             parse_boolean_allow(iso, ctx, perms, "shell"_v8(iso), "shell", out.shell_allow) &&
             parse_boolean_allow(iso, ctx, perms, "native"_v8(iso), "native", out.native_allow) &&
             parse_webauthn_permissions(iso, ctx, perms, out.webauthn_allow);
    }

    std::string current_script_origin(Isolate* iso) {
      auto stack = StackTrace::CurrentStackTrace(iso, 8, StackTrace::kScriptNameOrSourceURL);
      for (int i = 0; i < stack->GetFrameCount(); ++i) {
        auto frame = stack->GetFrame(iso, static_cast<u32>(i));
        Local<String> name = frame->GetScriptNameOrSourceURL();
        if (name.IsEmpty())
          continue;
        auto s = to_std_string(iso, name);
        if (!s.empty() && !s.starts_with("<"))
          return s;
      }
      return {};
    }

    std::filesystem::path resolve_preload_path(Isolate* iso, std::string_view preload,
                                               std::string& error) {
      namespace fs = std::filesystem;
      fs::path requested{std::string(preload)};
      std::error_code ec;
      if (requested.is_absolute())
        return fs::weakly_canonical(requested, ec).lexically_normal();

      auto origin = current_script_origin(iso);
      if (origin.empty()) {
        error = "Window preload relative path has no file script origin";
        return {};
      }
      fs::path origin_path{origin};
      if (!origin_path.is_absolute())
        origin_path = fs::absolute(origin_path, ec);
      auto resolved = origin_path.parent_path() / requested;
      auto canonical = fs::weakly_canonical(resolved, ec);
      if (!ec)
        return canonical.lexically_normal();
      return resolved.lexically_normal();
    }

    bool preload_is_module(const std::filesystem::path& path) {
      auto ext = path.extension().string();
      return ext == ".mjs" || ext == ".ts" || ext == ".mts" || ext == ".cts";
    }

    bool run_window_preload(Isolate* iso, const std::string& preload) {
      if (preload.empty())
        return true;
      std::string error;
      auto path = resolve_preload_path(iso, preload, error);
      if (!error.empty()) {
        (void)throw_error(iso, error);
        return false;
      }
      auto* h = host_for_isolate(iso);
      if (!h) {
        (void)throw_error(iso, "Window preload has no V8 host");
        return false;
      }
      auto result = h->run_preload_file(path, preload_is_module(path));
      if (!result.ok) {
        (void)throw_error(iso, "Window preload failed: " + result.message);
        return false;
      }
      return true;
    }

    bool serialize_window_value(Isolate* iso, Local<Context> ctx, Local<Value> value,
                                std::vector<u8>& out, std::string& error) {
      ValueSerializer serializer(iso);
      serializer.WriteHeader();
      if (!serializer.WriteValue(ctx, value).FromMaybe(false)) {
        error = "Window.send value could not be structured-cloned";
        return false;
      }
      auto released = serializer.Release();
      out.assign(released.first, released.first + released.second);
      std::free(released.first);
      return true;
    }

    MaybeLocal<Value> deserialize_window_value(Isolate* iso, Local<Context> ctx,
                                               const std::vector<u8>& bytes) {
      ValueDeserializer deserializer(iso, bytes.data(), bytes.size());
      if (!deserializer.ReadHeader(ctx).FromMaybe(false))
        return MaybeLocal<Value>();
      return deserializer.ReadValue(ctx);
    }

    // ---- cursor table -------------------------------------------------------

    bool parse_cursor(std::string_view name, cursor_kind& out) {
      struct row {
        std::string_view name;
        cursor_kind kind;
      };
      static constexpr row table[] = {
          {"arrow", cursor_kind::arrow},
          {"ibeam", cursor_kind::ibeam},
          {"crosshair", cursor_kind::crosshair},
          {"hand", cursor_kind::hand},
          {"hresize", cursor_kind::hresize},
          {"vresize", cursor_kind::vresize},
          {"allResize", cursor_kind::all_resize},
          {"all_resize", cursor_kind::all_resize},
          {"neswResize", cursor_kind::nesw_resize},
          {"nesw_resize", cursor_kind::nesw_resize},
          {"nwseResize", cursor_kind::nwse_resize},
          {"nwse_resize", cursor_kind::nwse_resize},
          {"notAllowed", cursor_kind::not_allowed},
          {"not_allowed", cursor_kind::not_allowed},
          {"hidden", cursor_kind::hidden},
      };
      for (auto& r : table) {
        if (r.name == name) {
          out = r.kind;
          return true;
        }
      }
      return false;
    }

    bool parse_title_bar_style(std::string_view name, title_bar_style& out) {
      struct row {
        std::string_view name;
        title_bar_style style;
      };
      static constexpr row table[] = {
          {"default", title_bar_style::default_},
          {"hidden", title_bar_style::hidden},
          {"hiddenInset", title_bar_style::hidden_inset},
          {"customButtons", title_bar_style::custom_buttons},
      };
      for (auto& r : table) {
        if (r.name == name) {
          out = r.style;
          return true;
        }
      }
      return false;
    }

    bool parse_vibrancy_kind(std::string_view name) {
      static constexpr std::string_view names[] = {"sidebar", "titlebar", "menu"};
      for (auto n : names)
        if (n == name)
          return true;
      return false;
    }

    // ---- event-name table ---------------------------------------------------

    const char* event_name_for_kind(input_event::kind_t k) {
      using K = input_event::kind_t;
      switch (k) {
      case K::mouse_move:
        return "mousemove";
      case K::mouse_button_down:
        return "mousedown";
      case K::mouse_button_up:
        return "mouseup";
      case K::mouse_wheel:
        return "wheel";
      case K::gesture_pinch_begin:
      case K::gesture_pinch_change:
      case K::gesture_pinch_end:
      case K::gesture_rotate_begin:
      case K::gesture_rotate_change:
      case K::gesture_rotate_end:
      case K::gesture_swipe:
        return "gesture";
      case K::key_down:
        return "keydown";
      case K::key_up:
        return "keyup";
      case K::key_char:
        return "keypress";
      case K::cursor_enter:
        return "cursorenter";
      case K::cursor_leave:
        return "cursorleave";
      case K::window_resize:
        return "resize";
      case K::window_move:
        return "move";
      case K::window_focus:
        return "focus";
      case K::window_blur:
        return "blur";
      case K::window_iconify:
        return "minimize";
      case K::window_restore:
        return "restore";
      case K::window_maximize:
        return "maximize";
      case K::window_unmaximize:
        return "unmaximize";
      case K::window_close:
        return "close";
      case K::window_scale:
        return "scale";
      case K::drop_files:
        return "drop";
      case K::drag_enter:
        return "dragenter";
      case K::drag_over:
        return "dragover";
      case K::drag_leave:
        return "dragleave";
      case K::message:
        return "message";
      case K::compose:
        return "compose";
      }
      return nullptr;
    }

    bool is_known_event(std::string_view name) {
      static constexpr std::string_view names[] = {
          "keydown",   "keyup",      "keypress", "message",     "mousemove",   "mousedown",
          "mouseup",   "wheel",      "gesture",  "cursorenter", "cursorleave", "resize",
          "move",      "scale",      "focus",    "blur",        "minimize",    "restore",
          "maximize",  "unmaximize", "close",    "drop",        "dragenter",   "dragover",
          "dragleave", "compose",
      };
      for (auto n : names)
        if (n == name)
          return true;
      return false;
    }

    const char* scroll_phase_name(input_event::scroll_phase_t phase) {
      using P = input_event::scroll_phase_t;
      switch (phase) {
      case P::none:
        return "none";
      case P::began:
        return "began";
      case P::changed:
        return "changed";
      case P::ended:
        return "ended";
      case P::momentum_began:
        return "momentum_began";
      case P::momentum_changed:
        return "momentum_changed";
      case P::momentum_ended:
        return "momentum_ended";
      }
      return "none";
    }

    // ---- payload builder ----------------------------------------------------

    Local<Object> build_event_payload(Isolate* iso, Local<Context> ctx, const input_event& ev,
                                      const char* name) {
      auto o = Object::New(iso);
      auto set = [&](Local<String> k, auto&& v) {
        set_prop(ctx, o, k, std::forward<decltype(v)>(v));
      };
      set("type"_v8(iso), to_v8_string(iso, name));
      using K = input_event::kind_t;
      switch (ev.kind) {
      case K::key_down:
      case K::key_up:
        set("key"_v8(iso), to_v8(iso, ev.key));
        set("scancode"_v8(iso), to_v8(iso, ev.scancode));
        set("modifiers"_v8(iso), to_v8(iso, ev.modifiers));
        break;
      case K::key_char:
        set("key"_v8(iso), to_v8(iso, ev.key));
        set("scancode"_v8(iso), to_v8(iso, ev.scancode));
        set("modifiers"_v8(iso), to_v8(iso, ev.modifiers));
        set("codepoint"_v8(iso), Integer::NewFromUnsigned(iso, ev.codepoint));
        break;
      case K::mouse_move:
        set("x"_v8(iso), to_v8(iso, ev.x));
        set("y"_v8(iso), to_v8(iso, ev.y));
        set("dx"_v8(iso), to_v8(iso, ev.dx));
        set("dy"_v8(iso), to_v8(iso, ev.dy));
        set("modifiers"_v8(iso), to_v8(iso, ev.modifiers));
        break;
      case K::mouse_button_down:
      case K::mouse_button_up:
        set("x"_v8(iso), to_v8(iso, ev.x));
        set("y"_v8(iso), to_v8(iso, ev.y));
        set("button"_v8(iso), to_v8(iso, ev.button));
        set("modifiers"_v8(iso), to_v8(iso, ev.modifiers));
        break;
      case K::mouse_wheel:
        set("x"_v8(iso), to_v8(iso, ev.x));
        set("y"_v8(iso), to_v8(iso, ev.y));
        set("dx"_v8(iso), to_v8(iso, ev.dx));
        set("dy"_v8(iso), to_v8(iso, ev.dy));
        set("modifiers"_v8(iso), to_v8(iso, ev.modifiers));
        set("phase"_v8(iso), to_v8_string(iso, scroll_phase_name(ev.scroll_phase)));
        set("precision"_v8(iso), to_v8(iso, ev.precision));
        break;
      case K::gesture_pinch_begin:
      case K::gesture_pinch_change:
      case K::gesture_pinch_end:
        set("type"_v8(iso), "pinch"_v8(iso));
        set("phase"_v8(iso), to_v8_string(iso, scroll_phase_name(ev.scroll_phase)));
        set("magnification"_v8(iso), to_v8(iso, static_cast<double>(ev.magnification)));
        break;
      case K::gesture_rotate_begin:
      case K::gesture_rotate_change:
      case K::gesture_rotate_end:
        set("type"_v8(iso), "rotate"_v8(iso));
        set("phase"_v8(iso), to_v8_string(iso, scroll_phase_name(ev.scroll_phase)));
        set("rotation"_v8(iso), to_v8(iso, static_cast<double>(ev.rotation_radians)));
        break;
      case K::gesture_swipe:
        set("type"_v8(iso), "swipe"_v8(iso));
        set("phase"_v8(iso), to_v8_string(iso, scroll_phase_name(ev.scroll_phase)));
        set("dx"_v8(iso), to_v8(iso, ev.swipe_dx));
        set("dy"_v8(iso), to_v8(iso, ev.swipe_dy));
        break;
      case K::window_resize:
        set("width"_v8(iso), to_v8(iso, ev.width));
        set("height"_v8(iso), to_v8(iso, ev.height));
        break;
      case K::window_move:
        set("x"_v8(iso), to_v8(iso, ev.pos_x));
        set("y"_v8(iso), to_v8(iso, ev.pos_y));
        break;
      case K::window_scale:
        set("scaleX"_v8(iso), to_v8(iso, static_cast<double>(ev.scale_x)));
        set("scaleY"_v8(iso), to_v8(iso, static_cast<double>(ev.scale_y)));
        break;
      case K::drop_files: {
        auto arr = Array::New(iso, static_cast<int>(ev.paths.size()));
        for (usize i = 0; i < ev.paths.size(); ++i)
          set_index(ctx, arr, static_cast<u32>(i), ev.paths[i]);
        set("paths"_v8(iso), arr);
        break;
      }
      case K::drag_enter:
      case K::drag_over: {
        set("x"_v8(iso), to_v8(iso, ev.x));
        set("y"_v8(iso), to_v8(iso, ev.y));
        auto arr = Array::New(iso, static_cast<int>(ev.paths.size()));
        for (usize i = 0; i < ev.paths.size(); ++i)
          set_index(ctx, arr, static_cast<u32>(i), ev.paths[i]);
        set("paths"_v8(iso), arr);
        break;
      }
      case K::drag_leave:
        break;
      case K::message: {
        set("channel"_v8(iso), to_v8_string(iso, ev.message_channel));
        auto arr = Array::New(iso, static_cast<int>(ev.message_args_serialised.size()));
        for (usize i = 0; i < ev.message_args_serialised.size(); ++i) {
          Local<Value> value;
          if (!deserialize_window_value(iso, ctx, ev.message_args_serialised[i]).ToLocal(&value)) {
            (void)throw_error(iso, "Window message payload could not be deserialized");
            return o;
          }
          set_index(ctx, arr, static_cast<u32>(i), value);
        }
        set("args"_v8(iso), arr);
        break;
      }
      case K::compose:
        set("preedit"_v8(iso), to_v8_string(iso, ev.preedit));
        set("cursor"_v8(iso), to_v8(iso, ev.cursor));
        set("committed"_v8(iso), to_v8_string(iso, ev.committed));
        break;
      default:
        break;
      }
      return o;
    }

    // Drain pending native events and dispatch them to registered listeners.
    void dispatch_pending_events(Isolate* iso, Local<Context> ctx, window* w) {
      auto* h = lookup_holder(w);
      if (!h)
        return;
      auto events = w->drain_input_events();
      if (events.empty())
        return;
      HandleScope hs(iso);
      for (auto& ev : events) {
        const char* name = event_name_for_kind(ev.kind);
        if (!name)
          continue;
        auto it = h->listeners.find(name);
        if (it == h->listeners.end() || it->second.empty())
          continue;
        // Snapshot Globals so removeAllListeners() in a handler is safe.
        std::vector<Global<Function>*> handlers;
        handlers.reserve(it->second.size());
        for (auto& e : it->second)
          handlers.push_back(&e.fn);
        auto payload = build_event_payload(iso, ctx, ev, name);
        for (auto* gfn : handlers) {
          TryCatch tc(iso);
          auto fn = gfn->Get(iso);
          if (fn.IsEmpty())
            continue;
          Local<Value> argv[1] = {payload};
          Local<Value> result;
          if (!fn->Call(ctx, ctx->Global(), 1, argv).ToLocal(&result)) {
            if (tc.HasCaught()) {
              auto exc = to_std_string(iso, tc.Exception());
              FXE_ERROR("js.window", "uncaught in window handler ({}): {}", name, exc);
              tc.Reset();
            }
          }
          iso->PerformMicrotaskCheckpoint();
        }
      }
    }

    // ---- constructor --------------------------------------------------------

    void win_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Window must be invoked with new");
        return;
      }
      window_desc desc;
      auto* h = new win_holder{};
      fxe::runtime::capability_set policy{};
      std::string preload;
      // B1 v1: `isolate: 'own'` spins a dedicated V8 runtime thread, but GLFW
      // window creation and renderer binding still stay on the shared/main path.
      std::string isolate_mode = "shared";
      if (info.Length() >= 1 && info[0]->IsObject()) {
        auto o = info[0].As<Object>();
        Local<Value> v;
        auto get = [&](Local<String> a, Local<String> b = {}) -> bool {
          if (auto primary = fxe::js::get_prop<Local<Value>>(ctx, o, a);
              primary && !(*primary)->IsUndefined()) {
            v = *primary;
            return true;
          }
          if (!b.IsEmpty()) {
            if (auto secondary = fxe::js::get_prop<Local<Value>>(ctx, o, b);
                secondary && !(*secondary)->IsUndefined()) {
              v = *secondary;
              return true;
            }
          }
          return false;
        };
        if (get("width"_v8(iso)) && v->IsNumber())
          desc.width = v->Uint32Value(ctx).FromMaybe(desc.width);
        if (get("height"_v8(iso)) && v->IsNumber())
          desc.height = v->Uint32Value(ctx).FromMaybe(desc.height);
        if (get("x"_v8(iso)) && v->IsNumber())
          desc.x = v->Int32Value(ctx).FromMaybe(desc.x);
        if (get("y"_v8(iso)) && v->IsNumber())
          desc.y = v->Int32Value(ctx).FromMaybe(desc.y);
        if (get("fullscreen"_v8(iso)) && v->IsBoolean())
          desc.fullscreen = v->BooleanValue(iso);
        if (get("visible"_v8(iso)) && v->IsBoolean())
          desc.visible = v->BooleanValue(iso);
        if (get("resizable"_v8(iso)) && v->IsBoolean())
          desc.resizable = v->BooleanValue(iso);
        if (get("decorated"_v8(iso)) && v->IsBoolean())
          desc.decorated = v->BooleanValue(iso);
        if (get("transparent"_v8(iso)) && v->IsBoolean())
          desc.transparent = v->BooleanValue(iso);
        if (get("alwaysOnTop"_v8(iso), "always_on_top"_v8(iso)) && v->IsBoolean())
          desc.always_on_top = v->BooleanValue(iso);
        if (get("maximized"_v8(iso)) && v->IsBoolean())
          desc.maximized = v->BooleanValue(iso);
        if (get("minWidth"_v8(iso), "min_width"_v8(iso)) && v->IsNumber())
          desc.min_width = v->Int32Value(ctx).FromMaybe(0);
        if (get("minHeight"_v8(iso), "min_height"_v8(iso)) && v->IsNumber())
          desc.min_height = v->Int32Value(ctx).FromMaybe(0);
        if (get("maxWidth"_v8(iso), "max_width"_v8(iso)) && v->IsNumber())
          desc.max_width = v->Int32Value(ctx).FromMaybe(0);
        if (get("maxHeight"_v8(iso), "max_height"_v8(iso)) && v->IsNumber())
          desc.max_height = v->Int32Value(ctx).FromMaybe(0);
        if (get("title"_v8(iso)) && v->IsString()) {
          h->title = to_std_string(iso, v);
          desc.title = h->title;
        }
        if (get("preload"_v8(iso)) && !v->IsNull()) {
          if (!v->IsString()) {
            delete h;
            (void)throw_type_error(iso, "WindowOptions.preload must be a string");
            return;
          }
          preload = to_std_string(iso, v);
        }
        Local<Value> iso_v;
        if (get("isolate"_v8(iso)) && !v->IsNullOrUndefined()) {
          iso_v = v;
          if (!iso_v->IsString()) {
            delete h;
            (void)throw_type_error(iso, "WindowOptions.isolate must be 'shared' or 'own'");
            return;
          }
          isolate_mode = to_std_string(iso, iso_v);
          if (isolate_mode != "shared" && isolate_mode != "own") {
            delete h;
            (void)throw_type_error(iso, "WindowOptions.isolate must be 'shared' or 'own'");
            return;
          }
        }
        if (get("permissions"_v8(iso)) && !parse_window_permissions(iso, ctx, v, policy)) {
          delete h;
          return;
        }
      }
      const auto& runner_overrides = get_runner_render_overrides();
      if (runner_overrides.override_window_visible)
        desc.visible = runner_overrides.window_visible;
      auto w = create_window(desc);
      if (!w) {
        delete h;
        (void)throw_error(iso, "create_window failed");
        return;
      }
      h->owned = std::move(w);
      if (isolate_mode == "own") {
        auto rid = isolate_coordinator::get().spawn_window_runtime();
        if (rid == 0) {
          FXE_WARN("js.window.isolate",
                   "WindowOptions.isolate='own' failed to start a dedicated runtime; falling "
                   "back to 'shared'.");
          isolate_mode = "shared";
        } else {
          h->isolate_runtime_id = rid;
          // v1 keeps the actual window on the main thread; this queued task is a
          // smoke signal that the child isolate is alive and can accept work.
          if (!isolate_coordinator::get().post_task(rid, [rid, preload] {
                if (preload.empty()) {
                  FXE_INFO("js.window.isolate",
                           "window isolate {} ready; main-thread window marshaling is still "
                           "pending.",
                           static_cast<unsigned long long>(rid));
                } else {
                  FXE_INFO("js.window.isolate",
                           "window isolate {} ready; deferred preload '{}' until cross-thread "
                           "window marshaling lands.",
                           static_cast<unsigned long long>(rid), preload);
                }
              })) {
            (void)isolate_coordinator::get().stop_runtime(rid);
            h->isolate_runtime_id = 0;
            isolate_mode = "shared";
            FXE_WARN("js.window.isolate",
                     "WindowOptions.isolate='own' could not queue startup work; falling back to "
                     "'shared'.");
          }
        }
      }
      auto self = info.This();
      set_native(iso, self, h->owned.get(), TAG_WINDOW);
      self->SetInternalField(2, to_v8(iso, static_cast<double>(h->isolate_runtime_id)));
      holder_map()[h->owned.get()] = h;
      h->capability_id = next_window_capability_id();
      fxe::runtime::register_window_capabilities(h->capability_id, policy);
      h->capabilities_registered = true;
      register_window_for_isolate(iso, h->owned.get());
      if (h->isolate_runtime_id == 0 && !run_window_preload(iso, preload)) {
        unregister_window_for_isolate(iso, h->owned.get());
        fxe::runtime::unregister_window_capabilities(h->capability_id);
        h->capabilities_registered = false;
        holder_map().erase(h->owned.get());
        delete h;
        return;
      }
      h->bind(iso, self);
    }

    void win_get_isolate_mode(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto id = window_runtime_id(iso, info.HolderV2());
      info.GetReturnValue().Set(id > 0 ? "own"_v8(iso) : "shared"_v8(iso));
    }

    void win_get_isolate_id(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto id = window_runtime_id(iso, info.HolderV2());
      info.GetReturnValue().Set(to_v8(iso, static_cast<double>(id)));
    }

    // ---- pre-existing methods ----------------------------------------------

    void win_poll(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (w)
        w->poll();
    }
    void win_close(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (w)
        w->close();
    }
    void win_should_close(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->should_close() : true);
    }
    void win_framebuffer_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto sz = w->framebuffer_size();
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, sz.x);
      set_index(ctx, arr, 1, sz.y);
      info.GetReturnValue().Set(arr);
    }
    void win_set_vsync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      bool b = info.Length() >= 1 && info[0]->BooleanValue(iso);
      w->set_vsync(b);
    }
    void win_wait_events(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (w)
        w->wait_events();
    }
    void win_wait_events_timeout(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      double s = info.Length() >= 1 ? info[0]->NumberValue(ctx).FromMaybe(0.0) : 0.0;
      w->wait_events_timeout(s);
    }
    void win_request_redraw(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (w)
        w->post_redraw();
    }
    void win_take_redraw_request(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->take_redraw_request() : false);
    }

    void win_send(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (fxe::runtime::current_worker_bootstrap() != nullptr) {
        (void)throw_error(iso, "Window.send is not yet supported across worker isolates");
        return;
      }
      auto* w = unwrap_win(info.This());
      if (!w) {
        (void)throw_error(iso, "send: no native window");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "send(channel, ...args)");
        return;
      }
      std::vector<std::vector<u8>> args;
      args.reserve(info.Length() > 1 ? static_cast<usize>(info.Length() - 1) : 0);
      for (int i = 1; i < info.Length(); ++i) {
        std::vector<u8> bytes;
        std::string error;
        if (!serialize_window_value(iso, ctx, info[i], bytes, error)) {
          (void)throw_error(iso, error);
          return;
        }
        args.push_back(std::move(bytes));
      }
      w->post_message(to_std_string(iso, info[0]), std::move(args));
    }

    // ---- new window methods -------------------------------------------------

    void win_set_title(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1)
        return;
      auto s = to_std_string(iso, info[0]);
      w->set_title(s);
    }
    void win_title(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      info.GetReturnValue().Set(to_v8_string(iso, w->get_title()));
    }
    void win_set_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2)
        return;
      int wd = info[0]->Int32Value(ctx).FromMaybe(0);
      int ht = info[1]->Int32Value(ctx).FromMaybe(0);
      w->set_size(wd, ht);
    }
    void win_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto sz = w->content_size();
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, sz.x);
      set_index(ctx, arr, 1, sz.y);
      info.GetReturnValue().Set(arr);
    }
    void win_set_position(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2)
        return;
      int x = info[0]->Int32Value(ctx).FromMaybe(0);
      int y = info[1]->Int32Value(ctx).FromMaybe(0);
      w->set_position(x, y);
    }
    void win_position(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto p = w->position();
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, p.x);
      set_index(ctx, arr, 1, p.y);
      info.GetReturnValue().Set(arr);
    }
    void win_bounds(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto bounds = w->get_bounds();
      auto obj = Object::New(iso);
      set_prop(ctx, obj, "x"_v8, bounds.x);
      set_prop(ctx, obj, "y"_v8, bounds.y);
      set_prop(ctx, obj, "width"_v8, bounds.z);
      set_prop(ctx, obj, "height"_v8, bounds.w);
      info.GetReturnValue().Set(obj);
    }
    void win_set_min_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2)
        return;
      w->set_min_size(info[0]->Int32Value(ctx).FromMaybe(0), info[1]->Int32Value(ctx).FromMaybe(0));
    }
    void win_min_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto size = w->get_min_size();
      if (!size) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, size->x);
      set_index(ctx, arr, 1, size->y);
      info.GetReturnValue().Set(arr);
    }
    void win_set_max_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2)
        return;
      w->set_max_size(info[0]->Int32Value(ctx).FromMaybe(0), info[1]->Int32Value(ctx).FromMaybe(0));
    }
    void win_max_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto size = w->get_max_size();
      if (!size) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, size->x);
      set_index(ctx, arr, 1, size->y);
      info.GetReturnValue().Set(arr);
    }
    void win_set_opacity(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1)
        return;
      w->set_opacity(static_cast<float>(info[0]->NumberValue(ctx).FromMaybe(1.0)));
    }
    // Window.setBackgroundColor(0xRRGGBBAA) | (r,g,b,a) where each is 0..1.
    // Sets the platform compositor's backdrop colour for any region of the
    // surface not yet covered by a freshly presented frame. Use to suppress
    // the brief flash during live resize where the layer bounds grow on one
    // CATransaction but the new frame lands on the next. No-op on platforms
    // without an equivalent surface property.
    void win_set_background_color(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1)
        return;
      float r = 0, g = 0, b = 0, a = 1;
      if (info.Length() >= 4) {
        r = static_cast<float>(info[0]->NumberValue(ctx).FromMaybe(0.0));
        g = static_cast<float>(info[1]->NumberValue(ctx).FromMaybe(0.0));
        b = static_cast<float>(info[2]->NumberValue(ctx).FromMaybe(0.0));
        a = static_cast<float>(info[3]->NumberValue(ctx).FromMaybe(1.0));
      } else {
        const auto packed = static_cast<uint32_t>(info[0]->IntegerValue(ctx).FromMaybe(0));
        r = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;
        g = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
        b = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
        a = static_cast<float>(packed & 0xFF) / 255.0f;
      }
      w->set_surface_background_color(r, g, b, a);
    }
    void win_opacity(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      info.GetReturnValue().Set(to_v8(iso, static_cast<double>(w->opacity())));
    }
    void win_set_always_on_top(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_always_on_top(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_is_always_on_top(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_always_on_top() : false);
    }
    void win_set_resizable(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_resizable(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_is_resizable(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_resizable() : false);
    }
    void win_set_decorated(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_decorated(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_is_decorated(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_decorated() : false);
    }
    void win_is_transparent(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_transparent() : false);
    }
    void win_set_title_bar_style(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "setTitleBarStyle: expected style string");
        return;
      }
      auto name = to_std_string(iso, info[0]);
      title_bar_style style;
      if (!parse_title_bar_style(name, style)) {
        (void)throw_type_error(iso, "setTitleBarStyle: unknown style '" + name + "'");
        return;
      }
      w->set_title_bar_style(style);
    }
    void win_set_traffic_light_position(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2)
        return;
      int x = info[0]->Int32Value(ctx).FromMaybe(0);
      int y = info[1]->Int32Value(ctx).FromMaybe(0);
      try {
        info.GetReturnValue().Set(w->set_traffic_light_position(x, y));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }
    void win_set_window_controls_overlay(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(false);
        return;
      }
      auto* iso = info.GetIsolate();
      try {
        info.GetReturnValue().Set(
            w->set_window_controls_overlay(info.Length() >= 1 && info[0]->BooleanValue(iso)));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }
    void win_set_vibrancy(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(false);
        return;
      }
      if (info.Length() < 1 || info[0]->IsNull() || info[0]->IsUndefined()) {
        try {
          info.GetReturnValue().Set(w->set_vibrancy(nullptr));
        } catch (const std::exception& err) {
          (void)throw_error(iso, err.what());
        }
        return;
      }
      if (!info[0]->IsString()) {
        (void)throw_type_error(iso, "setVibrancy: expected 'sidebar', 'titlebar', 'menu', or null");
        return;
      }
      auto name = to_std_string(iso, info[0]);
      if (!parse_vibrancy_kind(name)) {
        (void)throw_type_error(iso, "setVibrancy: unknown kind '" + name + "'");
        return;
      }
      try {
        info.GetReturnValue().Set(w->set_vibrancy(name.c_str()));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }
    void win_vibrancy_capabilities(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(Object::New(iso));
        return;
      }
      const auto caps = w->get_vibrancy_capabilities();
      auto out = Object::New(iso);
      set_prop(ctx, out, "supported"_v8, caps.supported);
      set_prop(ctx, out, "mica"_v8, caps.mica);
      set_prop(ctx, out, "acrylic"_v8, caps.acrylic);
      set_prop(ctx, out, "tabbed"_v8, caps.tabbed);
      set_prop(ctx, out, "blurBehind"_v8, caps.blur_behind);
      set_prop(ctx, out, "darkMode"_v8, caps.dark_mode);
      set_prop(ctx, out, "systemAccent"_v8, caps.system_accent);
      info.GetReturnValue().Set(out);
    }

    void win_set_blur_behind(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(false);
        return;
      }
      auto ctx = iso->GetCurrentContext();
      bool enabled = false;
      float radius = 24.0f;
      if (info.Length() >= 1 && info[0]->IsObject() && !info[0]->IsNullOrUndefined()) {
        auto opts = info[0].As<Object>();
        if (auto field = get_prop<Local<Value>>(ctx, opts, "enabled"_v8(iso));
            field && !(*field)->IsUndefined()) {
          enabled = (*field)->BooleanValue(iso);
        }
        if (auto field = get_prop<Local<Value>>(ctx, opts, "radius"_v8(iso));
            field && (*field)->IsNumber()) {
          const double v = (*field)->NumberValue(ctx).FromMaybe(24.0);
          if (std::isfinite(v) && v > 0.0)
            radius = static_cast<float>(v);
        }
      } else {
        enabled = info.Length() >= 1 && info[0]->BooleanValue(iso);
      }
#if !defined(__linux__)
      (void)radius;
#endif
      try {
#if defined(__linux__)
        if (auto* host = host_for_isolate(iso)) {
          if (auto* r = host->renderer_for(w)) {
            r->set_self_backdrop_blur(enabled, radius);
            if (enabled)
              FXE_INFO("window.blur", "linux self-backdrop-blur enabled radius={}", radius);
          }
        }
#endif
        info.GetReturnValue().Set(w->set_blur_behind(enabled));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }
    void win_set_visible(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_visible(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_set_icon(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 3) {
        (void)throw_type_error(iso, "setIcon(rgba, w, h)");
        return;
      }
      if (!info[0]->IsTypedArray()) {
        (void)throw_type_error(iso, "setIcon: rgba must be a typed array");
        return;
      }
      auto ta = info[0].As<TypedArray>();
      if (!info[0]->IsUint8Array() && !info[0]->IsUint8ClampedArray()) {
        (void)throw_type_error(iso, "setIcon: rgba must be Uint8Array or Uint8ClampedArray");
        return;
      }
      auto buf = ta->Buffer();
      auto store = buf->GetBackingStore();
      auto* base = static_cast<u8*>(store->Data()) + ta->ByteOffset();
      int wd = info[1]->Int32Value(ctx).FromMaybe(0);
      int ht = info[2]->Int32Value(ctx).FromMaybe(0);
      if (wd <= 0 || ht <= 0 ||
          ta->ByteLength() < static_cast<usize>(wd) * static_cast<usize>(ht) * 4u) {
        (void)throw_range_error(iso, "setIcon: rgba length too small for w*h*4");
        return;
      }
      try {
        info.GetReturnValue().Set(w->set_icon(base, wd, ht));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }
    void win_minimize(const FunctionCallbackInfo<Value>& info) {
      if (auto* w = unwrap_win(info.This()))
        w->minimize();
    }
    void win_maximize(const FunctionCallbackInfo<Value>& info) {
      if (auto* w = unwrap_win(info.This()))
        w->maximize();
    }
    void win_restore(const FunctionCallbackInfo<Value>& info) {
      if (auto* w = unwrap_win(info.This()))
        w->restore();
    }
    void win_focus(const FunctionCallbackInfo<Value>& info) {
      if (auto* w = unwrap_win(info.This()))
        w->focus();
    }
    void win_request_attention(const FunctionCallbackInfo<Value>& info) {
      if (auto* w = unwrap_win(info.This()))
        w->request_attention();
    }
    void win_center(const FunctionCallbackInfo<Value>& info) {
      if (auto* w = unwrap_win(info.This()))
        w->center();
    }
    void win_is_focused(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_focused() : false);
    }
    void win_is_minimized(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_minimized() : false);
    }
    void win_is_maximized(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_maximized() : false);
    }
    void win_is_visible(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_visible() : false);
    }
    void win_set_fullscreen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      const bool on = info.Length() >= 1 && info[0]->BooleanValue(iso);
      fullscreen_mode mode = fullscreen_mode::borderless;
      int monitor = -1;
      if (info.Length() >= 2 && info[1]->IsObject() && !info[1]->IsNullOrUndefined()) {
        auto options = info[1].As<Object>();
        if (auto field = get_prop<Local<Value>>(ctx, options, "mode"_v8(iso));
            field && !(*field)->IsUndefined()) {
          if (!(*field)->IsString()) {
            (void)throw_type_error(
                iso, "setFullscreen: options.mode must be 'borderless' or 'exclusive'");
            return;
          }
          auto s = (*field).As<String>();
          if (s == "exclusive"_v8) {
            mode = fullscreen_mode::exclusive;
          } else if (s == "borderless"_v8) {
            mode = fullscreen_mode::borderless;
          } else {
            (void)throw_type_error(
                iso, "setFullscreen: options.mode must be 'borderless' or 'exclusive'");
            return;
          }
        }
        if (auto field = get_prop<Local<Value>>(ctx, options, "monitorIndex"_v8(iso));
            field && !(*field)->IsUndefined()) {
          if (!(*field)->IsNumber()) {
            (void)throw_type_error(iso, "setFullscreen: options.monitorIndex must be a number");
            return;
          }
          monitor = (*field)->Int32Value(ctx).FromMaybe(-1);
        }
      } else if (info.Length() >= 2 && !info[1]->IsUndefined()) {
        if (!info[1]->IsNumber()) {
          (void)throw_type_error(iso,
                                 "setFullscreen: expected monitorIndex number or options object");
          return;
        }
        monitor = info[1]->Int32Value(ctx).FromMaybe(-1);
      }
      w->set_fullscreen(on, mode, monitor);
    }
    void win_is_fullscreen(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->is_fullscreen() : false);
    }
    void win_set_cursor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "setCursor: expected string");
        return;
      }
      auto name = to_std_string(iso, info[0]);
      cursor_kind k;
      if (!parse_cursor(name, k)) {
        (void)throw_type_error(iso, "setCursor: unknown kind '" + name + "'");
        return;
      }
      w->set_cursor(k);
    }
    void win_set_cursor_visible(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_cursor_visible(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_set_cursor_pos(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2)
        return;
      double x = info[0]->NumberValue(ctx).FromMaybe(0.0);
      double y = info[1]->NumberValue(ctx).FromMaybe(0.0);
      w->set_cursor_pos(x, y);
    }
    void win_cursor_pos(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto p = w->cursor_pos();
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, static_cast<double>(p.x));
      set_index(ctx, arr, 1, static_cast<double>(p.y));
      info.GetReturnValue().Set(arr);
    }
    void win_set_cursor_lock(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_cursor_lock(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_set_raw_mouse_motion(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_raw_mouse_motion(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_is_raw_mouse_motion_supported(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      info.GetReturnValue().Set(w->is_raw_mouse_motion_supported());
    }
    void win_set_dpi_scale_override(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      if (info.Length() < 1 || info[0]->IsNullOrUndefined()) {
        w->set_dpi_scale_override(std::nullopt);
        info.GetReturnValue().Set(true);
        return;
      }
      if (!info[0]->IsNumber()) {
        (void)throw_type_error(iso, "setDpiScaleOverride: expected number | null | undefined");
        return;
      }
      const double scale = info[0]->NumberValue(ctx).FromMaybe(0.0);
      if (!std::isfinite(scale) || scale <= 0.0) {
        info.GetReturnValue().Set(false);
        return;
      }
      w->set_dpi_scale_override(static_cast<float>(scale));
      info.GetReturnValue().Set(true);
    }
    void win_dpi_scale(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? to_v8(info.GetIsolate(), static_cast<double>(w->dpi_scale()))
                                  : to_v8(info.GetIsolate(), 1.0));
    }
    void win_has_dpi_scale_override(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      info.GetReturnValue().Set(w ? w->has_dpi_scale_override() : false);
    }
    void win_set_content_protection(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      const bool enabled = info.Length() >= 1 && info[0]->BooleanValue(iso);
      info.GetReturnValue().Set(w->set_content_protection(enabled));
    }
    void win_is_content_protection_enabled(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      info.GetReturnValue().Set(w->is_content_protection_enabled());
    }
    void win_set_cursor_image(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 5) {
        (void)throw_type_error(iso, "setCursorImage(rgba, w, h, hotX, hotY)");
        return;
      }
      if (!info[0]->IsTypedArray()) {
        (void)throw_type_error(iso, "setCursorImage: rgba must be a typed array");
        return;
      }
      auto ta = info[0].As<TypedArray>();
      if (!info[0]->IsUint8Array() && !info[0]->IsUint8ClampedArray()) {
        (void)throw_type_error(iso, "setCursorImage: rgba must be Uint8Array or Uint8ClampedArray");
        return;
      }
      auto buf = ta->Buffer();
      auto store = buf->GetBackingStore();
      auto* base = static_cast<u8*>(store->Data()) + ta->ByteOffset();
      int w_px = info[1]->Int32Value(ctx).FromMaybe(0);
      int h_px = info[2]->Int32Value(ctx).FromMaybe(0);
      int hot_x = info[3]->Int32Value(ctx).FromMaybe(0);
      int hot_y = info[4]->Int32Value(ctx).FromMaybe(0);
      if (w_px <= 0 || h_px <= 0) {
        (void)throw_range_error(iso, "setCursorImage: width and height must be positive");
        return;
      }
      usize expected = static_cast<usize>(w_px) * static_cast<usize>(h_px) * 4u;
      if (ta->ByteLength() < expected) {
        (void)throw_range_error(iso, "setCursorImage: rgba length too small for w*h*4");
        return;
      }
      if (hot_x < 0 || hot_x >= w_px || hot_y < 0 || hot_y >= h_px) {
        (void)throw_range_error(iso, "setCursorImage: hotspot out of range");
        return;
      }
      try {
        info.GetReturnValue().Set(w->set_cursor_image(base, w_px, h_px, hot_x, hot_y));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }
    void win_clear_cursor_image(const FunctionCallbackInfo<Value>& info) {
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->clear_cursor_image();
    }
    void win_clipboard_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto s = w->clipboard_text();
      info.GetReturnValue().Set(to_v8_string(iso, s));
    }
    void win_set_clipboard_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1)
        return;
      w->set_clipboard_text(to_std_string(iso, info[0]));
    }

    void win_read_clipboard_image(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }

      clipboard_image image;
      if (!w->read_clipboard_image(image)) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }

      auto store = ArrayBuffer::NewBackingStore(iso, image.data.size());
      if (!image.data.empty())
        std::memcpy(store->Data(), image.data.data(), image.data.size());
      auto ab = ArrayBuffer::New(iso, std::move(store));
      auto out = Object::New(iso);
      set_prop(ctx, out, "width"_v8, image.width);
      set_prop(ctx, out, "height"_v8, image.height);
      set_prop(ctx, out, "data"_v8, Uint8Array::New(ab, 0, image.data.size()));
      info.GetReturnValue().Set(out);
    }

    void win_write_clipboard_image(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(false);
        return;
      }
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "writeClipboardImage({ width, height, data })");
        return;
      }

      auto object = info[0].As<Object>();
      auto width_value = get_prop<Local<Value>>(ctx, object, "width"_v8(iso));
      auto height_value = get_prop<Local<Value>>(ctx, object, "height"_v8(iso));
      auto data_value = get_prop<Local<Value>>(ctx, object, "data"_v8(iso));
      if (!width_value || !height_value || !data_value || !(*data_value)->IsTypedArray() ||
          (!(*data_value)->IsUint8Array() && !(*data_value)->IsUint8ClampedArray())) {
        (void)throw_type_error(iso,
                               "writeClipboardImage: data must be Uint8Array or Uint8ClampedArray");
        return;
      }

      int width = (*width_value)->Int32Value(ctx).FromMaybe(0);
      int height = (*height_value)->Int32Value(ctx).FromMaybe(0);
      auto data = (*data_value).As<TypedArray>();
      if (width <= 0 || height <= 0 ||
          static_cast<usize>(width) >
              std::numeric_limits<usize>::max() / static_cast<usize>(height) / 4u ||
          data->ByteLength() < static_cast<usize>(width) * static_cast<usize>(height) * 4u) {
        (void)throw_range_error(iso,
                                "writeClipboardImage: data length too small for width*height*4");
        return;
      }

      clipboard_image image;
      image.width = static_cast<u32>(width);
      image.height = static_cast<u32>(height);
      image.data.resize(static_cast<usize>(width) * static_cast<usize>(height) * 4u);
      data->CopyContents(image.data.data(), image.data.size());
      info.GetReturnValue().Set(w->set_clipboard_image(image));
    }

    void win_clipboard_html(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto value = w->clipboard_html();
      if (!value) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, *value));
    }

    void win_set_clipboard_html(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(w->set_clipboard_html(to_std_string(iso, info[0])));
    }

    void win_clipboard_rtf(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto value = w->clipboard_rtf();
      if (!value) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, *value));
    }

    void win_set_clipboard_rtf(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(w->set_clipboard_rtf(to_std_string(iso, info[0])));
    }

    void win_clipboard_mime(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto bytes = w->clipboard_mime(to_std_string(iso, info[0]));
      if (!bytes) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto store = ArrayBuffer::NewBackingStore(iso, bytes->size());
      if (!bytes->empty())
        std::memcpy(store->Data(), bytes->data(), bytes->size());
      auto ab = ArrayBuffer::New(iso, std::move(store));
      info.GetReturnValue().Set(Uint8Array::New(ab, 0, bytes->size()));
    }

    void win_set_clipboard_mime(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 2) {
        info.GetReturnValue().Set(false);
        return;
      }
      if (!info[1]->IsTypedArray() ||
          (!info[1]->IsUint8Array() && !info[1]->IsUint8ClampedArray())) {
        (void)throw_type_error(
            iso, "setClipboardMime(mime, bytes): bytes must be Uint8Array or Uint8ClampedArray");
        return;
      }
      auto data = info[1].As<TypedArray>();
      std::vector<u8> bytes(data->ByteLength());
      if (!bytes.empty())
        data->CopyContents(bytes.data(), bytes.size());
      info.GetReturnValue().Set(w->set_clipboard_mime(to_std_string(iso, info[0]), bytes));
    }

    void win_set_drag_region(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      std::vector<math::ivec4> rects;
      if (info.Length() >= 1 && info[0]->IsArray()) {
        auto arr = info[0].As<Array>();
        u32 n = arr->Length();
        rects.reserve(n);
        for (u32 i = 0; i < n; ++i) {
          auto el_value = get_index<Local<Value>>(ctx, arr, i);
          if (!el_value)
            continue;
          auto el = *el_value;
          int x = 0, y = 0, ww = 0, hh = 0;
          if (el->IsArray()) {
            auto a = el.As<Array>();
            auto get_i = [&](u32 idx) -> int {
              auto value = get_index<Local<Value>>(ctx, a, idx);
              if (!value)
                return 0;
              return (*value)->Int32Value(ctx).FromMaybe(0);
            };
            x = get_i(0);
            y = get_i(1);
            ww = get_i(2);
            hh = get_i(3);
          } else if (el->IsObject()) {
            auto o = el.As<Object>();
            auto get_i = [&](Local<String> k) -> int {
              auto value = get_prop<Local<Value>>(ctx, o, k);
              if (!value)
                return 0;
              return (*value)->Int32Value(ctx).FromMaybe(0);
            };
            x = get_i("x"_v8(iso));
            y = get_i("y"_v8(iso));
            ww = get_i("width"_v8(iso));
            hh = get_i("height"_v8(iso));
          } else {
            continue;
          }
          rects.emplace_back(x, y, ww, hh);
        }
      }
      w->set_drag_region(rects);
    }

    void win_start_drag(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(false);
        return;
      }
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "startDrag({ files, text?, html?, image?, icon? })");
        return;
      }

      auto object = info[0].As<Object>();
      drag_payload payload;

      if (auto files_value = get_prop<Local<Value>>(ctx, object, "files"_v8(iso));
          files_value && (*files_value)->IsArray()) {
        auto files = (*files_value).As<Array>();
        const u32 count = files->Length();
        payload.files.reserve(count);
        for (u32 i = 0; i < count; ++i) {
          if (auto value = get_index<Local<Value>>(ctx, files, i); value && (*value)->IsString())
            payload.files.push_back(to_std_string(iso, *value));
        }
      }

      if (auto text_value = get_prop<Local<Value>>(ctx, object, "text"_v8(iso));
          text_value && (*text_value)->IsString()) {
        payload.text = to_std_string(iso, *text_value);
      }

      auto parse_drag_image = [&](Local<Value> value, image_data& out) -> bool {
        if (!value->IsObject())
          return false;
        auto object = value.As<Object>();
        auto width_value = get_prop<Local<Value>>(ctx, object, "width"_v8(iso));
        auto height_value = get_prop<Local<Value>>(ctx, object, "height"_v8(iso));
        auto data_value = get_prop<Local<Value>>(ctx, object, "data"_v8(iso));
        if (!width_value || !height_value || !data_value || !(*data_value)->IsTypedArray() ||
            (!(*data_value)->IsUint8Array() && !(*data_value)->IsUint8ClampedArray()))
          return false;
        int width = (*width_value)->Int32Value(ctx).FromMaybe(0);
        int height = (*height_value)->Int32Value(ctx).FromMaybe(0);
        auto data = (*data_value).As<TypedArray>();
        if (width <= 0 || height <= 0 ||
            static_cast<usize>(width) >
                std::numeric_limits<usize>::max() / static_cast<usize>(height) / 4u ||
            data->ByteLength() < static_cast<usize>(width) * static_cast<usize>(height) * 4u)
          return false;
        out.width = static_cast<u32>(width);
        out.height = static_cast<u32>(height);
        out.data.resize(static_cast<usize>(width) * static_cast<usize>(height) * 4u);
        data->CopyContents(out.data.data(), out.data.size());
        return true;
      };

      if (auto html_value = get_prop<Local<Value>>(ctx, object, "html"_v8(iso));
          html_value && (*html_value)->IsString()) {
        payload.html = to_std_string(iso, *html_value);
      }

      image_data icon;
      if (auto icon_value = get_prop<Local<Value>>(ctx, object, "icon"_v8(iso));
          icon_value && parse_drag_image(*icon_value, icon)) {
        payload.icon = std::move(icon);
      }

      image_data image;
      if (auto image_value = get_prop<Local<Value>>(ctx, object, "image"_v8(iso));
          image_value && parse_drag_image(*image_value, image)) {
        payload.image = std::move(image);
      }

      if (payload.files.empty() && !payload.text && !payload.html && !payload.image) {
        info.GetReturnValue().Set(false);
        return;
      }

      try {
        info.GetReturnValue().Set(w->start_drag(payload));
      } catch (const std::exception& err) {
        (void)throw_error(iso, err.what());
      }
    }

    // ---- listener registration ---------------------------------------------

    // Disposer callback installed by `on()`. Reads {window, event, token}
    // from its bound Data and removes the corresponding entry.
    void listener_disposer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto data = info.Data().As<Object>();
      auto v = get_prop<Local<Value>>(ctx, data, "win"_v8(iso));
      if (!v || !(*v)->IsExternal())
        return;
      auto* w = external_ptr<window>(*v);
      auto* h = lookup_holder(w);
      if (!h)
        return;
      auto ev_v = get_prop<Local<Value>>(ctx, data, "event"_v8(iso));
      if (!ev_v)
        return;
      auto event = to_std_string(iso, *ev_v);
      auto tok_v = get_prop<Local<Value>>(ctx, data, "token"_v8(iso));
      if (!tok_v)
        return;
      double tok_d = (*tok_v)->NumberValue(ctx).FromMaybe(0.0);
      u64 token = static_cast<u64>(tok_d);
      auto it = h->listeners.find(event);
      if (it == h->listeners.end())
        return;
      auto& vec = it->second;
      for (auto vi = vec.begin(); vi != vec.end(); ++vi) {
        if (vi->token == token) {
          vi->fn.Reset();
          vec.erase(vi);
          break;
        }
      }
    }

    void win_on(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      auto* h = lookup_holder(w);
      if (!h) {
        (void)throw_error(iso, "on: no native window");
        return;
      }
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction()) {
        (void)throw_type_error(iso, "on(event, handler)");
        return;
      }
      auto event = to_std_string(iso, info[0]);
      if (!is_known_event(event)) {
        (void)throw_type_error(iso, "on: unknown event '" + event + "'");
        return;
      }
      auto fn = info[1].As<Function>();
      u64 token = h->next_token++;
      h->listeners[event].push_back({token, Global<Function>(iso, fn)});

      // Build disposer.
      auto data = Object::New(iso);
      set_prop(ctx, data, "win"_v8, make_external(iso, w));
      set_prop(ctx, data, "event"_v8, event);
      set_prop(ctx, data, "token"_v8, static_cast<double>(token));
      auto disp = Function::New(ctx, listener_disposer, data).ToLocalChecked();
      info.GetReturnValue().Set(disp);
    }

    void win_off(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      auto* h = lookup_holder(w);
      if (!h)
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "off(event[, handler])");
        return;
      }
      auto event = to_std_string(iso, info[0]);
      auto it = h->listeners.find(event);
      if (it == h->listeners.end())
        return;
      if (info.Length() < 2 || !info[1]->IsFunction()) {
        for (auto& e : it->second)
          e.fn.Reset();
        it->second.clear();
        return;
      }
      auto fn = info[1].As<Function>();
      auto& vec = it->second;
      for (auto vi = vec.begin(); vi != vec.end();) {
        auto stored = vi->fn.Get(iso);
        if (!stored.IsEmpty() && stored->StrictEquals(fn)) {
          vi->fn.Reset();
          vi = vec.erase(vi);
          break;
        } else {
          ++vi;
        }
      }
    }

    void win_remove_all_listeners(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      auto* h = lookup_holder(w);
      if (!h)
        return;
      if (info.Length() >= 1 && info[0]->IsString()) {
        auto event = to_std_string(iso, info[0]);
        auto it = h->listeners.find(event);
        if (it != h->listeners.end()) {
          for (auto& e : it->second)
            e.fn.Reset();
          it->second.clear();
        }
        return;
      }
      for (auto& [_, v] : h->listeners) {
        for (auto& e : v)
          e.fn.Reset();
      }
      h->listeners.clear();
    }

    // ---- run loop -----------------------------------------------------------

    struct run_opts {
      double frame_period = 0.0;
      // True when the caller wants a redraw every loop iteration (no lazy
      // gating on input/redraw requests). With this set, the loop polls
      // events instead of calling `wait_events_timeout`, which on macOS
      // periodically stalls inside `mach_msg` for 25-55 ms when asked to
      // wait 1 ms — the dominant source of frame-time outliers.
      bool continuous = false;
    };

    bool parse_run_opts(Isolate* iso, Local<Context> ctx, Local<Value> v, run_opts& out) {
      out = {};
      const auto& runner_overrides = get_runner_render_overrides();
      // Continuous mode polls only when uncapped. If the runner also supplied
      // an explicit fps limit, app_run_loop keeps that cadence while still
      // avoiding lazy redraw gating.
      if (runner_overrides.override_fps) {
        out.frame_period = runner_overrides.fps > 0.0 ? 1.0 / runner_overrides.fps : 0.0;
        out.continuous = runner_overrides.force_continuous;
        return true;
      }
      if (runner_overrides.force_continuous) {
        out.continuous = true;
        out.frame_period = 0.0;
        return true;
      }
      if (!v->IsObject())
        return true;
      auto opts = v.As<Object>();
      double fps = 0.0;
      if (auto x = get_prop<Local<Value>>(ctx, opts, "fps"_v8(iso)); x && (*x)->IsNumber()) {
        fps = (*x)->NumberValue(ctx).FromMaybe(0.0);
      }
      if (fps <= 0.0) {
        if (auto x = get_prop<Local<Value>>(ctx, opts, "animate"_v8(iso));
            x && (*x)->BooleanValue(iso))
          fps = 60.0;
      }
      out.frame_period = fps > 0.0 ? 1.0 / fps : 0.0;
      return true;
    }

    void win_exit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      // Static method: prefer info.This() if it's a window, else active window.
      window* w = nullptr;
      auto self = info.This();
      if (!self.IsEmpty() && self->IsObject() && self->InternalFieldCount() >= 2)
        w = unwrap_win(self);
      if (!w) {
        // Reach into the host slot to find the active window.
        // (Window.exit() called as static.)
        w = get_active_window_for_isolate(iso);
      }
      if (w)
        w->close();
    }
    void win_supports_native_gestures(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(fxe_supports_native_gestures());
    }

    // Re-entrancy guard for `drive_redraw_now`. Refresh callbacks can fire
    // while we are already inside the run loop's render pass; we must not
    // recurse.
    bool g_redraw_pump_in_progress = false;

    // Pump one full render iteration synchronously. This mirrors the body of
    // `app_run_loop`'s while(true) without the blocking `wait_events`. Used
    // as the per-window `redraw_handler` so live resize stays smooth: macOS
    // Cocoa keeps `glfwWaitEvents` parked inside its modal resize loop, but
    // it still fires our framebuffer-size + window-refresh callbacks — which
    // call this directly, so the user sees fresh frames while dragging.
    template <typename InvokeOnFrame>
    void drive_redraw_now(Isolate* iso, Local<Context> ctx, InvokeOnFrame&& invoke_on_frame) {
      if (g_redraw_pump_in_progress)
        return;
      auto* h = host_for_isolate(iso);
      if (!h)
        return;
      g_redraw_pump_in_progress = true;
      struct guard {
        bool& f;
        ~guard() {
          f = false;
        }
      } _g{g_redraw_pump_in_progress};

      auto wins = h->windows();
      for (auto* w : wins)
        dispatch_pending_events(iso, ctx, w);
      {
        TryCatch tc(iso);
        fxe::js::drain_due_timers(iso);
        if (tc.HasCaught())
          tc.Reset();
      }
      iso->PerformMicrotaskCheckpoint();
      {
        TryCatch tc(iso);
        fxe::js::drain_animation_frames(iso);
        if (tc.HasCaught())
          tc.Reset();
      }
      iso->PerformMicrotaskCheckpoint();
      fxe::js::bind_fetch::pump(iso);
      fxe::js::bind_websocket::pump(iso);
      fxe::os::pump_main_thread_dispatches();
#if FXE_HAS_LIBUV
      fxe::runtime::pump_nonblocking();
#endif
      wins = h->windows();
      for (auto* w : wins) {
        if (w->take_redraw_request())
          (void)invoke_on_frame(w);
      }
    }

    // Centralised multi-window event loop. Snapshots the host's window
    // registry every iteration so windows opened/closed during a callback are
    // picked up safely. Re-entrant calls (from a second Window.run while the
    // loop is already executing in this isolate) return immediately so each
    // run() simply registers its onFrame.
    void app_run_loop(Isolate* iso, double frame_period, bool continuous) {
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = host_for_isolate(iso);
      if (!h)
        return;
      if (h->is_app_running())
        return;
      h->set_app_running(true);

      auto invoke_on_frame = [&](window* w) -> bool {
        auto* hh = lookup_holder(w);
        if (!hh || hh->on_frame.IsEmpty())
          return true;
        HandleScope inner(iso);
        TryCatch tc(iso);
        auto cb = hh->on_frame.Get(iso);
        Local<Object> self_obj;
        if (!hh->self_strong.IsEmpty())
          self_obj = hh->self_strong.Get(iso);
        else
          self_obj = make_window_object(iso, ctx, w);
        Local<Value> argv[1] = {self_obj};
        Local<Value> result;
        if (!cb->Call(ctx, ctx->Global(), 1, argv).ToLocal(&result)) {
          if (tc.HasCaught()) {
            auto exc = to_std_string(iso, tc.Exception());
            FXE_ERROR("js.window", "uncaught in onFrame: {}", exc);
            tc.Reset();
          }
          return false;
        }
        iso->PerformMicrotaskCheckpoint();
        return true;
      };

      // Initial pause-aware drain.
      while (is_paused_for_isolate(iso))
        std::this_thread::sleep_for(std::chrono::milliseconds(20)), pump_debug_for_isolate(iso);
      pump_debug_for_isolate(iso);

      // Initial paint pass — invoke onFrame for every window whose dirty bit
      // is set (the freshly-created ones start dirty). Also wires the live-
      // resize redraw handler so refresh callbacks during a Cocoa modal
      // resize loop drive a real frame instead of dropping silently.
      {
        auto wins = h->windows();
        for (auto* w : wins) {
          if (w->should_close())
            continue;
          w->set_redraw_handler(
              [iso, ctx, &invoke_on_frame] { drive_redraw_now(iso, ctx, invoke_on_frame); });
          if (w->take_redraw_request())
            (void)invoke_on_frame(w);
        }
      }
      using run_clock = std::chrono::steady_clock;
      auto frame_period_duration = run_clock::duration::zero();
      if (frame_period > 0.0) {
        frame_period_duration =
            std::chrono::ceil<run_clock::duration>(std::chrono::duration<double>(frame_period));
      }
      const bool capped_continuous =
          continuous && frame_period_duration > run_clock::duration::zero();
      auto next_continuous_frame = run_clock::now() + frame_period_duration;

      while (true) {
        auto wins = h->windows();
        if (wins.empty())
          break;
        bool all_closed =
            std::all_of(wins.begin(), wins.end(), [](window* w) { return w->should_close(); });
        if (all_closed)
          break;

        // One global GLFW event-pump call drives every window. Uncapped
        // continuous mode polls instead of waiting: short Cocoa waits (e.g.
        // wait_events_timeout(1ms)) periodically stall in mach_msg for
        // 25-55 ms even when no events are pending. When an explicit fps
        // limit is present, keep that cadence and only force redraws when
        // the next frame deadline has arrived.
        if (continuous && !capped_continuous) {
          wins.front()->wait_events_timeout(0.0);
        } else {
          double timeout = frame_period > 0.0 ? frame_period : 0.05;
          if (capped_continuous) {
            const auto now = run_clock::now();
            timeout = now >= next_continuous_frame
                          ? 0.0
                          : std::chrono::duration<double>(next_continuous_frame - now).count();
          }
          double timer_dl = fxe::js::next_timer_deadline_seconds(iso);
          if (timer_dl < timeout)
            timeout = timer_dl;
          if (timeout < 0.0)
            timeout = 0.0;
          wins.front()->wait_events_timeout(timeout);
        }

        bool force_redraw = false;
        if (continuous) {
          if (capped_continuous) {
            const auto now = run_clock::now();
            if (now >= next_continuous_frame) {
              force_redraw = true;
              do {
                next_continuous_frame += frame_period_duration;
              } while (next_continuous_frame <= now);
            }
          } else {
            force_redraw = true;
          }
        } else if (frame_period > 0.0) {
          force_redraw = true;
        }
        if (force_redraw)
          for (auto* w : wins)
            w->post_redraw();

        // Ensure newly-created windows pick up the live-resize redraw handler
        // immediately. set_redraw_handler is idempotent, so re-binding existing
        // windows costs only a copy.
        for (auto* w : wins) {
          w->set_redraw_handler(
              [iso, ctx, &invoke_on_frame] { drive_redraw_now(iso, ctx, invoke_on_frame); });
        }
        // Re-snapshot in case windows were closed/created during the wait.
        wins = h->windows();
        for (auto* w : wins)
          dispatch_pending_events(iso, ctx, w);
        // Drain JS-side scheduled work after platform events.
        {
          TryCatch tc(iso);
          fxe::js::drain_due_timers(iso);
          if (tc.HasCaught()) {
            tc.ReThrow();
            break;
          }
        }
        iso->PerformMicrotaskCheckpoint();
        {
          TryCatch tc(iso);
          fxe::js::drain_animation_frames(iso);
          if (tc.HasCaught()) {
            tc.ReThrow();
            break;
          }
        }
        iso->PerformMicrotaskCheckpoint();
        fxe::js::bind_fetch::pump(iso);
        fxe::js::bind_websocket::pump(iso);
        fxe::os::pump_main_thread_dispatches();
#if FXE_HAS_LIBUV
        fxe::runtime::pump_nonblocking();
#endif

        // Reap closed windows so subsequent iterations see only the live set.
        {
          auto snapshot = wins;
          for (auto* w : snapshot)
            if (w->should_close())
              h->unregister_window(w);
        }
        wins = h->windows();

        for (auto* w : wins) {
          if (w->take_redraw_request())
            (void)invoke_on_frame(w);
        }
        // Slack between paint completion and the next wait_events. Hand V8
        // ~2 ms to chew on incremental GC / sweeping; this keeps the heap
        // walking steadily instead of taking a stop-the-world hit mid-frame.
        // Budget is intentionally small relative to a 16.6 ms frame so we
        // never starve real input/redraw work.
        fxe::js::idle_notification(0.002);
        pump_debug_for_isolate(iso);
        while (is_paused_for_isolate(iso)) {
          pump_debug_for_isolate(iso);
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      }

      // Detach the live-resize redraw handler before the captured lambda's
      // referenced stack frame goes out of scope.
      for (auto* w : h->windows())
        w->set_redraw_handler({});

      // Drop strong refs the JS bindings stashed for the run loop's lifetime.
      for (auto& [w, hh] : holder_map()) {
        (void)w;
        hh->self_strong.Reset();
      }
      h->set_app_running(false);
    }

    // Window.run(cb, opts): stores cb as the per-window onFrame, then drives
    // the loop (or returns immediately if a loop is already running).
    void win_run(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* w = unwrap_win(info.This());
      auto* hh = lookup_holder(w);
      if (!w || !hh)
        return;
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "run: expected callback function");
        return;
      }
      run_opts opts;
      Local<Value> opt_value = info.Length() >= 2 ? info[1] : Undefined(iso).As<Value>();
      parse_run_opts(iso, ctx, opt_value, opts);

      hh->on_frame.Reset(iso, info[0].As<Function>());
      hh->self_strong.Reset(iso, info.This());

      app_run_loop(iso, opts.frame_period, opts.continuous);
    }

    // setFrameCallback(cb): register the per-window onFrame without
    // entering app_run_loop. Use this from compositors / mount layers that
    // expect a separate App.run() / Window.run() driver. The callback is
    // invoked once per `window.requestRedraw()` ack while the OS event
    // loop is running.
    //
    // Pass `null` to clear.
    void win_set_frame_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto self = info.This();
      auto* w = unwrap_win(self);
      if (!w)
        return;
      auto* hh = lookup_holder(w);
      if (!hh)
        return;
      if (info.Length() < 1 || info[0]->IsNullOrUndefined()) {
        hh->on_frame.Reset();
        hh->self_strong.Reset();
        return;
      }
      if (!info[0]->IsFunction()) {
        (void)throw_type_error(iso, "setFrameCallback: expected function or null");
        return;
      }
      hh->on_frame.Reset(iso, info[0].As<Function>());
      hh->self_strong.Reset(iso, self);
    }

    // ---- Monitors namespace -------------------------------------------------

    Local<Object> monitor_to_js(Isolate* iso, Local<Context> ctx, const monitor_info& m) {
      auto o = Object::New(iso);
      auto set = [&](Local<String> k, auto&& v) {
        set_prop(ctx, o, k, std::forward<decltype(v)>(v));
      };
      set("name"_v8(iso), to_v8_string(iso, m.name));
      set("x"_v8(iso), to_v8(iso, m.x));
      set("y"_v8(iso), to_v8(iso, m.y));
      set("width"_v8(iso), to_v8(iso, m.width));
      set("height"_v8(iso), to_v8(iso, m.height));
      set("workX"_v8(iso), to_v8(iso, m.work_x));
      set("workY"_v8(iso), to_v8(iso, m.work_y));
      set("workWidth"_v8(iso), to_v8(iso, m.work_width));
      set("workHeight"_v8(iso), to_v8(iso, m.work_height));
      set("scaleX"_v8(iso), to_v8(iso, static_cast<double>(m.scale_x)));
      set("scaleY"_v8(iso), to_v8(iso, static_cast<double>(m.scale_y)));
      set("refreshHz"_v8(iso), to_v8(iso, m.refresh_hz));
      set("primary"_v8(iso), to_v8(iso, m.primary));
      return o;
    }

    void monitors_list(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto mons = list_monitors();
      auto arr = Array::New(iso, static_cast<int>(mons.size()));
      for (usize i = 0; i < mons.size(); ++i)
        set_index(ctx, arr, static_cast<u32>(i), monitor_to_js(iso, ctx, mons[i]));
      info.GetReturnValue().Set(arr);
    }

    void monitors_primary(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      info.GetReturnValue().Set(monitor_to_js(iso, ctx, primary_monitor()));
    }

    void monitors_listener_disposer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto data = info.Data().As<Object>();
      auto event_value = get_prop<Local<Value>>(ctx, data, "event"_v8(iso));
      if (!event_value || !(*event_value)->IsString())
        return;
      if (to_std_string(iso, *event_value) != "change")
        return;
      auto handler_value = get_prop<Local<Value>>(ctx, data, "handler"_v8(iso));
      if (!handler_value || !(*handler_value)->IsFunction())
        return;
      (void)remove_monitors_listener(iso, (*handler_value).As<Function>());
    }

    void monitors_on(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction()) {
        (void)throw_type_error(iso, "Monitors.on(event, handler)");
        return;
      }
      auto event = to_std_string(iso, info[0]);
      if (event != "change") {
        (void)throw_type_error(iso, "Monitors.on: unknown event '" + event + "'");
        return;
      }
      auto& state = monitors_state();
      state.isolate = iso;
      state.context.Reset(iso, ctx);
      auto handler = info[1].As<Function>();
      state.change_listeners.emplace_back(iso, handler);
      ensure_monitors_observer_installed();
      auto data = Object::New(iso);
      set_prop(ctx, data, "event"_v8, event);
      set_prop(ctx, data, "handler"_v8, handler);
      info.GetReturnValue().Set(
          Function::New(ctx, monitors_listener_disposer, data).ToLocalChecked());
    }

    void monitors_off(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto& state = monitors_state();
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "Monitors.off(event[, handler])");
        return;
      }
      auto event = to_std_string(iso, info[0]);
      if (event != "change") {
        (void)throw_type_error(iso, "Monitors.off: unknown event '" + event + "'");
        return;
      }
      if (info.Length() < 2 || info[1]->IsNullOrUndefined()) {
        for (auto& listener : state.change_listeners)
          listener.Reset();
        state.change_listeners.clear();
        maybe_uninstall_monitors_observer();
        return;
      }
      if (!info[1]->IsFunction()) {
        (void)throw_type_error(iso, "Monitors.off(event[, handler])");
        return;
      }
      (void)remove_monitors_listener(iso, info[1].As<Function>());
    }
    // ---- App namespace ------------------------------------------------------

    void app_run(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      run_opts opts;
      Local<Value> opt_value = info.Length() >= 1 ? info[0] : Undefined(iso).As<Value>();
      parse_run_opts(iso, ctx, opt_value, opts);
      app_run_loop(iso, opts.frame_period, opts.continuous);
    }

    void app_quit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = host_for_isolate(iso);
      if (!h)
        return;
      for (auto* w : h->windows())
        w->close();
    }

    void app_windows(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = host_for_isolate(iso);
      auto wins = h ? h->windows() : std::vector<window*>{};
      auto arr = Array::New(iso, static_cast<int>(wins.size()));
      for (usize i = 0; i < wins.size(); ++i) {
        auto* hh = lookup_holder(wins[i]);
        Local<Object> obj;
        if (hh && !hh->self_strong.IsEmpty())
          obj = hh->self_strong.Get(iso);
        else
          obj = make_window_object(iso, ctx, wins[i]);
        set_index(ctx, arr, static_cast<u32>(i), obj);
      }
      info.GetReturnValue().Set(arr);
    }
  } // namespace

  void install_window_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, win_constructor);
    tpl->SetClassName("Window"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(3);
    tpl->InstanceTemplate()->SetNativeDataProperty("isolateMode"_v8(iso), win_get_isolate_mode,
                                                   nullptr);
    tpl->InstanceTemplate()->SetNativeDataProperty("isolateId"_v8(iso), win_get_isolate_id,
                                                   nullptr);

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "poll", FunctionTemplate::New(iso, win_poll));
    proto->Set(iso, "close", FunctionTemplate::New(iso, win_close));
    proto->Set(iso, "shouldClose", FunctionTemplate::New(iso, win_should_close));
    proto->Set(iso, "framebufferSize", FunctionTemplate::New(iso, win_framebuffer_size));
    proto->Set(iso, "setVsync", FunctionTemplate::New(iso, win_set_vsync));
    proto->Set(iso, "waitEvents", FunctionTemplate::New(iso, win_wait_events));
    proto->Set(iso, "waitEventsTimeout", FunctionTemplate::New(iso, win_wait_events_timeout));
    proto->Set(iso, "requestRedraw", FunctionTemplate::New(iso, win_request_redraw));
    proto->Set(iso, "takeRedrawRequest", FunctionTemplate::New(iso, win_take_redraw_request));
    proto->Set(iso, "send", FunctionTemplate::New(iso, win_send));
    proto->Set(iso, "run", FunctionTemplate::New(iso, win_run));
    proto->Set(iso, "setFrameCallback", FunctionTemplate::New(iso, win_set_frame_callback));

    proto->Set(iso, "setTitle", FunctionTemplate::New(iso, win_set_title));
    proto->Set(iso, "title", FunctionTemplate::New(iso, win_title));
    proto->Set(iso, "getTitle", FunctionTemplate::New(iso, win_title));
    proto->Set(iso, "setSize", FunctionTemplate::New(iso, win_set_size));
    proto->Set(iso, "size", FunctionTemplate::New(iso, win_size));
    proto->Set(iso, "setPosition", FunctionTemplate::New(iso, win_set_position));
    proto->Set(iso, "position", FunctionTemplate::New(iso, win_position));
    proto->Set(iso, "bounds", FunctionTemplate::New(iso, win_bounds));
    proto->Set(iso, "getBounds", FunctionTemplate::New(iso, win_bounds));
    proto->Set(iso, "setMinSize", FunctionTemplate::New(iso, win_set_min_size));
    proto->Set(iso, "minSize", FunctionTemplate::New(iso, win_min_size));
    proto->Set(iso, "getMinSize", FunctionTemplate::New(iso, win_min_size));
    proto->Set(iso, "setMaxSize", FunctionTemplate::New(iso, win_set_max_size));
    proto->Set(iso, "maxSize", FunctionTemplate::New(iso, win_max_size));
    proto->Set(iso, "getMaxSize", FunctionTemplate::New(iso, win_max_size));
    proto->Set(iso, "setOpacity", FunctionTemplate::New(iso, win_set_opacity));
    proto->Set(iso, "setBackgroundColor", FunctionTemplate::New(iso, win_set_background_color));
    proto->Set(iso, "opacity", FunctionTemplate::New(iso, win_opacity));
    proto->Set(iso, "setAlwaysOnTop", FunctionTemplate::New(iso, win_set_always_on_top));
    proto->Set(iso, "isAlwaysOnTop", FunctionTemplate::New(iso, win_is_always_on_top));
    proto->Set(iso, "setResizable", FunctionTemplate::New(iso, win_set_resizable));
    proto->Set(iso, "isResizable", FunctionTemplate::New(iso, win_is_resizable));
    proto->Set(iso, "setDecorated", FunctionTemplate::New(iso, win_set_decorated));
    proto->Set(iso, "isDecorated", FunctionTemplate::New(iso, win_is_decorated));
    proto->Set(iso, "isTransparent", FunctionTemplate::New(iso, win_is_transparent));
    proto->Set(iso, "setTitleBarStyle", FunctionTemplate::New(iso, win_set_title_bar_style));
    proto->Set(iso, "setTrafficLightPosition",
               FunctionTemplate::New(iso, win_set_traffic_light_position));
    proto->Set(iso, "setWindowControlsOverlay",
               FunctionTemplate::New(iso, win_set_window_controls_overlay));
    proto->Set(iso, "setVibrancy", FunctionTemplate::New(iso, win_set_vibrancy));
    proto->Set(iso, "vibrancyCapabilities", FunctionTemplate::New(iso, win_vibrancy_capabilities));
    proto->Set(iso, "setBlurBehind", FunctionTemplate::New(iso, win_set_blur_behind));
    proto->Set(iso, "setVisible", FunctionTemplate::New(iso, win_set_visible));
    proto->Set(iso, "setIcon", FunctionTemplate::New(iso, win_set_icon));
    proto->Set(iso, "minimize", FunctionTemplate::New(iso, win_minimize));
    proto->Set(iso, "maximize", FunctionTemplate::New(iso, win_maximize));
    proto->Set(iso, "restore", FunctionTemplate::New(iso, win_restore));
    proto->Set(iso, "focus", FunctionTemplate::New(iso, win_focus));
    proto->Set(iso, "requestAttention", FunctionTemplate::New(iso, win_request_attention));
    proto->Set(iso, "center", FunctionTemplate::New(iso, win_center));
    proto->Set(iso, "isFocused", FunctionTemplate::New(iso, win_is_focused));
    proto->Set(iso, "isMinimized", FunctionTemplate::New(iso, win_is_minimized));
    proto->Set(iso, "isMaximized", FunctionTemplate::New(iso, win_is_maximized));
    proto->Set(iso, "isVisible", FunctionTemplate::New(iso, win_is_visible));
    proto->Set(iso, "setFullscreen", FunctionTemplate::New(iso, win_set_fullscreen));
    proto->Set(iso, "isFullscreen", FunctionTemplate::New(iso, win_is_fullscreen));
    proto->Set(iso, "setCursor", FunctionTemplate::New(iso, win_set_cursor));
    proto->Set(iso, "setCursorVisible", FunctionTemplate::New(iso, win_set_cursor_visible));
    proto->Set(iso, "setCursorPos", FunctionTemplate::New(iso, win_set_cursor_pos));
    proto->Set(iso, "cursorPos", FunctionTemplate::New(iso, win_cursor_pos));
    proto->Set(iso, "setCursorLock", FunctionTemplate::New(iso, win_set_cursor_lock));
    proto->Set(iso, "setRawMouseMotion", FunctionTemplate::New(iso, win_set_raw_mouse_motion));
    proto->Set(iso, "setDpiScaleOverride", FunctionTemplate::New(iso, win_set_dpi_scale_override));
    proto->Set(iso, "dpiScale", FunctionTemplate::New(iso, win_dpi_scale));
    proto->Set(iso, "hasDpiScaleOverride", FunctionTemplate::New(iso, win_has_dpi_scale_override));
    proto->Set(iso, "isRawMouseMotionSupported",
               FunctionTemplate::New(iso, win_is_raw_mouse_motion_supported));
    proto->Set(iso, "setContentProtection", FunctionTemplate::New(iso, win_set_content_protection));
    proto->Set(iso, "isContentProtectionEnabled",
               FunctionTemplate::New(iso, win_is_content_protection_enabled));
    proto->Set(iso, "setCursorImage", FunctionTemplate::New(iso, win_set_cursor_image));
    proto->Set(iso, "clearCursorImage", FunctionTemplate::New(iso, win_clear_cursor_image));
    proto->Set(iso, "clipboardText", FunctionTemplate::New(iso, win_clipboard_text));
    proto->Set(iso, "setClipboardText", FunctionTemplate::New(iso, win_set_clipboard_text));
    proto->Set(iso, "readClipboardImage", FunctionTemplate::New(iso, win_read_clipboard_image));
    proto->Set(iso, "writeClipboardImage", FunctionTemplate::New(iso, win_write_clipboard_image));
    proto->Set(iso, "clipboardHtml", FunctionTemplate::New(iso, win_clipboard_html));
    proto->Set(iso, "setClipboardHtml", FunctionTemplate::New(iso, win_set_clipboard_html));
    proto->Set(iso, "clipboardRtf", FunctionTemplate::New(iso, win_clipboard_rtf));
    proto->Set(iso, "setClipboardRtf", FunctionTemplate::New(iso, win_set_clipboard_rtf));
    proto->Set(iso, "clipboardMime", FunctionTemplate::New(iso, win_clipboard_mime));
    proto->Set(iso, "setClipboardMime", FunctionTemplate::New(iso, win_set_clipboard_mime));
    proto->Set(iso, "setDragRegion", FunctionTemplate::New(iso, win_set_drag_region));
    proto->Set(iso, "startDrag", FunctionTemplate::New(iso, win_start_drag));
    proto->Set(iso, "on", FunctionTemplate::New(iso, win_on));
    proto->Set(iso, "off", FunctionTemplate::New(iso, win_off));
    proto->Set(iso, "removeAllListeners", FunctionTemplate::New(iso, win_remove_all_listeners));

    // Static method: Window.exit()
    tpl->Set(iso, "exit", FunctionTemplate::New(iso, win_exit));
    tpl->Set(iso, "supportsNativeGestures",
             FunctionTemplate::New(iso, win_supports_native_gestures));

    global->Set(iso, "Window", tpl);
    win_tpl_cache::install(iso, tpl);

    // Monitors namespace.
    auto mons = ObjectTemplate::New(iso);
    mons->Set(iso, "list", FunctionTemplate::New(iso, monitors_list));
    mons->Set(iso, "primary", FunctionTemplate::New(iso, monitors_primary));
    mons->Set(iso, "on", FunctionTemplate::New(iso, monitors_on));
    mons->Set(iso, "off", FunctionTemplate::New(iso, monitors_off));
    global->Set(iso, "Monitors", mons);

    // App namespace.
    auto app = ObjectTemplate::New(iso);
    app->Set(iso, "run", FunctionTemplate::New(iso, app_run));
    app->Set(iso, "quit", FunctionTemplate::New(iso, app_quit));
    app->Set(iso, "windows", FunctionTemplate::New(iso, app_windows));
    global->Set(iso, "App", app);
  }

  Local<Object> make_window_object(Isolate* iso, Local<Context> ctx, window* w) {
    return wrap(iso, ctx, win_tpl_cache::resolve(iso), w, TAG_WINDOW);
  }
} // namespace fxe::js
