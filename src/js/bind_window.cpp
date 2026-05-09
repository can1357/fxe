// JS bindings for fxe::window. Type tag 'WIND'.
//
// Constructor allocates a unique_ptr<window> via fxe::create_window() and
// stores the raw pointer in internal field 0. The unique_ptr is held inside a
// holder struct that the GC finaliser deletes.

#include "../runtime/capabilities.hpp"
#include "../runtime/v8/fxe_native.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/v8_literals.hpp>
#include <fxe/window.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
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

namespace fxe::js {
  namespace {
    using namespace v8;
    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& win_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void throw_native_error(Isolate* iso, const std::exception& err) {
      (void)throw_error(iso, err.what());
    }

    void win_reset_for_isolate(Isolate* iso) {
      auto& t = win_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct win_resetter_register {
      win_resetter_register() {
        register_template_resetter(&win_reset_for_isolate);
      }
    };
    static win_resetter_register s_win_resetter_register;

    struct listener_entry {
      u64 token;
      Global<Function> fn;
    };

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

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string();
    }

    // ---- option-bag helpers -------------------------------------------------

    bool get_prop(Local<Context> ctx, Local<Object> o, Local<String> a, Local<Value>& out,
                  Local<String> b = {}) {
      Local<Value> v;
      if (o->Get(ctx, a).ToLocal(&v) && !v->IsUndefined()) {
        out = v;
        return true;
      }
      if (!b.IsEmpty() && o->Get(ctx, b).ToLocal(&v) && !v->IsUndefined()) {
        out = v;
        return true;
      }
      return false;
    }

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
        Local<Value> item;
        if (!array->Get(ctx, i).ToLocal(&item) || !item->IsString()) {
          (void)throw_type_error(iso,
                                 std::string("permissions.") + name + " entries must be strings");
          return false;
        }
        out.push_back(utf8(iso, item));
      }
      return true;
    }

    template <typename AllowList>
    bool parse_string_allowlist(Isolate* iso, Local<Context> ctx, Local<Object> perms,
                                Local<String> key, const char* name, AllowList& out) {
      Local<Value> value;
      if (!perms->Get(ctx, key).ToLocal(&value) || value->IsUndefined())
        return true;
      if (value->IsBoolean()) {
        if (value->BooleanValue(iso))
          out = std::nullopt;
        else
          out = std::vector<std::string>{};
        return true;
      }
      std::vector<std::string> entries;
      if (!read_string_array(iso, ctx, value, entries, name))
        return false;
      out = std::move(entries);
      return true;
    }

    template <typename BoolAllow>
    bool parse_boolean_allow(Isolate* iso, Local<Context> ctx, Local<Object> perms,
                             Local<String> key, const char* name, BoolAllow& out) {
      Local<Value> value;
      if (!perms->Get(ctx, key).ToLocal(&value) || value->IsUndefined())
        return true;
      if (!value->IsBoolean()) {
        (void)throw_type_error(iso, std::string("permissions.") + name + " must be boolean");
        return false;
      }
      if (value->BooleanValue(iso))
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
      Local<Value> value;
      if (!perms->Get(ctx, "webauthn"_v8(iso)).ToLocal(&value) || value->IsUndefined())
        return true;
      if (value->IsBoolean()) {
        if (value->BooleanValue(iso)) {
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
      if (!value->IsObject()) {
        (void)throw_type_error(iso, "permissions.webauthn must be boolean or an object");
        return false;
      }

      auto obj = value.As<Object>();
      fxe::runtime::capability_set::webauthn_policy policy;
      Local<Value> field;
      if (!obj->Get(ctx, "rpIds"_v8(iso)).ToLocal(&field) || !field->IsArray()) {
        (void)throw_type_error(iso, "permissions.webauthn.rpIds must be a string[]");
        return false;
      }
      if (!read_string_array(iso, ctx, field, policy.rp_ids, "webauthn.rpIds"))
        return false;

      if (obj->Get(ctx, "attestation"_v8(iso)).ToLocal(&field) && !field->IsUndefined()) {
        if (!field->IsString()) {
          (void)throw_type_error(iso, "permissions.webauthn.attestation must be a string");
          return false;
        }
        policy.attestation = utf8(iso, field);
        if (!is_webauthn_attestation_value(policy.attestation)) {
          (void)throw_type_error(
              iso, "permissions.webauthn.attestation must be 'none', 'indirect', or 'direct'");
          return false;
        }
      }
      if (obj->Get(ctx, "userVerification"_v8(iso)).ToLocal(&field) && !field->IsUndefined()) {
        if (!field->IsString()) {
          (void)throw_type_error(iso, "permissions.webauthn.userVerification must be a string");
          return false;
        }
        policy.user_verification = utf8(iso, field);
        if (!is_webauthn_user_verification_value(policy.user_verification)) {
          (void)throw_type_error(iso, "permissions.webauthn.userVerification must be "
                                      "'discouraged', 'preferred', or 'required'");
          return false;
        }
      }
      if (obj->Get(ctx, "transports"_v8(iso)).ToLocal(&field) && !field->IsUndefined()) {
        if (!read_string_array(iso, ctx, field, policy.transports, "webauthn.transports"))
          return false;
      }
      if (obj->Get(ctx, "allowVirtualAuthenticator"_v8(iso)).ToLocal(&field) &&
          !field->IsUndefined()) {
        if (!field->IsBoolean()) {
          (void)throw_type_error(iso,
                                 "permissions.webauthn.allowVirtualAuthenticator must be boolean");
          return false;
        }
        policy.allow_virtual_authenticator = field->BooleanValue(iso);
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
        auto s = utf8(iso, name);
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
          "keydown",   "keyup",     "keypress", "message",     "mousemove",
          "mousedown", "mouseup",   "wheel",    "cursorenter", "cursorleave",
          "resize",    "move",      "scale",    "focus",       "blur",
          "minimize",  "restore",   "maximize", "unmaximize",  "close",
          "drop",      "dragenter", "dragover", "dragleave",   "compose",
      };
      for (auto n : names)
        if (n == name)
          return true;
      return false;
    }

    // ---- payload builder ----------------------------------------------------

    Local<Object> build_event_payload(Isolate* iso, Local<Context> ctx, const input_event& ev,
                                      const char* name) {
      auto o = Object::New(iso);
      auto set = [&](Local<String> k, Local<Value> v) { (void)o->Set(ctx, k, v); };
      set("type"_v8(iso), str(iso, name));
      using K = input_event::kind_t;
      switch (ev.kind) {
      case K::key_down:
      case K::key_up:
        set("key"_v8(iso), Integer::New(iso, ev.key));
        set("scancode"_v8(iso), Integer::New(iso, ev.scancode));
        set("modifiers"_v8(iso), Integer::New(iso, ev.modifiers));
        break;
      case K::key_char:
        set("key"_v8(iso), Integer::New(iso, ev.key));
        set("scancode"_v8(iso), Integer::New(iso, ev.scancode));
        set("modifiers"_v8(iso), Integer::New(iso, ev.modifiers));
        set("codepoint"_v8(iso), Integer::NewFromUnsigned(iso, ev.codepoint));
        break;
      case K::mouse_move:
        set("x"_v8(iso), Number::New(iso, ev.x));
        set("y"_v8(iso), Number::New(iso, ev.y));
        set("dx"_v8(iso), Number::New(iso, ev.dx));
        set("dy"_v8(iso), Number::New(iso, ev.dy));
        set("modifiers"_v8(iso), Integer::New(iso, ev.modifiers));
        break;
      case K::mouse_button_down:
      case K::mouse_button_up:
        set("x"_v8(iso), Number::New(iso, ev.x));
        set("y"_v8(iso), Number::New(iso, ev.y));
        set("button"_v8(iso), Integer::New(iso, ev.button));
        set("modifiers"_v8(iso), Integer::New(iso, ev.modifiers));
        break;
      case K::mouse_wheel:
        set("x"_v8(iso), Number::New(iso, ev.x));
        set("y"_v8(iso), Number::New(iso, ev.y));
        set("dx"_v8(iso), Number::New(iso, ev.dx));
        set("dy"_v8(iso), Number::New(iso, ev.dy));
        set("modifiers"_v8(iso), Integer::New(iso, ev.modifiers));
        break;
      case K::window_resize:
        set("width"_v8(iso), Integer::New(iso, ev.width));
        set("height"_v8(iso), Integer::New(iso, ev.height));
        break;
      case K::window_move:
        set("x"_v8(iso), Integer::New(iso, ev.pos_x));
        set("y"_v8(iso), Integer::New(iso, ev.pos_y));
        break;
      case K::window_scale:
        set("scaleX"_v8(iso), Number::New(iso, static_cast<double>(ev.scale_x)));
        set("scaleY"_v8(iso), Number::New(iso, static_cast<double>(ev.scale_y)));
        break;
      case K::drop_files: {
        auto arr = Array::New(iso, static_cast<int>(ev.paths.size()));
        for (usize i = 0; i < ev.paths.size(); ++i)
          (void)arr->Set(ctx, static_cast<u32>(i), str(iso, ev.paths[i]));
        set("paths"_v8(iso), arr);
        break;
      }
      case K::drag_enter:
      case K::drag_over: {
        set("x"_v8(iso), Number::New(iso, ev.x));
        set("y"_v8(iso), Number::New(iso, ev.y));
        auto arr = Array::New(iso, static_cast<int>(ev.paths.size()));
        for (usize i = 0; i < ev.paths.size(); ++i)
          (void)arr->Set(ctx, static_cast<u32>(i), str(iso, ev.paths[i]));
        set("paths"_v8(iso), arr);
        break;
      }
      case K::drag_leave:
        break;
      case K::message: {
        set("channel"_v8(iso), str(iso, ev.message_channel));
        auto arr = Array::New(iso, static_cast<int>(ev.message_args_serialised.size()));
        for (usize i = 0; i < ev.message_args_serialised.size(); ++i) {
          Local<Value> value;
          if (!deserialize_window_value(iso, ctx, ev.message_args_serialised[i]).ToLocal(&value)) {
            (void)throw_error(iso, "Window message payload could not be deserialized");
            return o;
          }
          (void)arr->Set(ctx, static_cast<u32>(i), value);
        }
        set("args"_v8(iso), arr);
        break;
      }
      case K::compose:
        set("preedit"_v8(iso), str(iso, ev.preedit));
        set("cursor"_v8(iso), Integer::New(iso, ev.cursor));
        set("committed"_v8(iso), str(iso, ev.committed));
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
              auto exc = utf8(iso, tc.Exception());
              std::fprintf(stderr, "[fxe] uncaught in window handler (%s): %s\n", name,
                           exc.c_str());
              std::fflush(stderr);
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
          return get_prop(ctx, o, a, v, b);
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
          String::Utf8Value u(iso, v);
          if (*u)
            h->title.assign(*u, u.length());
          desc.title = h->title;
        }
        if (get("preload"_v8(iso)) && !v->IsNull()) {
          if (!v->IsString()) {
            delete h;
            (void)throw_type_error(iso, "WindowOptions.preload must be a string");
            return;
          }
          preload = utf8(iso, v);
        }
        Local<Value> iso_v;
        if (get("isolate"_v8(iso)) && !v->IsNullOrUndefined()) {
          iso_v = v;
          if (!iso_v->IsString()) {
            delete h;
            (void)throw_type_error(iso, "WindowOptions.isolate must be 'shared' or 'own'");
            return;
          }
          isolate_mode = utf8(iso, iso_v);
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
          std::fprintf(stderr, "fxe: WindowOptions.isolate='own' failed to start a dedicated "
                               "runtime; falling back to 'shared'.\n");
          isolate_mode = "shared";
        } else {
          h->isolate_runtime_id = rid;
          // v1 keeps the actual window on the main thread; this queued task is a
          // smoke signal that the child isolate is alive and can accept work.
          if (!isolate_coordinator::get().post_task(rid, [rid, preload] {
                if (preload.empty()) {
                  std::fprintf(stderr,
                               "fxe: window isolate %llu ready; main-thread window marshaling is "
                               "still pending.\n",
                               static_cast<unsigned long long>(rid));
                } else {
                  std::fprintf(stderr,
                               "fxe: window isolate %llu ready; deferred preload '%s' until "
                               "cross-thread window marshaling lands.\n",
                               static_cast<unsigned long long>(rid), preload.c_str());
                }
              })) {
            (void)isolate_coordinator::get().stop_runtime(rid);
            h->isolate_runtime_id = 0;
            isolate_mode = "shared";
            std::fprintf(stderr, "fxe: WindowOptions.isolate='own' could not queue startup work; "
                                 "falling back to 'shared'.\n");
          }
        }
      }
      auto self = info.This();
      set_native(iso, self, h->owned.get(), TAG_WINDOW);
      self->SetInternalField(2, Number::New(iso, static_cast<double>(h->isolate_runtime_id)));
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
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(id)));
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
      (void)arr->Set(ctx, 0, Integer::NewFromUnsigned(iso, sz.x));
      (void)arr->Set(ctx, 1, Integer::NewFromUnsigned(iso, sz.y));
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
      w->post_message(utf8(iso, info[0]), std::move(args));
    }

    // ---- new window methods -------------------------------------------------

    void win_set_title(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1)
        return;
      auto s = utf8(iso, info[0]);
      w->set_title(s);
    }
    void win_title(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      info.GetReturnValue().Set(str(iso, w->get_title()));
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
      (void)arr->Set(ctx, 0, Integer::NewFromUnsigned(iso, sz.x));
      (void)arr->Set(ctx, 1, Integer::NewFromUnsigned(iso, sz.y));
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
      (void)arr->Set(ctx, 0, Integer::New(iso, p.x));
      (void)arr->Set(ctx, 1, Integer::New(iso, p.y));
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
      (void)obj->Set(ctx, "x"_v8(iso), Integer::New(iso, bounds.x));
      (void)obj->Set(ctx, "y"_v8(iso), Integer::New(iso, bounds.y));
      (void)obj->Set(ctx, "width"_v8(iso), Integer::New(iso, bounds.z));
      (void)obj->Set(ctx, "height"_v8(iso), Integer::New(iso, bounds.w));
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
      (void)arr->Set(ctx, 0, Integer::New(iso, size->x));
      (void)arr->Set(ctx, 1, Integer::New(iso, size->y));
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
      (void)arr->Set(ctx, 0, Integer::New(iso, size->x));
      (void)arr->Set(ctx, 1, Integer::New(iso, size->y));
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
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(w->opacity())));
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
      auto name = utf8(iso, info[0]);
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
        throw_native_error(iso, err);
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
        throw_native_error(iso, err);
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
          throw_native_error(iso, err);
        }
        return;
      }
      if (!info[0]->IsString()) {
        (void)throw_type_error(iso, "setVibrancy: expected 'sidebar', 'titlebar', 'menu', or null");
        return;
      }
      auto name = utf8(iso, info[0]);
      if (!parse_vibrancy_kind(name)) {
        (void)throw_type_error(iso, "setVibrancy: unknown kind '" + name + "'");
        return;
      }
      try {
        info.GetReturnValue().Set(w->set_vibrancy(name.c_str()));
      } catch (const std::exception& err) {
        throw_native_error(iso, err);
      }
    }
    void win_set_blur_behind(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w) {
        info.GetReturnValue().Set(false);
        return;
      }
      try {
        info.GetReturnValue().Set(
            w->set_blur_behind(info.Length() >= 1 && info[0]->BooleanValue(iso)));
      } catch (const std::exception& err) {
        throw_native_error(iso, err);
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
        throw_native_error(iso, err);
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
      bool on = info.Length() >= 1 && info[0]->BooleanValue(iso);
      int monitor = -1;
      if (info.Length() >= 2 && info[1]->IsNumber())
        monitor = info[1]->Int32Value(ctx).FromMaybe(-1);
      w->set_fullscreen(on, monitor);
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
      auto name = utf8(iso, info[0]);
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
      (void)arr->Set(ctx, 0, Number::New(iso, static_cast<double>(p.x)));
      (void)arr->Set(ctx, 1, Number::New(iso, static_cast<double>(p.y)));
      info.GetReturnValue().Set(arr);
    }
    void win_set_cursor_lock(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      w->set_cursor_lock(info.Length() >= 1 && info[0]->BooleanValue(iso));
    }
    void win_clipboard_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w)
        return;
      auto s = w->clipboard_text();
      info.GetReturnValue().Set(str(iso, s));
    }
    void win_set_clipboard_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1)
        return;
      w->set_clipboard_text(utf8(iso, info[0]));
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
      (void)out->Set(ctx, "width"_v8(iso), Integer::NewFromUnsigned(iso, image.width));
      (void)out->Set(ctx, "height"_v8(iso), Integer::NewFromUnsigned(iso, image.height));
      (void)out->Set(ctx, "data"_v8(iso), Uint8Array::New(ab, 0, image.data.size()));
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
      Local<Value> width_value;
      Local<Value> height_value;
      Local<Value> data_value;
      if (!object->Get(ctx, "width"_v8(iso)).ToLocal(&width_value) ||
          !object->Get(ctx, "height"_v8(iso)).ToLocal(&height_value) ||
          !object->Get(ctx, "data"_v8(iso)).ToLocal(&data_value) || !data_value->IsTypedArray() ||
          (!data_value->IsUint8Array() && !data_value->IsUint8ClampedArray())) {
        (void)throw_type_error(iso,
                               "writeClipboardImage: data must be Uint8Array or Uint8ClampedArray");
        return;
      }

      int width = width_value->Int32Value(ctx).FromMaybe(0);
      int height = height_value->Int32Value(ctx).FromMaybe(0);
      auto data = data_value.As<TypedArray>();
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
      info.GetReturnValue().Set(str(iso, *value));
    }

    void win_set_clipboard_html(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(w->set_clipboard_html(utf8(iso, info[0])));
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
      info.GetReturnValue().Set(str(iso, *value));
    }

    void win_set_clipboard_rtf(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1) {
        info.GetReturnValue().Set(false);
        return;
      }
      info.GetReturnValue().Set(w->set_clipboard_rtf(utf8(iso, info[0])));
    }

    void win_clipboard_mime(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* w = unwrap_win(info.This());
      if (!w || info.Length() < 1) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto bytes = w->clipboard_mime(utf8(iso, info[0]));
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
      info.GetReturnValue().Set(w->set_clipboard_mime(utf8(iso, info[0]), bytes));
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
          Local<Value> el;
          if (!arr->Get(ctx, i).ToLocal(&el))
            continue;
          int x = 0, y = 0, ww = 0, hh = 0;
          if (el->IsArray()) {
            auto a = el.As<Array>();
            auto get_i = [&](u32 idx) -> int {
              Local<Value> v;
              if (!a->Get(ctx, idx).ToLocal(&v))
                return 0;
              return v->Int32Value(ctx).FromMaybe(0);
            };
            x = get_i(0);
            y = get_i(1);
            ww = get_i(2);
            hh = get_i(3);
          } else if (el->IsObject()) {
            auto o = el.As<Object>();
            auto get_i = [&](Local<String> k) -> int {
              Local<Value> v;
              if (!o->Get(ctx, k).ToLocal(&v))
                return 0;
              return v->Int32Value(ctx).FromMaybe(0);
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

      Local<Value> files_value;
      if (object->Get(ctx, "files"_v8(iso)).ToLocal(&files_value) && files_value->IsArray()) {
        auto files = files_value.As<Array>();
        const u32 count = files->Length();
        payload.files.reserve(count);
        for (u32 i = 0; i < count; ++i) {
          Local<Value> value;
          if (files->Get(ctx, i).ToLocal(&value) && value->IsString())
            payload.files.push_back(utf8(iso, value));
        }
      }

      Local<Value> text_value;
      if (object->Get(ctx, "text"_v8(iso)).ToLocal(&text_value) && text_value->IsString())
        payload.text = utf8(iso, text_value);

      auto parse_drag_image = [&](Local<Value> value, image_data& out) -> bool {
        if (!value->IsObject())
          return false;
        auto object = value.As<Object>();
        Local<Value> width_value;
        Local<Value> height_value;
        Local<Value> data_value;
        if (!object->Get(ctx, "width"_v8(iso)).ToLocal(&width_value) ||
            !object->Get(ctx, "height"_v8(iso)).ToLocal(&height_value) ||
            !object->Get(ctx, "data"_v8(iso)).ToLocal(&data_value) || !data_value->IsTypedArray() ||
            (!data_value->IsUint8Array() && !data_value->IsUint8ClampedArray()))
          return false;
        int width = width_value->Int32Value(ctx).FromMaybe(0);
        int height = height_value->Int32Value(ctx).FromMaybe(0);
        auto data = data_value.As<TypedArray>();
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

      Local<Value> html_value;
      if (object->Get(ctx, "html"_v8(iso)).ToLocal(&html_value) && html_value->IsString())
        payload.html = utf8(iso, html_value);

      Local<Value> icon_value;
      image_data icon;
      if (object->Get(ctx, "icon"_v8(iso)).ToLocal(&icon_value) &&
          parse_drag_image(icon_value, icon))
        payload.icon = std::move(icon);

      Local<Value> image_value;
      image_data image;
      if (object->Get(ctx, "image"_v8(iso)).ToLocal(&image_value) &&
          parse_drag_image(image_value, image))
        payload.image = std::move(image);

      if (payload.files.empty() && !payload.text && !payload.html && !payload.image) {
        info.GetReturnValue().Set(false);
        return;
      }

      try {
        info.GetReturnValue().Set(w->start_drag(payload));
      } catch (const std::exception& err) {
        throw_native_error(iso, err);
      }
    }

    // ---- listener registration ---------------------------------------------

    // Disposer callback installed by `on()`. Reads {window, event, token}
    // from its bound Data and removes the corresponding entry.
    void listener_disposer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto data = info.Data().As<Object>();
      Local<Value> v;
      if (!data->Get(ctx, "win"_v8(iso)).ToLocal(&v) || !v->IsExternal())
        return;
      auto* w = external_ptr<window>(v);
      auto* h = lookup_holder(w);
      if (!h)
        return;
      Local<Value> ev_v;
      if (!data->Get(ctx, "event"_v8(iso)).ToLocal(&ev_v))
        return;
      auto event = utf8(iso, ev_v);
      Local<Value> tok_v;
      if (!data->Get(ctx, "token"_v8(iso)).ToLocal(&tok_v))
        return;
      double tok_d = tok_v->NumberValue(ctx).FromMaybe(0.0);
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
      auto event = utf8(iso, info[0]);
      if (!is_known_event(event)) {
        (void)throw_type_error(iso, "on: unknown event '" + event + "'");
        return;
      }
      auto fn = info[1].As<Function>();
      u64 token = h->next_token++;
      h->listeners[event].push_back({token, Global<Function>(iso, fn)});

      // Build disposer.
      auto data = Object::New(iso);
      (void)data->Set(ctx, "win"_v8(iso), make_external(iso, w));
      (void)data->Set(ctx, "event"_v8(iso), str(iso, event));
      (void)data->Set(ctx, "token"_v8(iso), Number::New(iso, static_cast<double>(token)));
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
      auto event = utf8(iso, info[0]);
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
        auto event = utf8(iso, info[0]);
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
      // Disabled vsync: the loop polls events every iteration instead of
      // sleeping inside `wait_events_timeout`, since short Cocoa waits
      // periodically overshoot by 25-55 ms.
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
      Local<Value> x;
      double fps = 0.0;
      if (opts->Get(ctx, "fps"_v8(iso)).ToLocal(&x) && x->IsNumber()) {
        fps = x->NumberValue(ctx).FromMaybe(0.0);
      }
      if (fps <= 0.0 && opts->Get(ctx, "animate"_v8(iso)).ToLocal(&x) && x->BooleanValue(iso)) {
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
            auto exc = utf8(iso, tc.Exception());
            std::fprintf(stderr, "[fxe] uncaught in onFrame: %s\n", exc.c_str());
            std::fflush(stderr);
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

      while (true) {
        auto wins = h->windows();
        if (wins.empty())
          break;
        bool all_closed =
            std::all_of(wins.begin(), wins.end(), [](window* w) { return w->should_close(); });
        if (all_closed)
          break;

        // One global GLFW event-pump call drives every window. In
        // continuous mode we poll instead of waiting: short Cocoa waits
        // (e.g. wait_events_timeout(1ms)) periodically stall in mach_msg
        // for 25-55 ms even when no events are pending, which surfaces as
        // 30+ ms frame-time spikes under --no-lazy --no-vsync.
        if (continuous) {
          wins.front()->wait_events_timeout(0.0);
        } else {
          double timer_dl = fxe::js::next_timer_deadline_seconds(iso);
          double timeout = frame_period > 0.0 ? frame_period : 0.05;
          if (timer_dl < timeout)
            timeout = timer_dl;
          if (timeout < 0.0)
            timeout = 0.0;
          wins.front()->wait_events_timeout(timeout);
        }
        if (continuous || frame_period > 0.0)
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
      auto set = [&](Local<String> k, Local<Value> v) { (void)o->Set(ctx, k, v); };
      set("name"_v8(iso), str(iso, m.name));
      set("x"_v8(iso), Integer::New(iso, m.x));
      set("y"_v8(iso), Integer::New(iso, m.y));
      set("width"_v8(iso), Integer::New(iso, m.width));
      set("height"_v8(iso), Integer::New(iso, m.height));
      set("workX"_v8(iso), Integer::New(iso, m.work_x));
      set("workY"_v8(iso), Integer::New(iso, m.work_y));
      set("workWidth"_v8(iso), Integer::New(iso, m.work_width));
      set("workHeight"_v8(iso), Integer::New(iso, m.work_height));
      set("scaleX"_v8(iso), Number::New(iso, static_cast<double>(m.scale_x)));
      set("scaleY"_v8(iso), Number::New(iso, static_cast<double>(m.scale_y)));
      set("refreshHz"_v8(iso), Integer::New(iso, m.refresh_hz));
      set("primary"_v8(iso), Boolean::New(iso, m.primary));
      return o;
    }

    void monitors_list(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto mons = list_monitors();
      auto arr = Array::New(iso, static_cast<int>(mons.size()));
      for (usize i = 0; i < mons.size(); ++i)
        (void)arr->Set(ctx, static_cast<u32>(i), monitor_to_js(iso, ctx, mons[i]));
      info.GetReturnValue().Set(arr);
    }

    void monitors_primary(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      info.GetReturnValue().Set(monitor_to_js(iso, ctx, primary_monitor()));
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
        (void)arr->Set(ctx, static_cast<u32>(i), obj);
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

    global->Set(iso, "Window", tpl);
    win_tpl_table()[iso].Reset(iso, tpl);

    // Monitors namespace.
    auto mons = ObjectTemplate::New(iso);
    mons->Set(iso, "list", FunctionTemplate::New(iso, monitors_list));
    mons->Set(iso, "primary", FunctionTemplate::New(iso, monitors_primary));
    global->Set(iso, "Monitors", mons);

    // App namespace.
    auto app = ObjectTemplate::New(iso);
    app->Set(iso, "run", FunctionTemplate::New(iso, app_run));
    app->Set(iso, "quit", FunctionTemplate::New(iso, app_quit));
    app->Set(iso, "windows", FunctionTemplate::New(iso, app_windows));
    global->Set(iso, "App", app);
  }

  Local<Object> make_window_object(Isolate* iso, Local<Context> ctx, window* w) {
    return wrap(iso, ctx, win_tpl_table()[iso].Get(iso), w, TAG_WINDOW);
  }
} // namespace fxe::js
