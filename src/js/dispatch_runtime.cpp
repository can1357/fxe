// V8-dependent debug protocol handlers. Compiled into fxe_js (which links
// libv8) and registered with fxe_debug at startup so the dispatcher can
// invoke them without fxe_debug itself needing libv8.
//
// fxe_debug owns the method table; this TU only provides the function bodies
// that need an active host. The first time a host is constructed, the
// registration helper runs.


#include "../debug/dispatch.hpp"

#include <fxe/debug.hpp>
#include <fxe/v8_host.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <v8.h>

namespace fxe::js {
  std::vector<std::string> invalidate_module_for_isolate(v8::Isolate* iso, std::string_view path,
                                                         std::string& error);
  v8::MaybeLocal<v8::Value> reimport_module_for_isolate(v8::Isolate* iso,
                                                        v8::Local<v8::Context> ctx,
                                                        std::string_view path, std::string& error);
  namespace {
    using namespace fxe::debug;

    [[noreturn]] void invalid_params(std::string msg) {
      throw dispatch_error{err_code::invalid_params, std::move(msg), ""};
    }
    [[noreturn]] void no_host() {
      throw dispatch_error{err_code::detached, "V8 host not attached", ""};
    }

    json runtime_evaluate(dispatch_context& cx, const json& params) {
      if (!cx.host)
        no_host();
      if (!params.is_object())
        invalid_params("expected object params");
      auto expr_p = params.find("expression");
      if (expr_p == params.end() || !expr_p->is_string())
        invalid_params("missing 'expression'");
      bool by_value = params.value("returnByValue", true);
      auto er = cx.host->debug_evaluate(expr_p->get<std::string>(), by_value);
      json out{json::object()};
      if (!er.exception.empty()) {
        out["exception"] = er.exception;
        json details{json::object()};
        details["text"] = er.exception;
        details["exceptionId"] = 1.0;
        if (er.position.has_position) {
          details["url"] = er.position.url;
          details["lineNumber"] = static_cast<double>(er.position.line_number);
          details["columnNumber"] = static_cast<double>(er.position.column_number);
        }
        if (er.position.has_original_position) {
          details["originalUrl"] = er.position.original_url;
          details["originalLineNumber"] = static_cast<double>(er.position.original_line_number);
          details["originalColumnNumber"] = static_cast<double>(er.position.original_column_number);
        }
        out["exceptionDetails"] = std::move(details);
      } else if (er.json_value.empty()) {
        out["value"] = nullptr;
      } else {
        try {
          out["value"] = json::parse(er.json_value);
        } catch (const nlohmann::json::parse_error&) {
          out["value"] = er.json_value;
        }
      }
      return out;
    }

    json runtime_get_globals(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      auto keys = cx.host->debug_global_keys();
      json::array_t a;
      for (auto& k : keys)
        a.emplace_back(k);
      json out{json::object()};
      out["keys"] = std::move(a);
      return out;
    }

    json runtime_fire_hmr(dispatch_context& cx, const json& params) {
      if (!cx.host)
        no_host();
      if (!params.is_object())
        invalid_params("expected object params");
      auto path_p = params.find("path");
      if (path_p == params.end() || !path_p->is_string())
        invalid_params("missing 'path'");
      int handlers_called = 0;
      auto result = cx.host->fire_hmr(path_p->get<std::string>(), handlers_called);
      if (!result.ok)
        throw dispatch_error{err_code::script_throw, result.message, ""};
      json out{json::object()};
      out["handlersCalled"] = static_cast<double>(handlers_called);
      return out;
    }

    json runtime_invalidate_module(dispatch_context& cx, const json& params) {
      if (!cx.host)
        no_host();
      if (!params.is_object())
        invalid_params("expected object params");
      auto path_p = params.find("path");
      if (path_p == params.end() || !path_p->is_string())
        invalid_params("missing 'path'");
      auto evicted = cx.host->invalidate_module(path_p->get<std::string>());
      json out{json::object()};
      json arr = json::array();
      for (const auto& s : evicted)
        arr.push_back(s);
      out["evicted"] = std::move(arr);
      return out;
    }

