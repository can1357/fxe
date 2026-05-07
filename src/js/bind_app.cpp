// Extras for the JS `App` global. The base `App` (run/quit/windows) is
// installed in bind_window.cpp. We mutate the live global object in
// install_app_extras_to() to layer on the OS-shim methods so we don't fight
// over the ObjectTemplate slot.

#include "bind_app.hpp"
#include "../net/http_client.hpp"
#include "../os/os.hpp"
#include "../runtime/updater.hpp"
#include "fxe/single_instance.hpp"
#include <fxe/js_bindings.hpp>

// FXE_APP_NAME and FXE_APP_VERSION may be injected by the build as quoted
// string-literal compile definitions; keep local defaults for unbranded builds.
#ifndef FXE_APP_NAME
#define FXE_APP_NAME "fxe"
#endif
#ifndef FXE_APP_VERSION
#define FXE_APP_VERSION "0.0.0"
#endif
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <v8.h>
#include <vector>

namespace fxe::js {
  using namespace v8;
  namespace {
    Local<String> s(Isolate* iso, const char* str) {
      return String::NewFromUtf8(iso, str, NewStringType::kNormal).ToLocalChecked();
    }
    std::string to_str(Isolate* iso, Local<Value> v) {
      if (v.IsEmpty() || !v->IsString())
        return {};
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    bool require_string_arg(const FunctionCallbackInfo<Value>& info, const char* message,
                            std::string& out) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(Exception::TypeError(s(iso, message)));
        return false;
      }
      out = to_str(iso, info[0]);
      return true;
    }

    void throw_native_error(Isolate* iso, Local<Context> ctx, const char* code,
                            const char* message) {
      auto err = Exception::Error(s(iso, message)).As<Object>();
      (void)err->Set(ctx, s(iso, "code"), s(iso, code));
      iso->ThrowException(err);
    }

    struct persistent_callback_refs {
      Isolate* isolate = nullptr;
      Global<Context> context;
      Global<Function> function;
    };

    std::mutex& persistent_callback_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<Isolate*, std::vector<std::weak_ptr<persistent_callback_refs>>>&
    persistent_callback_table() {
      static std::unordered_map<Isolate*, std::vector<std::weak_ptr<persistent_callback_refs>>>
          table;
      return table;
    }

    void reset_persistent_callbacks_for_isolate(Isolate* iso) {
      std::lock_guard<std::mutex> lock(persistent_callback_mutex());
      auto& table = persistent_callback_table();
      auto it = table.find(iso);
      if (it == table.end())
        return;
      for (auto& weak : it->second) {
        if (auto refs = weak.lock()) {
          refs->context.Reset();
          refs->function.Reset();
          refs->isolate = nullptr;
        }
      }
      table.erase(it);
    }

    struct persistent_callback_resetter_register {
      persistent_callback_resetter_register() {
        register_template_resetter(&reset_persistent_callbacks_for_isolate);
      }
    };
    static persistent_callback_resetter_register s_persistent_callback_resetter_register;

    std::shared_ptr<persistent_callback_refs>
    make_persistent_callback(Isolate* iso, Local<Context> ctx, Local<Function> fn) {
      auto refs = std::make_shared<persistent_callback_refs>();
      refs->isolate = iso;
      refs->context.Reset(iso, ctx);
      refs->function.Reset(iso, fn);
      std::lock_guard<std::mutex> lock(persistent_callback_mutex());
      persistent_callback_table()[iso].push_back(refs);
      return refs;
    }

    void app_get_name(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(s(info.GetIsolate(), FXE_APP_NAME));
    }
    void app_get_version(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(s(info.GetIsolate(), FXE_APP_VERSION));
    }
    void app_get_path(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string kind = info.Length() > 0 ? to_str(iso, info[0]) : std::string{"userData"};
      auto p = fxe::os::get_path(kind);
      info.GetReturnValue().Set(s(iso, p.c_str()));
    }
    void app_bookmark_persist(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::string path;
      if (!require_string_arg(info, "App.bookmark.persist requires a path string", path))
        return;
      auto blob = fxe::os::bookmark_persist(path);
      if (!blob) {
        throw_native_error(iso, ctx, "ERR_FXE_BOOKMARK_PERSIST_FAILED",
                           "App.bookmark.persist failed");
        return;
      }
      info.GetReturnValue().Set(s(iso, blob->c_str()));
    }

    void app_bookmark_resolve(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::string blob;
      if (!require_string_arg(info, "App.bookmark.resolve requires a bookmark string", blob))
        return;
      auto resolved = fxe::os::bookmark_resolve(blob);
      if (!resolved) {
        throw_native_error(iso, ctx, "ERR_FXE_BOOKMARK_RESOLVE_FAILED",
                           "App.bookmark.resolve failed");
        return;
      }
      Local<Object> result = Object::New(iso);
      (void)result->Set(ctx, s(iso, "path"), s(iso, resolved->first.c_str()));
      (void)result->Set(ctx, s(iso, "isStale"), Boolean::New(iso, resolved->second));
      info.GetReturnValue().Set(result);
    }

