#include "inspector.hpp"

#include "../../../debug/dispatch.hpp"
#include "../../uv_loop.hpp"
#include <fxe/debug.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/v8_literals.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fxe::runtime {
  namespace {
    using json = fxe::debug::json;
    using namespace v8;

    using namespace fxe::js;
    Local<String> str(Isolate* iso, std::string_view value) {
      return String::NewFromUtf8(iso, value.data(), NewStringType::kNormal,
                                 static_cast<int>(value.size()))
          .ToLocalChecked();
    }

    std::string utf8_arg(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
    }

    Local<Promise> resolved([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> value) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Resolve(ctx, value);
      return resolver->GetPromise();
    }

    Local<Promise> rejected(Isolate* iso, Local<Context> ctx, std::string_view message) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Reject(ctx, Exception::Error(to_v8_string(iso, message)));
      return resolver->GetPromise();
    }

    std::string inspector_dispatch(Isolate* iso, std::string_view method,
                                   std::string_view params_json) {
      auto* srv = fxe::debug::active_server();
      if (!srv || !srv->running())
        throw std::runtime_error("fxe debug server is not running");
      auto* host = fxe::js::host_for_isolate(iso);
      auto* win = host ? host->active_window() : nullptr;
      auto* rdr = host ? host->active_renderer() : nullptr;
      json params = json::object();
      if (!params_json.empty())
        params = json::parse(params_json.begin(), params_json.end());
      if (!params.is_object())
        throw std::runtime_error("inspector params must decode to a JSON object");
      fxe::debug::dispatch_context cx{srv, host, win, rdr, 0};
      return fxe::debug::dispatch(cx, method, params).dump();
    }

    void dispatch_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        info.GetReturnValue().Set(rejected(
            iso, ctx, "__fxe_native.inspector.dispatch requires method and paramsJsonString"));
        return;
      }
      try {
        auto result = inspector_dispatch(iso, utf8_arg(iso, info[0]), utf8_arg(iso, info[1]));
        info.GetReturnValue().Set(resolved(iso, ctx, to_v8_string(iso, result)));
      } catch (const fxe::debug::dispatch_error& e) {
        info.GetReturnValue().Set(rejected(iso, ctx, e.message));
      } catch (const std::exception& e) {
        info.GetReturnValue().Set(rejected(iso, ctx, e.what()));
      }
    }

    void server_url_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* srv = fxe::debug::active_server();
      if (!srv || !srv->running() || srv->bound_port() == 0) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto url = std::string("ws://127.0.0.1:") + std::to_string(srv->bound_port()) +
                 "/devtools/page/fxe-main";
      info.GetReturnValue().Set(to_v8_string(iso, url));
    }

    struct wait_state {
      Isolate* iso = nullptr;
      Global<Context> ctx;
      Global<Promise::Resolver> resolver;

      explicit wait_state(Isolate* isolate, Local<Context> context, Local<Promise::Resolver> r)
          : iso(isolate), ctx(isolate, context), resolver(isolate, r) {}
    };

    void server_wait_for_connect_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());

      auto* srv = fxe::debug::active_server();
      if (!srv || !srv->running() || srv->has_clients() ||
          !uv_loop_runtime::instance().available()) {
        (void)resolver->Resolve(ctx, Undefined(iso));
        return;
      }

      auto state = std::make_shared<wait_state>(iso, ctx, resolver);
      srv->when_client_attached([state] {
        (void)uv_loop_runtime::instance().try_post([state] {
          Isolate::Scope isolate_scope(state->iso);
          HandleScope handle_scope(state->iso);
          auto ctx = state->ctx.Get(state->iso);
          Context::Scope context_scope(ctx);
          auto resolver = state->resolver.Get(state->iso);
          (void)resolver->Resolve(ctx, Undefined(state->iso));
        });
      });
    }
  } // namespace

  void install_native_inspector(Isolate* iso, Local<Context> ctx) {
    auto global = ctx->Global();
    Local<Value> native_value;
    Local<Object> native;
    if (global->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) &&
        native_value->IsObject()) {
      native = native_value.As<Object>();
    } else {
      native = Object::New(iso);
      define_prop(ctx, global, "__fxe_native"_v8, native, static_cast<PropertyAttribute>(DontEnum));
    }

    auto inspector = Object::New(iso);
    (void)inspector->Set(ctx, "dispatch"_v8(iso),
                         Function::New(ctx, dispatch_callback).ToLocalChecked());
    (void)inspector->Set(ctx, "serverUrl"_v8(iso),
                         Function::New(ctx, server_url_callback).ToLocalChecked());
    (void)inspector->Set(ctx, "serverWaitForConnect"_v8(iso),
                         Function::New(ctx, server_wait_for_connect_callback).ToLocalChecked());
    (void)native->Set(ctx, "inspector"_v8(iso), inspector);
  }
} // namespace fxe::runtime