    json runtime_reimport_module(dispatch_context& cx, const json& params) {
      if (!cx.host)
        no_host();
      if (!params.is_object())
        invalid_params("expected object params");
      auto path_p = params.find("path");
      if (path_p == params.end() || !path_p->is_string())
        invalid_params("missing 'path'");
      auto result = cx.host->reimport_module(path_p->get<std::string>());
      if (!result.ok)
        throw dispatch_error{err_code::script_throw, result.message, ""};
      json out{json::object()};
      out["reimported"] = path_p->get<std::string>();
      return out;
    }

    json runtime_reconciler_snapshot(dispatch_context& cx, const json& params) {
      if (!cx.host)
        no_host();
      if (!params.is_object())
        invalid_params("expected object params");
      auto er = cx.host->debug_evaluate("(typeof globalThis.__fxeReconcilerSnapshot === 'function' "
                                        "? globalThis.__fxeReconcilerSnapshot() : { tree: [] })",
                                        true);
      if (!er.exception.empty())
        throw dispatch_error{err_code::script_throw, er.exception, ""};
      if (er.json_value.empty())
        return json{{"tree", json::array()}};
      try {
        auto out = json::parse(er.json_value);
        if (!out.is_object() || !out.contains("tree") || !out.at("tree").is_array())
          throw dispatch_error{err_code::script_throw,
                               "Reconciler.snapshot returned invalid payload", ""};
        return out;
      } catch (const nlohmann::json::parse_error&) {
        throw dispatch_error{err_code::script_throw,
                             "Reconciler.snapshot returned non-JSON payload", ""};
      }
    }

    json profiler_enable(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      cx.host->debug_profiler_enable();
      return json{json::object()};
    }

    json profiler_disable(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      cx.host->debug_profiler_disable();
      return json{json::object()};
    }

    json profiler_start(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      auto result = cx.host->debug_profiler_start();
      if (!result.ok)
        throw dispatch_error{err_code::service_unavailable, result.message, "profiler_unavailable"};
      return json{json::object()};
    }

    json profiler_stop(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      auto result = cx.host->debug_profiler_stop();
      if (!result.ok)
        throw dispatch_error{err_code::service_unavailable, result.message, "profiler_unavailable"};
      json out{json::object()};
      try {
        out["profile"] = json::parse(result.profile_json);
      } catch (const nlohmann::json::parse_error&) {
        throw dispatch_error{err_code::internal, "CPU profile serialization returned invalid JSON",
                             ""};
      }
      return out;
    }

    json heap_profiler_enable(dispatch_context&, const json&) {
      return json{json::object()};
    }

    json heap_profiler_disable(dispatch_context&, const json&) {
      return json{json::object()};
    }

    json heap_profiler_collect_garbage(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      cx.host->debug_collect_garbage();
      return json{json::object()};
    }

    json heap_profiler_take_heap_snapshot(dispatch_context& cx, const json&) {
      if (!cx.host)
        no_host();
      cx.host->debug_take_heap_snapshot([srv = cx.srv](std::string_view chunk) {
        if (!srv)
          return;
        json params{json::object()};
        params["chunk"] = std::string(chunk);
        srv->emit_event("HeapProfiler.addHeapSnapshotChunk", std::move(params));
      });
      if (cx.srv) {
        json progress{json::object()};
        progress["done"] = 1.0;
        progress["total"] = 1.0;
        progress["finished"] = true;
        cx.srv->emit_event("HeapProfiler.reportHeapSnapshotProgress", std::move(progress));
      }
      return json{json::object()};
    }

    v8::Local<v8::String> v8_string(v8::Isolate* iso, std::string_view s) {
      return v8::String::NewFromUtf8(iso, s.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string to_std_string(v8::Isolate* iso, v8::Local<v8::Value> value) {
      if (value.IsEmpty())
        return {};
      v8::String::Utf8Value utf8(iso, value);
      if (!*utf8)
        return {};
      return std::string(*utf8, utf8.length());
    }

    void throw_type_error(v8::Isolate* iso, std::string_view message) {
      iso->ThrowException(v8::Exception::TypeError(v8_string(iso, message)));
    }

    bool hmr_path_arg(const v8::FunctionCallbackInfo<v8::Value>& info, std::string& out) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_type_error(iso, "__fxe_native.hmr expects a module path string");
        return false;
      }
      out = to_std_string(iso, info[0]);
      return true;
    }