    void app_bookmark_start_accessing(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string blob;
      if (!require_string_arg(info, "App.bookmark.startAccessing requires a bookmark string", blob))
        return;
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::bookmark_start_access(blob)));
    }

    void app_bookmark_stop_accessing(const FunctionCallbackInfo<Value>& info) {
      std::string blob;
      if (!require_string_arg(info, "App.bookmark.stopAccessing requires a bookmark string", blob))
        return;
      fxe::os::bookmark_stop_access(blob);
    }
    void app_open_window(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();

      Local<Value> window_ctor_value;
      if (!ctx->Global()->Get(ctx, s(iso, "Window")).ToLocal(&window_ctor_value)) {
        // Preserve the pending V8 exception from global Window lookup.
        return;
      }

      if (!window_ctor_value->IsFunction()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.openWindow requires global Window constructor")));
        return;
      }

      auto window_ctor = window_ctor_value.As<Function>();
      Local<Value> argv[] = {info[0]};
      Local<Object> window;
      if (!window_ctor
               ->NewInstance(ctx, info.Length() > 0 ? 1 : 0, info.Length() > 0 ? argv : nullptr)
               .ToLocal(&window)) {
        // Preserve the pending V8 exception from the Window constructor.
        return;
      }

      info.GetReturnValue().Set(window);
    }
    void app_request_single_instance_lock(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string id = info.Length() > 0 ? to_str(iso, info[0]) : std::string{"fxe"};
      bool ok = fxe::os::request_single_instance_lock(id);
      info.GetReturnValue().Set(Boolean::New(iso, ok));
    }
    void app_set_badge_count(const FunctionCallbackInfo<Value>& info) {
      int n = 0;
      if (info.Length() > 0 && info[0]->IsNumber())
        n = static_cast<int>(
            info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0));
      fxe::os::set_badge_count(n);
    }
    void app_when_ready(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Resolve(ctx, Undefined(iso));
      info.GetReturnValue().Set(resolver->GetPromise());
    }
    std::string get_obj_string(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                               const char* name) {
      Local<Value> value;
      if (!obj->Get(ctx, s(iso, name)).ToLocal(&value) || !value->IsString())
        return {};
      return to_str(iso, value);
    }

    std::string get_obj_string_any(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                   const char* first, const char* second) {
      std::string value = get_obj_string(iso, ctx, obj, first);
      if (!value.empty())
        return value;
      return get_obj_string(iso, ctx, obj, second);
    }

    void set_obj_string(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                        const std::string& value) {
      (void)obj->Set(ctx, s(iso, name), s(iso, value.c_str()));
    }

    bool get_obj_bool(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      bool fallback = false) {
      Local<Value> value;
      if (!obj->Get(ctx, s(iso, name)).ToLocal(&value) || value->IsUndefined() || value->IsNull()) {
        return fallback;
      }
      return value->BooleanValue(iso);
    }

    std::int64_t get_obj_int64(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                               const char* name, std::int64_t fallback = 0) {
      Local<Value> value;
      if (!obj->Get(ctx, s(iso, name)).ToLocal(&value) || !value->IsNumber())
        return fallback;
      return static_cast<std::int64_t>(value->IntegerValue(ctx).FromMaybe(fallback));
    }

    Local<Object> cookie_to_object(Isolate* iso, Local<Context> ctx, const fxe::net::cookie& c) {
      auto obj = Object::New(iso);
      set_obj_string(iso, ctx, obj, "name", c.name);
      set_obj_string(iso, ctx, obj, "value", c.value);
      set_obj_string(iso, ctx, obj, "domain", c.domain);
      set_obj_string(iso, ctx, obj, "path", c.path.empty() ? std::string("/") : c.path);
      (void)obj->Set(ctx, s(iso, "expires"), Number::New(iso, static_cast<double>(c.expires)));
      (void)obj->Set(ctx, s(iso, "secure"), Boolean::New(iso, c.secure));
      (void)obj->Set(ctx, s(iso, "httpOnly"), Boolean::New(iso, c.http_only));
      (void)obj->Set(ctx, s(iso, "hostOnly"), Boolean::New(iso, c.host_only));
      set_obj_string(iso, ctx, obj, "sameSite", fxe::net::cookie_same_site_name(c.same_site));
      return obj;
    }

    std::optional<fxe::net::cookie> cookie_from_object(Isolate* iso, Local<Context> ctx,
                                                       Local<Object> obj, std::string& error) {
      const std::string name = get_obj_string(iso, ctx, obj, "name");
      const std::string value = get_obj_string(iso, ctx, obj, "value");
      if (name.empty()) {
        error = "App.session.cookies.set requires cookie.name";
        return std::nullopt;
      }
      std::string header = name + "=" + value;
      const std::string domain = get_obj_string(iso, ctx, obj, "domain");
      const std::string path = get_obj_string(iso, ctx, obj, "path");
      const std::string url = get_obj_string(iso, ctx, obj, "url");
      const bool secure = get_obj_bool(iso, ctx, obj, "secure");
      const bool http_only = get_obj_bool(iso, ctx, obj, "httpOnly");
      const std::string same_site = get_obj_string(iso, ctx, obj, "sameSite");
      const std::int64_t expires = get_obj_int64(iso, ctx, obj, "expires", 0);
      if (!domain.empty())
        header += "; Domain=" + domain;
      if (!path.empty())
        header += "; Path=" + path;
      if (expires > 0)
        header += "; Expires=" + std::to_string(expires);
      if (secure)
        header += "; Secure";
      if (http_only)
        header += "; HttpOnly";
      if (!same_site.empty())
        header += "; SameSite=" + same_site;
      if (domain.empty() && url.empty()) {
        error = "App.session.cookies.set requires cookie.url when cookie.domain is omitted";
        return std::nullopt;
      }
      auto parsed = fxe::net::cookie_jar::parse_set_cookie(header, url);
      if (!parsed) {
        error = "App.session.cookies.set rejected invalid cookie";
        return std::nullopt;
      }
      return parsed;
    }

    void session_cookies_get_all(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      fxe::net::cookie_filter filter;
      if (info.Length() > 0 && info[0]->IsObject()) {
        auto obj = info[0].As<Object>();
        filter.name = get_obj_string(iso, ctx, obj, "name");
        filter.domain = get_obj_string(iso, ctx, obj, "domain");
        filter.url = get_obj_string(iso, ctx, obj, "url");
      }
      auto cookies = fxe::net::http_client::instance().cookies().get_all(std::move(filter));
      auto arr = Array::New(iso, static_cast<int>(cookies.size()));
      for (uint32_t i = 0; i < cookies.size(); ++i)
        (void)arr->Set(ctx, i, cookie_to_object(iso, ctx, cookies[i]));
      info.GetReturnValue().Set(arr);
    }

    void session_cookies_set(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.session.cookies.set requires a cookie object")));
        return;
      }
      std::string error;
      auto cookie = cookie_from_object(iso, ctx, info[0].As<Object>(), error);
      if (!cookie) {
        iso->ThrowException(Exception::TypeError(s(iso, error.c_str())));
        return;
      }
      if (!fxe::net::http_client::instance().cookies().set(std::move(*cookie))) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.session.cookies.set rejected invalid cookie")));
      }
    }

    void session_cookies_remove(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string name;
      if (!require_string_arg(info, "App.session.cookies.remove requires a cookie name", name))
        return;
      if (info.Length() < 2 || !info[1]->IsString()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.session.cookies.remove requires a URL")));
        return;
      }
      (void)fxe::net::http_client::instance().cookies().remove(name, to_str(iso, info[1]));
    }

    void session_cookies_clear(const FunctionCallbackInfo<Value>&) {
      fxe::net::http_client::instance().cookies().clear();
    }

    void session_cookies_persist(const FunctionCallbackInfo<Value>& info) {
      std::string path;
      if (!require_string_arg(info, "App.session.cookies.persist requires a path string", path))
        return;
      fxe::net::http_client::instance().cookies().persist(std::move(path));
    }

    void install_app_session_cookies(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
      Local<Value> session_value;
      Local<Object> session;
      if (appObj->Get(ctx, s(iso, "session")).ToLocal(&session_value) &&
          session_value->IsObject()) {
        session = session_value.As<Object>();
      } else {
        session = Object::New(iso);
        (void)appObj->Set(ctx, s(iso, "session"), session);
      }
      auto cookies = Object::New(iso);
      auto set_fn = [&](const char* name, FunctionCallback cb) {
        (void)cookies->Set(ctx, s(iso, name), Function::New(ctx, cb).ToLocalChecked());
      };
      set_fn("getAll", session_cookies_get_all);
      set_fn("set", session_cookies_set);
      set_fn("remove", session_cookies_remove);
      set_fn("clear", session_cookies_clear);
      set_fn("persist", session_cookies_persist);
      (void)session->Set(ctx, s(iso, "cookies"), cookies);
    }

    bool bytes_from_value(Local<Value> value, std::vector<std::uint8_t>& out) {
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        out.resize(view->ByteLength());
        if (!out.empty())
          (void)view->CopyContents(out.data(), out.size());
        return true;
      }
      if (value->IsArrayBuffer()) {
        auto backing = value.As<ArrayBuffer>()->GetBackingStore();
        out.resize(backing->ByteLength());
        if (!out.empty())
          std::memcpy(out.data(), backing->Data(), out.size());
        return true;
      }
      return false;
    }

    void app_verify_update_signature(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 3 || !info[0]->IsString() || !info[1]->IsString() ||
          !info[2]->IsString()) {
        iso->ThrowException(Exception::TypeError(s(
            iso,
            "App.__fxeVerifyUpdateSignature requires signature, manifest, and publicKey strings")));
        return;
      }
      std::string error;
      const bool ok = fxe::runtime::verify_manifest_signature(
          to_str(iso, info[0]), to_str(iso, info[1]), to_str(iso, info[2]), error);
      info.GetReturnValue().Set(Boolean::New(iso, ok));
    }

    void app_stage_update(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      Local<Object> result = Object::New(iso);
      auto set_bool = [&](const char* name, bool value) {
        (void)result->Set(ctx, s(iso, name), Boolean::New(iso, value));
      };
      auto set_string = [&](const char* name, const std::string& value) {
        (void)result->Set(ctx, s(iso, name), s(iso, value.c_str()));
      };

      if (info.Length() < 2 || !info[0]->IsObject()) {
        iso->ThrowException(Exception::TypeError(s(
            iso, "App.__fxeStageUpdate requires an update descriptor object and artifact bytes")));
        return;
      }
      fxe::runtime::update_descriptor d;
      auto obj = info[0].As<Object>();
      d.version = get_obj_string(iso, ctx, obj, "version");
      d.url = get_obj_string(iso, ctx, obj, "url");
      d.sha256 = get_obj_string(iso, ctx, obj, "sha256");
      d.signature = get_obj_string(iso, ctx, obj, "signature");
      d.signature_algorithm = get_obj_string(iso, ctx, obj, "signatureAlgorithm");
      d.canonical_manifest = get_obj_string(iso, ctx, obj, "canonicalManifest");
      d.expected_public_key = get_obj_string(iso, ctx, obj, "expectedPublicKey");
      d.expected_signing_authority = get_obj_string_any(iso, ctx, obj, "expected_signing_authority",
                                                        "expectedSigningAuthority");
      d.expected_subject = get_obj_string_any(iso, ctx, obj, "expected_subject", "expectedSubject");
      std::string channel = get_obj_string(iso, ctx, obj, "channel");
      if (!channel.empty()) {
        auto parsed = fxe::runtime::updater::parse_channel(channel);
        if (!parsed) {
          iso->ThrowException(Exception::TypeError(
              s(iso, "App.__fxeStageUpdate descriptor.channel must be stable, beta, or alpha")));
          return;
        }
        d.channel = *parsed;
      }
      if (d.version.empty()) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "App.__fxeStageUpdate requires descriptor.version as a non-empty string")));
        return;
      }
      if (d.url.empty()) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "App.__fxeStageUpdate requires descriptor.url as a non-empty string")));
        return;
      }
      if (d.sha256.size() != 64) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "App.__fxeStageUpdate requires descriptor.sha256 as a 64-character string")));
        return;
      }
      if (!d.signature.empty() && (d.canonical_manifest.empty() || d.expected_public_key.empty())) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "App.__fxeStageUpdate requires descriptor.canonicalManifest and "
                   "descriptor.expectedPublicKey for signed updates")));
        return;
      }
      d.user_data_dir = fxe::os::get_path("userData");
      if (!bytes_from_value(info[1], d.artifact)) {
        iso->ThrowException(Exception::TypeError(s(
            iso, "App.__fxeStageUpdate requires artifact bytes as an ArrayBuffer or typed array")));
        return;
      }
      std::string error;
      auto pending = fxe::runtime::updater::stage(d, error);
      if (!pending) {
        const std::string message = error.empty() ? "failed to stage update" : error;
        auto err = Exception::Error(s(iso, message.c_str())).As<Object>();
        (void)err->Set(ctx, s(iso, "code"), s(iso, "ERR_FXE_UPDATE_STAGE_FAILED"));
        iso->ThrowException(err);
        return;
      }
      set_bool("ok", true);
      set_string("pendingPath", *pending);
      info.GetReturnValue().Set(result);
    }

    void app_set_update_channel(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.update.setChannel requires stable, beta, or alpha")));
        return;
      }
      auto parsed = fxe::runtime::updater::parse_channel(to_str(iso, info[0]));
      if (!parsed) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.update.setChannel requires stable, beta, or alpha")));
        return;
      }
      std::string error;
      if (!fxe::runtime::updater::set_channel(*parsed, error)) {
        const std::string message = error.empty() ? "failed to set update channel" : error;
        iso->ThrowException(Exception::Error(s(iso, message.c_str())));
      }
    }

    void app_get_update_channel(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      info.GetReturnValue().Set(
          s(iso, fxe::runtime::updater::channel_name(fxe::runtime::updater::channel())));
    }

    void app_resolve_update_feed_url(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.__fxeResolveUpdateFeedUrl requires a URL")));
        return;
      }
      const auto resolved = fxe::runtime::updater::substitute_channel(to_str(iso, info[0]));
      info.GetReturnValue().Set(s(iso, resolved.c_str()));
    }

    void app_update_device_id(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string error;
      std::string id = fxe::runtime::updater::device_id(error);
      if (id.empty()) {
        const std::string message = error.empty() ? "failed to read update device id" : error;
        iso->ThrowException(Exception::Error(s(iso, message.c_str())));
        return;
      }
      info.GetReturnValue().Set(s(iso, id.c_str()));
    }

    void app_update_rollout_eligible(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsNumber()) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "App.__fxeUpdateRolloutEligible requires rollout percent")));
        return;
      }
      int percent = info[0]->Int32Value(ctx).FromMaybe(0);
      std::string id;
      if (info.Length() > 1 && info[1]->IsString()) {
        id = to_str(iso, info[1]);
      } else {
        std::string error;
        id = fxe::runtime::updater::device_id(error);
        if (id.empty()) {
          const std::string message = error.empty() ? "failed to read update device id" : error;
          iso->ThrowException(Exception::Error(s(iso, message.c_str())));
          return;
        }
      }
      info.GetReturnValue().Set(
          Boolean::New(iso, fxe::runtime::updater::rollout_eligible(percent, id)));
    }

    void app_update_rollback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string error;
      if (!fxe::runtime::updater::rollback(error)) {
        const std::string message = error.empty() ? "failed to roll back update" : error;
        auto err = Exception::Error(s(iso, message.c_str())).As<Object>();
        (void)err->Set(iso->GetCurrentContext(), s(iso, "code"),
                       s(iso, "ERR_FXE_UPDATE_ROLLBACK_FAILED"));
        iso->ThrowException(err);
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, true));
    }

    void app_update_history(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::string error;
      auto history = fxe::runtime::updater::history(error);
      Local<Array> out = Array::New(iso, static_cast<int>(history.size()));
      for (std::size_t i = 0; i < history.size(); ++i)
        (void)out->Set(ctx, static_cast<std::uint32_t>(i), s(iso, history[i].c_str()));
      info.GetReturnValue().Set(out);
    }

    void app_apply_pending_update(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string error;
      if (!fxe::runtime::updater::apply_pending(error)) {
        const std::string message = error.empty() ? "failed to apply pending update" : error;
        iso->ThrowException(Exception::Error(s(iso, message.c_str())));
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, true));
    }

    void app_relaunch(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() > 0 && info[0]->IsObject()) {
        Local<Value> install_update;
        if (info[0].As<Object>()->Get(ctx, s(iso, "installUpdate")).ToLocal(&install_update) &&
            install_update->BooleanValue(iso)) {
          std::string error;
          if (!fxe::runtime::updater::apply_pending(error)) {
            const std::string message = error.empty() ? "failed to apply pending update" : error;
            auto err = Exception::Error(s(iso, message.c_str())).As<Object>();
            (void)err->Set(ctx, s(iso, "code"), s(iso, "ERR_FXE_UPDATE_APPLY_FAILED"));
            iso->ThrowException(err);
            return;
          }
        }
      }
      fxe::os::relaunch();
    }
    Local<Function> make_check_for_updates(Isolate* iso, Local<Context> ctx) {
      constexpr const char* kSource = R"JS(
(async function checkForUpdates(manifestUrl, opts) {
  const readManifest = async (url, requestOpts) => {
    if (typeof url !== 'string' || url.length === 0) {
      throw new TypeError('App.checkForUpdates requires a manifest URL string');
    }
    const fetchFn = globalThis.fetch;
    if (typeof fetchFn !== 'function') {
      throw new Error('App.checkForUpdates requires globalThis.fetch; runtime update checks cannot download the manifest without fetch');
    }
    const response = await fetchFn(url, requestOpts);
    if (response == null || typeof response !== 'object') {
      throw new Error('App.checkForUpdates fetch returned an invalid response');
    }
    if (typeof response.ok === 'boolean' && !response.ok) {
      const status = typeof response.status === 'number' ? ` (status ${response.status})` : '';
      throw new Error(`App.checkForUpdates failed to fetch manifest${status}`);
    }
    if (typeof response.json === 'function') {
      return await response.json();
    }
    if (typeof response.text === 'function') {
      return JSON.parse(await response.text());
    }
    throw new Error('App.checkForUpdates response must provide json() or text()');
  };
  const canonicalJson = (value, omitSignature = false) => {
    if (value === null || typeof value !== 'object') {
      return JSON.stringify(value);
    }
    if (Array.isArray(value)) {
      return `[${value.map((v) => canonicalJson(v)).join(',')}]`;
    }
    const keys = Object.keys(value).filter((k) => !(omitSignature && k === 'signature')).sort();
    return `{${keys.map((k) => `${JSON.stringify(k)}:${canonicalJson(value[k])}`).join(',')}}`;
  };
  const splitRequestOptions = (rawOpts) => {
    const requestOpts = (rawOpts && typeof rawOpts === 'object') ? { ...rawOpts } : rawOpts;
    let expectedPublicKey;
    let expectedFeedPublicKey;
    let publicKey;
    let deviceId;
    if (requestOpts && typeof requestOpts === 'object') {
      expectedPublicKey = requestOpts.expectedPublicKey;
      expectedFeedPublicKey = requestOpts.expectedFeedPublicKey;
      publicKey = requestOpts.publicKey;
      deviceId = requestOpts.deviceId;
      delete requestOpts.expectedPublicKey;
      delete requestOpts.expectedFeedPublicKey;
      delete requestOpts.publicKey;
      delete requestOpts.deviceId;
      delete requestOpts.manifestUrl;
      delete requestOpts.relaunch;
    }
    return { requestOpts, expectedPublicKey, expectedFeedPublicKey, publicKey, deviceId };
  };
  const validChannel = (value) => value === 'stable' || value === 'beta' || value === 'alpha';
  const { requestOpts, expectedPublicKey, expectedFeedPublicKey, publicKey, deviceId } =
    splitRequestOptions(opts);
  const resolvedManifestUrl = App.__fxeResolveUpdateFeedUrl(manifestUrl);
  const manifest = await readManifest(resolvedManifestUrl, requestOpts);
  if (manifest == null || typeof manifest !== 'object' || Array.isArray(manifest)) {
    throw new Error('App.checkForUpdates manifest must be an object');
  }
  const requireString = (field) => {
    const value = manifest[field];
    if (typeof value !== 'string' || value.length === 0) {
      throw new Error(`App.checkForUpdates manifest field ${field} must be a non-empty string`);
    }
    return value;
  };
  const optionalString = (first, second) => {
    const value = manifest[first] ?? (second ? manifest[second] : undefined);
    if (value === undefined) {
      return '';
    }
    if (typeof value !== 'string' || value.length === 0) {
      throw new Error(`App.checkForUpdates manifest field ${first} must be a non-empty string`);
    }
    return value;
  };
  const version = requireString('version');
  const url = App.__fxeResolveUpdateFeedUrl(requireString('url'));
  const sha256 = requireString('sha256');
  if (!/^[0-9a-fA-F]{64}$/.test(sha256)) {
    throw new Error('App.checkForUpdates manifest field sha256 must be a 64-character hexadecimal string');
  }
  const channel = App.__fxeGetUpdateChannel();
  const manifestChannel = optionalString('channel');
  if (manifestChannel && !validChannel(manifestChannel)) {
    throw new Error('App.checkForUpdates manifest field channel must be stable, beta, or alpha');
  }
  let rolloutPercent = 100;
  if ('rollout_percent' in manifest) {
    rolloutPercent = manifest.rollout_percent;
    if (!Number.isInteger(rolloutPercent) || rolloutPercent < 0 || rolloutPercent > 100) {
      throw new Error('App.checkForUpdates manifest field rollout_percent must be an integer from 0 to 100');
    }
  }
  let signature = '';
  let signatureAlgorithm = '';
  const key = expectedFeedPublicKey ?? expectedPublicKey ?? publicKey;
  const canonicalManifest = canonicalJson(manifest, true);
  let reason;
  let canInstall = true;
  const missingCapabilities = [];
  if (expectedPublicKey !== undefined && (typeof expectedPublicKey !== 'string' || expectedPublicKey.length === 0)) {
    throw new Error('App.checkForUpdates option expectedPublicKey must be a non-empty string for signed manifests');
  }
  if (expectedFeedPublicKey !== undefined && (typeof expectedFeedPublicKey !== 'string' || expectedFeedPublicKey.length === 0)) {
    throw new Error('App.checkForUpdates option expectedFeedPublicKey must be a non-empty string for signed manifests');
  }
  if (publicKey !== undefined && (typeof publicKey !== 'string' || publicKey.length === 0)) {
    throw new Error('App.checkForUpdates option publicKey must be a non-empty string for signed manifests');
  }
  if (expectedPublicKey !== undefined && publicKey !== undefined && expectedPublicKey !== publicKey) {
    throw new Error('App.checkForUpdates signed manifest publicKey does not match expectedPublicKey');
  }
  if (deviceId !== undefined && (typeof deviceId !== 'string' || deviceId.length === 0)) {
    throw new Error('App.checkForUpdates option deviceId must be a non-empty string');
  }
  if (!('signature' in manifest)) {
    canInstall = false;
    reason = 'signed update manifest is required';
    missingCapabilities.push('trusted signed manifest');
  } else {
    signature = requireString('signature');
    if ('signatureAlgorithm' in manifest) {
      signatureAlgorithm = requireString('signatureAlgorithm');
      if (signatureAlgorithm.toLowerCase() !== 'ed25519') {
        canInstall = false;
        reason = 'unsupported update signatureAlgorithm';
        missingCapabilities.push('trusted signed manifest');
      }
    }
    if (!key) {
      canInstall = false;
      reason = reason || 'signed update manifest requires expectedPublicKey';
      missingCapabilities.push('trusted signed manifest');
    } else if (canInstall && !App.__fxeVerifyUpdateSignature(signature, canonicalManifest, key)) {
      canInstall = false;
      reason = 'signature verification failed';
      missingCapabilities.push('trusted signed manifest');
    }
  }
  if (manifestChannel && manifestChannel !== channel) {
    canInstall = false;
    reason = reason || 'update channel mismatch';
  }
  if (!App.__fxeUpdateRolloutEligible(rolloutPercent, deviceId)) {
    canInstall = false;
    reason = reason || 'device is not eligible for rollout';
  }
  const currentVersion = typeof App.getVersion === 'function' ? App.getVersion() : undefined;
  const available = version !== currentVersion;
  if (!available) {
    canInstall = false;
    reason = reason || 'update version is already installed';
  }
  const descriptor = {
    version,
    url,
    sha256,
    signature,
    signatureAlgorithm,
    canonicalManifest,
    expectedPublicKey: key || '',
    expectedSigningAuthority: optionalString('expected_signing_authority', 'expectedSigningAuthority'),
    expectedSubject: optionalString('expected_subject', 'expectedSubject'),
    channel,
  };
  Object.defineProperty(App, '__fxeLastUpdateManifest', {
    value: { descriptor, manifestUrl: resolvedManifestUrl, requestOpts },
    configurable: true,
    enumerable: false,
    writable: true,
  });
  return {
    available,
    version,
    url,
    sha256,
    channel,
    rolloutPercent,
    canInstall,
    reason,
    installUnavailableReason: reason,
    missingCapabilities,
  };
})
)JS";
      Local<String> source = s(iso, kSource);
      Local<Script> script;
      if (!Script::Compile(ctx, source).ToLocal(&script))
        return Local<Function>();
      Local<Value> value;
      if (!script->Run(ctx).ToLocal(&value) || !value->IsFunction())
        return Local<Function>();
      return value.As<Function>();
    }

    Local<Function> make_install_update(Isolate* iso, Local<Context> ctx) {
      constexpr const char* kSource = R"JS(
(async function installUpdate(opts) {
  const fetchFn = globalThis.fetch;
  if (typeof fetchFn !== 'function') {
    throw new Error('App.installUpdate requires globalThis.fetch to download the update artifact');
  }
  let record = App.__fxeLastUpdateManifest;
  if (opts && typeof opts === 'object' && typeof opts.manifestUrl === 'string') {
    const check = await App.checkForUpdates(opts.manifestUrl, opts);
    if (!check.canInstall) {
      return { installed: false, reason: check.reason || check.installUnavailableReason || 'update cannot be installed' };
    }
    record = App.__fxeLastUpdateManifest;
  }
  if (!record || !record.descriptor) {
    return { installed: false, reason: 'no checked update manifest is available' };
  }
  const descriptor = { ...record.descriptor };
  if (opts && typeof opts === 'object') {
    if (typeof opts.expectedPublicKey === 'string') {
      descriptor.expectedPublicKey = opts.expectedPublicKey;
    }
    if (typeof opts.expectedFeedPublicKey === 'string') {
      descriptor.expectedPublicKey = opts.expectedFeedPublicKey;
    }
    if (typeof opts.publicKey === 'string' && !descriptor.expectedPublicKey) {
      descriptor.expectedPublicKey = opts.publicKey;
    }
  }
  const requestOpts = (opts && typeof opts === 'object') ? { ...opts } : (record.requestOpts || undefined);
  if (requestOpts && typeof requestOpts === 'object') {
    delete requestOpts.expectedPublicKey;
    delete requestOpts.expectedFeedPublicKey;
    delete requestOpts.publicKey;
    delete requestOpts.deviceId;
    delete requestOpts.manifestUrl;
    delete requestOpts.relaunch;
  }
  const response = await fetchFn(descriptor.url, requestOpts);
  if (response == null || typeof response !== 'object') {
    throw new Error('App.installUpdate fetch returned an invalid response');
  }
  if (typeof response.ok === 'boolean' && !response.ok) {
    const status = typeof response.status === 'number' ? ` (status ${response.status})` : '';
    throw new Error(`App.installUpdate failed to fetch artifact${status}`);
  }
  let bytes;
  if (typeof response.arrayBuffer === 'function') {
    bytes = new Uint8Array(await response.arrayBuffer());
  } else if (typeof response.text === 'function') {
    bytes = new TextEncoder().encode(await response.text());
  } else {
    throw new Error('App.installUpdate artifact response must provide arrayBuffer() or text()');
  }
  const staged = App.__fxeStageUpdate(descriptor, bytes);
  if (!staged || staged.ok !== true) {
    return { installed: false, reason: (staged && staged.reason) || 'failed to stage update' };
  }
  if (opts && typeof opts === 'object' && opts.relaunch === true) {
    App.relaunch({ installUpdate: true });
  }
  return { installed: true, pendingPath: staged.pendingPath };
})
)JS";
      Local<Script> script;
      if (!Script::Compile(ctx, s(iso, kSource)).ToLocal(&script))
        return Local<Function>();
      Local<Value> value;
      if (!script->Run(ctx).ToLocal(&value) || !value->IsFunction())
        return Local<Function>();
      return value.As<Function>();
    }

    void install_app_run_frame_bridge(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
      constexpr const char* kSource = R"JS(
(function installAppRunFrameBridge(app) {
  const nativeRun = app && app.run;
  if (typeof nativeRun !== 'function') {
    return;
  }
  if (app.__fxeUiFrameBridgeInstalled === true) {
    return;
  }
  Object.defineProperty(app, '__fxeUiFrameBridgeInstalled', {
    value: true,
    configurable: false,
    enumerable: false,
    writable: false,
  });
  Object.defineProperty(app, '__fxeNativeRun', {
    value: nativeRun,
    configurable: false,
    enumerable: false,
    writable: false,
  });
  app.run = function run(opts) {
    if (opts && typeof opts === 'object' && (opts.animate === true || Number(opts.fps) > 0)) {
      const ensureFrameLoop = globalThis.__fxeUiEnsureFrameLoop;
      if (typeof ensureFrameLoop === 'function') {
        ensureFrameLoop();
      }
    }
    return nativeRun.apply(this, arguments);
  };
})
)JS";
      Local<Script> script;
      if (!Script::Compile(ctx, s(iso, kSource)).ToLocal(&script)) {
        // Preserve the pending V8 exception from compiling the embedded bridge.
        return;
      }
      Local<Value> value;
      if (!script->Run(ctx).ToLocal(&value)) {
        // Preserve the pending V8 exception from bridge script execution.
        return;
      }
      if (!value->IsFunction()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "installAppRunFrameBridge must evaluate to a function")));
        return;
      }
      Local<Value> argv[1] = {appObj};
      Local<Value> ignored;
      if (!value.As<Function>()->Call(ctx, ctx->Global(), 1, argv).ToLocal(&ignored)) {
        // Preserve the pending V8 exception thrown while installing the bridge.
        return;
      }
    }

    void invoke_persistent_callback(std::shared_ptr<persistent_callback_refs> refs, int argc,
                                    Local<Value>* argv) {
      if (!refs || !refs->isolate || refs->context.IsEmpty() || refs->function.IsEmpty()) {
        // Callback owner was torn down; late OS callbacks are intentionally ignored.
        return;
      }
      Isolate* iso = refs->isolate;
      HandleScope hs(iso);
      auto ctx = refs->context.Get(iso);
      if (ctx.IsEmpty()) {
        // Callback context was reset during shutdown; drop the late callback.
        return;
      }
      Context::Scope cs(ctx);
      auto fn = refs->function.Get(iso);
      if (fn.IsEmpty()) {
        // Callback function was reset during shutdown; drop the late callback.
        return;
      }
      Local<Value> ignored;
      (void)fn->Call(ctx, ctx->Global(), argc, argv).ToLocal(&ignored);
    }

    void app_install_second_instance_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.__fxeOnSecondInstance requires a function")));
        return;
      }
      auto refs = make_persistent_callback(iso, iso->GetCurrentContext(), info[0].As<Function>());
      fxe::os::on_second_instance([refs](std::vector<std::string> argv, std::string cwd) {
        Isolate* iso = refs ? refs->isolate : nullptr;
        if (!iso || refs->context.IsEmpty()) {
          // Isolate shutdown reset this callback; drop the late OS event.
          return;
        }
        HandleScope hs(iso);
        auto ctx = refs->context.Get(iso);
        if (ctx.IsEmpty()) {
          // Isolate shutdown reset this callback context; drop the late OS event.
          return;
        }
        auto arr = Array::New(iso, static_cast<int>(argv.size()));
        for (uint32_t i = 0; i < argv.size(); ++i)
          (void)arr->Set(ctx, i, s(iso, argv[i].c_str()));
        Local<Value> js_argv[2] = {arr, s(iso, cwd.c_str())};
        invoke_persistent_callback(refs, 2, js_argv);
      });
    }

    void app_install_open_url_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        iso->ThrowException(Exception::TypeError(s(iso, "App.__fxeOnOpenUrl requires a function")));
        return;
      }
      auto refs = make_persistent_callback(iso, iso->GetCurrentContext(), info[0].As<Function>());
      fxe::os::on_open_url([refs](std::string url) {
        Isolate* iso = refs ? refs->isolate : nullptr;
        if (!iso) {
          // Isolate shutdown reset this callback; drop the late OS event.
          return;
        }
        HandleScope hs(iso);
        Local<Value> js_argv[1] = {s(iso, url.c_str())};
        invoke_persistent_callback(refs, 1, js_argv);
      });
    }

    void app_install_open_file_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.__fxeOnOpenFile requires a function")));
        return;
      }
      auto refs = make_persistent_callback(iso, iso->GetCurrentContext(), info[0].As<Function>());
      fxe::os::on_open_file([refs](std::string path) {
        Isolate* iso = refs ? refs->isolate : nullptr;
        if (!iso) {
          // Isolate shutdown reset this callback; drop the late OS event.
          return;
        }
        HandleScope hs(iso);
        Local<Value> js_argv[1] = {s(iso, path.c_str())};
        invoke_persistent_callback(refs, 1, js_argv);
      });
    }

    std::vector<std::string> string_array_arg(Isolate* iso, Local<Context> ctx,
                                              Local<Value> value) {
      std::vector<std::string> out;
      if (!value->IsArray())
        return out;
      auto arr = value.As<Array>();
      uint32_t len = arr->Length();
      out.reserve(len);
      for (uint32_t i = 0; i < len; ++i) {
        Local<Value> item;
        if (!arr->Get(ctx, i).ToLocal(&item))
          continue;
        out.push_back(to_str(iso, item));
      }
      return out;
    }

    void app_acquire_or_forward(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::vector<std::string> argv =
          info.Length() > 0 ? string_array_arg(iso, ctx, info[0]) : std::vector<std::string>{};
      std::vector<char*> raw;
      raw.reserve(argv.size());
      for (std::string& arg : argv)
        raw.push_back(arg.data());
      bool ok = fxe::os::acquire_or_forward(static_cast<int>(raw.size()),
                                            raw.empty() ? nullptr : raw.data());
      info.GetReturnValue().Set(Boolean::New(iso, ok));
    }

    void app_set_default_protocol_client(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string scheme = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::set_default_protocol_client(scheme)));
    }

    void app_set_default_file_handler(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::string ext = info.Length() > 0 ? to_str(iso, info[0]) : std::string{};
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::set_default_file_handler(ext)));
    }

    void app_recent_documents_add(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.recentDocuments.add requires a path string")));
        return;
      }
      std::string path = to_str(iso, info[0]);
      if (path.empty()) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "App.recentDocuments.add requires a non-empty path string")));
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::app::add_recent_document(path)));
    }

    void app_recent_documents_list(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto documents = fxe::os::app::recent_documents();
      auto arr = Array::New(iso, static_cast<int>(documents.size()));
      for (uint32_t i = 0; i < documents.size(); ++i)
        (void)arr->Set(ctx, i, s(iso, documents[i].c_str()));
      info.GetReturnValue().Set(arr);
    }

    void app_recent_documents_clear(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(
          Boolean::New(info.GetIsolate(), fxe::os::app::clear_recent_documents()));
    }

    void install_app_recent_documents(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
      Local<Object> recent;
      Local<Value> existing;
      if (appObj->Get(ctx, s(iso, "recentDocuments")).ToLocal(&existing) && existing->IsObject()) {
        recent = existing.As<Object>();
      } else {
        recent = Object::New(iso);
        (void)appObj->Set(ctx, s(iso, "recentDocuments"), recent);
      }
      (void)recent->Set(ctx, s(iso, "add"),
                        Function::New(ctx, app_recent_documents_add).ToLocalChecked());
      (void)recent->Set(ctx, s(iso, "list"),
                        Function::New(ctx, app_recent_documents_list).ToLocalChecked());
      (void)recent->Set(ctx, s(iso, "clear"),
                        Function::New(ctx, app_recent_documents_clear).ToLocalChecked());
    }

    void install_app_single_instance_bridge(Isolate* iso, Local<Context> ctx,
                                            Local<Object> appObj) {
      constexpr const char* kSource = R"JS(
(function installAppSingleInstanceBridge(app) {
  if (!app) {
    throw new TypeError('installAppSingleInstanceBridge requires App object');
  }
  if (app.__fxeSingleInstanceBridgeInstalled === true) {
    // Bridge installation is idempotent; repeated installs are intentionally ignored.
    return;
  }
  const listeners = {
    'second-instance': new Set(),
    'open-url': new Set(),
    'open-file': new Set(),
  };
  const pending = {
    'second-instance': [],
    'open-url': [],
    'open-file': [],
  };
  const emit = (event, args) => {
    if (listeners[event].size === 0) {
      // Buffer early OS events until JavaScript registers a listener.
      pending[event].push(args);
      return;
    }
    for (const cb of Array.from(listeners[event])) {
      cb(...args);
    }
  };
  Object.defineProperty(app, '__fxeSingleInstanceBridgeInstalled', {
    value: true,
    configurable: false,
    enumerable: false,
    writable: false,
  });
  app.on = function on(event, cb) {
    if (!Object.prototype.hasOwnProperty.call(listeners, event)) {
      throw new TypeError(`Unsupported App event: ${String(event)}`);
    }
    if (typeof cb !== 'function') {
      throw new TypeError('App.on requires a callback');
    }
    listeners[event].add(cb);
    const queued = pending[event].splice(0);
    for (const args of queued) {
      cb(...args);
    }
    return () => {
      listeners[event].delete(cb);
    };
  };
  app.__fxeOnSecondInstance((argv, cwd) => emit('second-instance', [argv, cwd]));
  app.__fxeOnOpenUrl((url) => emit('open-url', [url]));
  app.__fxeOnOpenFile((path) => emit('open-file', [path]));
  const argv = () => {
    const p = globalThis.process;
    return p && Array.isArray(p.argv) ? p.argv : [];
  };
  app.requestSingleInstanceLock = function requestSingleInstanceLock() {
    return app.__fxeAcquireOrForward(argv());
  };
})
)JS";
      Local<Script> script;
      if (!Script::Compile(ctx, s(iso, kSource)).ToLocal(&script)) {
        // Preserve the pending V8 exception from compiling the embedded bridge.
        return;
      }
      Local<Value> value;
      if (!script->Run(ctx).ToLocal(&value)) {
        // Preserve the pending V8 exception from bridge script execution.
        return;
      }
      if (!value->IsFunction()) {
        iso->ThrowException(Exception::TypeError(
            s(iso, "installAppSingleInstanceBridge must evaluate to a function")));
        return;
      }
      Local<Value> argv[1] = {appObj};
      Local<Value> ignored;
      if (!value.As<Function>()->Call(ctx, ctx->Global(), 1, argv).ToLocal(&ignored)) {
        // Preserve the pending V8 exception thrown while installing the bridge.
        return;
      }
    }

  } // namespace

  void install_app_extras_to(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
    HandleScope hs(iso);
    auto set_fn = [&](const char* name, FunctionCallback cb) {
      auto fn = Function::New(ctx, cb).ToLocalChecked();
      (void)appObj->Set(ctx, s(iso, name), fn);
    };
    set_fn("getName", app_get_name);
    set_fn("getVersion", app_get_version);
    set_fn("getPath", app_get_path);
    set_fn("requestSingleInstanceLock", app_request_single_instance_lock);
    set_fn("setBadgeCount", app_set_badge_count);
    set_fn("whenReady", app_when_ready);
    set_fn("openWindow", app_open_window);
    set_fn("relaunch", app_relaunch);
    Local<Object> bookmark = Object::New(iso);
    (void)bookmark->Set(ctx, s(iso, "persist"),
                        Function::New(ctx, app_bookmark_persist).ToLocalChecked());
    (void)bookmark->Set(ctx, s(iso, "resolve"),
                        Function::New(ctx, app_bookmark_resolve).ToLocalChecked());
    (void)bookmark->Set(ctx, s(iso, "startAccessing"),
                        Function::New(ctx, app_bookmark_start_accessing).ToLocalChecked());
    (void)bookmark->Set(ctx, s(iso, "stopAccessing"),
                        Function::New(ctx, app_bookmark_stop_accessing).ToLocalChecked());
    (void)appObj->Set(ctx, s(iso, "bookmark"), bookmark);
    set_fn("__fxeVerifyUpdateSignature", app_verify_update_signature);
    set_fn("__fxeStageUpdate", app_stage_update);
    set_fn("__fxeSetUpdateChannel", app_set_update_channel);
    set_fn("__fxeGetUpdateChannel", app_get_update_channel);
    set_fn("__fxeResolveUpdateFeedUrl", app_resolve_update_feed_url);
    set_fn("__fxeUpdateDeviceId", app_update_device_id);
    set_fn("__fxeUpdateRolloutEligible", app_update_rollout_eligible);
    set_fn("__fxeApplyPendingUpdate", app_apply_pending_update);
    install_app_run_frame_bridge(iso, ctx, appObj);
    auto check_for_updates = make_check_for_updates(iso, ctx);
    if (!check_for_updates.IsEmpty())
      (void)appObj->Set(ctx, s(iso, "checkForUpdates"), check_for_updates);
    auto install_update = make_install_update(iso, ctx);
    if (!install_update.IsEmpty())
      (void)appObj->Set(ctx, s(iso, "installUpdate"), install_update);
    Local<Object> update = Object::New(iso);
    (void)update->Set(ctx, s(iso, "setChannel"),
                      Function::New(ctx, app_set_update_channel).ToLocalChecked());
    (void)update->Set(ctx, s(iso, "getChannel"),
                      Function::New(ctx, app_get_update_channel).ToLocalChecked());
    (void)update->Set(ctx, s(iso, "rollback"),
                      Function::New(ctx, app_update_rollback).ToLocalChecked());
    (void)update->Set(ctx, s(iso, "history"),
                      Function::New(ctx, app_update_history).ToLocalChecked());
    if (!check_for_updates.IsEmpty())
      (void)update->Set(ctx, s(iso, "checkForUpdates"), check_for_updates);
    if (!install_update.IsEmpty())
      (void)update->Set(ctx, s(iso, "install"), install_update);
    (void)appObj->Set(ctx, s(iso, "update"), update);
    install_app_recent_documents(iso, ctx, appObj);
    install_app_session_cookies(iso, ctx, appObj);
    // ---- NEW: single-instance / deep-link / file-association bindings ----
    set_fn("__fxeOnSecondInstance", app_install_second_instance_callback);
    set_fn("__fxeOnOpenUrl", app_install_open_url_callback);
    set_fn("__fxeOnOpenFile", app_install_open_file_callback);
    set_fn("__fxeAcquireOrForward", app_acquire_or_forward);
    set_fn("setAsDefaultProtocolClient", app_set_default_protocol_client);
    set_fn("setAsDefaultFileHandler", app_set_default_file_handler);
    install_app_single_instance_bridge(iso, ctx, appObj);
  }
} // namespace fxe::js