    void hmr_invalidate_callback(const v8::FunctionCallbackInfo<v8::Value>& info) {
      auto* iso = info.GetIsolate();
      v8::HandleScope hs(iso);
      std::string path;
      if (!hmr_path_arg(info, path))
        return;
      std::string error;
      auto evicted = invalidate_module_for_isolate(iso, path, error);
      if (!error.empty()) {
        iso->ThrowException(v8::Exception::Error(v8_string(iso, error)));
        return;
      }
      auto ctx = iso->GetCurrentContext();
      auto out = v8::Array::New(iso, static_cast<int>(evicted.size()));
      for (uint32_t i = 0; i < evicted.size(); ++i)
        (void)out->Set(ctx, i, v8_string(iso, evicted[i]));
      info.GetReturnValue().Set(out);
    }

    void hmr_reimport_callback(const v8::FunctionCallbackInfo<v8::Value>& info) {
      auto* iso = info.GetIsolate();
      v8::HandleScope hs(iso);
      std::string path;
      if (!hmr_path_arg(info, path))
        return;
      auto ctx = iso->GetCurrentContext();
      std::string error;
      v8::Local<v8::Value> module_namespace;
      if (!reimport_module_for_isolate(iso, ctx, path, error).ToLocal(&module_namespace)) {
        if (error.empty())
          error = "HMR module reimport failed";
        iso->ThrowException(v8::Exception::Error(v8_string(iso, error)));
        return;
      }
      info.GetReturnValue().Set(module_namespace);
    }

  } // namespace

  void install_hmr_native_bindings(v8::Isolate* iso, v8::Local<v8::Context> ctx) {
    auto global = ctx->Global();
    v8::Local<v8::Value> native_value;
    v8::Local<v8::Object> native;
    auto native_key = v8_string(iso, "__fxe_native");
    if (global->Get(ctx, native_key).ToLocal(&native_value) && native_value->IsObject()) {
      native = native_value.As<v8::Object>();
    } else {
      native = v8::Object::New(iso);
      (void)global->DefineOwnProperty(ctx, native_key, native,
                                      static_cast<v8::PropertyAttribute>(v8::DontEnum));
    }

    auto hmr = v8::Object::New(iso);
    (void)hmr->Set(ctx, v8_string(iso, "invalidate"),
                   v8::Function::New(ctx, hmr_invalidate_callback).ToLocalChecked());
    (void)hmr->Set(ctx, v8_string(iso, "reimport"),
                   v8::Function::New(ctx, hmr_reimport_callback).ToLocalChecked());
    (void)native->Set(ctx, v8_string(iso, "hmr"), hmr);
  }

  // Called by host::host(). The static-init pattern is unreliable for
  // archive-only consumers (the linker drops TUs whose external symbols are
  // unreferenced), so we expose an explicit anchor that v8_host.cpp invokes.
  void install_runtime_dispatch_handlers() {
    using namespace fxe::debug;
    runtime_handlers h{};
    h.evaluate = &runtime_evaluate;
    h.get_globals = &runtime_get_globals;
    h.fire_hmr = &runtime_fire_hmr;
    h.invalidate_module = &runtime_invalidate_module;
    h.reimport_module = &runtime_reimport_module;
    h.reconciler_snapshot = &runtime_reconciler_snapshot;

    set_runtime_handlers(h);

    profiler_handlers profiler{};
    profiler.enable = &profiler_enable;
    profiler.disable = &profiler_disable;
    profiler.start = &profiler_start;
    profiler.stop = &profiler_stop;
    set_profiler_handlers(std::move(profiler));
    heap_profiler_handlers heap{};
    heap.enable = &heap_profiler_enable;
    heap.disable = &heap_profiler_disable;
    heap.take_heap_snapshot = &heap_profiler_take_heap_snapshot;
    heap.collect_garbage = &heap_profiler_collect_garbage;
    set_heap_profiler_handlers(std::move(heap));
  }
} // namespace fxe::js
