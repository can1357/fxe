// Match brew V8 ABI: pointer compression + sandbox are enabled in libv8.dylib.
#define V8_COMPRESS_POINTERS 1

// JS bindings to install WebAssembly streaming compilation. V8 already
// exposes the synchronous WebAssembly.* surface — compile, instantiate,
// validate, Module, Instance, Memory, Table, Global, CompileError,
// LinkError, RuntimeError — but `WebAssembly.compileStreaming` and
// `WebAssembly.instantiateStreaming` require an embedder hook via
// `Isolate::SetWasmStreamingCallback`. This file implements the spec-
// mandated steps:
//   1. Coerce the argument to a Promise<Response> via `Promise.resolve`.
//   2. Validate `response.ok` and the `Content-Type` essence is
//      `application/wasm` (case-insensitive, parameters ignored).
//   3. Drain `response.arrayBuffer()` and feed bytes to V8's
//      `WasmStreaming` pipeline, then `Finish`.
//   4. On any rejection, `Abort` with the rejection reason so V8 propagates
//      it to the user-facing promise.
//
// The Response contract is duck-typed (any object exposing `.ok`,
// `.headers.get(name)`, `.arrayBuffer()`, optional `.url`) so polyfills,
// mocked responses in tests, and our own native fxe Response all work
// without coupling this binding to fetch internals. V8 reuses the same
// callback for both compileStreaming and instantiateStreaming; once the
// resulting CompiledModule is ready, V8 instantiates internally when the
// JS entry point was instantiateStreaming.

#include "bind_wasm.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    // Per-streaming-call heap state. Owned by the External attached to each
    // promise reaction; freed exactly once at the terminal step (Finish or
    // Abort). The shared_ptr keeps V8's WasmStreaming alive across the
    // promise chain.
    struct streaming_state {
      std::shared_ptr<WasmStreaming> streaming;
    };

    void terminate(streaming_state* st) {
      delete st;
    }

    Local<String> s8(Isolate* iso, const char* s) {
      return String::NewFromUtf8(iso, s).ToLocalChecked();
    }

    std::string to_str(Isolate* iso, Local<Value> v) {
      Local<String> s;
      if (!v->ToString(iso->GetCurrentContext()).ToLocal(&s))
        return {};
      String::Utf8Value u(iso, s);
      return std::string(*u ? *u : "", *u ? u.length() : 0);
    }

    Local<Value> type_error(Isolate* iso, const char* msg) {
      return Exception::TypeError(s8(iso, msg));
    }

    // Match the WebAssembly Web API MIME check: essence (substring before
    // any ';') equals "application/wasm" with ASCII-case-insensitive
    // comparison; surrounding whitespace ignored.
    bool mime_is_wasm(const std::string& ct) {
      std::string s;
      s.reserve(ct.size());
      for (char c : ct) {
        if (c == ';')
          break;
        s.push_back(c);
      }
      auto issp = [](char c) { return c == ' ' || c == '\t'; };
      while (!s.empty() && issp(s.front()))
        s.erase(s.begin());
      while (!s.empty() && issp(s.back()))
        s.pop_back();
      for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c + 32);
      }
      return s == "application/wasm";
    }

    streaming_state* state_from(const FunctionCallbackInfo<Value>& info) {
      auto data = info.Data().As<External>();
      return static_cast<streaming_state*>(data->Value(v8::kExternalPointerTypeTagDefault));
    }

    void on_bytes_resolved(const FunctionCallbackInfo<Value>& info);
    void on_bytes_rejected(const FunctionCallbackInfo<Value>& info);
    void on_response_resolved(const FunctionCallbackInfo<Value>& info);
    void on_response_rejected(const FunctionCallbackInfo<Value>& info);

    void on_response_resolved(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* st = state_from(info);

      if (info.Length() < 1 || !info[0]->IsObject()) {
        st->streaming->Abort(type_error(iso, "WebAssembly.compileStreaming: argument did not "
                                             "resolve to a Response"));
        terminate(st);
        return;
      }
      auto resp = info[0].As<Object>();

      // response.ok
      Local<Value> okv;
      if (!resp->Get(ctx, s8(iso, "ok")).ToLocal(&okv) || !okv->BooleanValue(iso)) {
        st->streaming->Abort(type_error(iso, "WebAssembly.compileStreaming: response is not ok"));
        terminate(st);
        return;
      }

      // response.headers.get("Content-Type") -> mime essence == application/wasm
      Local<Value> hv;
      if (!resp->Get(ctx, s8(iso, "headers")).ToLocal(&hv) || !hv->IsObject()) {
        st->streaming->Abort(
            type_error(iso, "WebAssembly.compileStreaming: response missing headers"));
        terminate(st);
        return;
      }
      auto headers = hv.As<Object>();
      Local<Value> get_fn;
      if (!headers->Get(ctx, s8(iso, "get")).ToLocal(&get_fn) || !get_fn->IsFunction()) {
        st->streaming->Abort(
            type_error(iso, "WebAssembly.compileStreaming: response.headers.get not callable"));
        terminate(st);
        return;
      }
      Local<Value> ct_arg = s8(iso, "Content-Type");
      Local<Value> ct_v;
      {
        TryCatch tc(iso);
        if (!get_fn.As<Function>()->Call(ctx, headers, 1, &ct_arg).ToLocal(&ct_v)) {
          Local<Value> reason = tc.HasCaught() ? tc.Exception()
                                               : type_error(iso, "WebAssembly.compileStreaming: "
                                                                 "headers.get threw");
          st->streaming->Abort(reason);
          terminate(st);
          return;
        }
      }
      std::string ct = ct_v->IsString() ? to_str(iso, ct_v) : "";
      if (!mime_is_wasm(ct)) {
        st->streaming->Abort(type_error(
            iso, "WebAssembly.compileStreaming: invalid MIME type, expected application/wasm"));
        terminate(st);
        return;
      }

      // Optional source URL for stack traces.
      Local<Value> urlv;
      if (resp->Get(ctx, s8(iso, "url")).ToLocal(&urlv) && urlv->IsString()) {
        std::string url = to_str(iso, urlv);
        st->streaming->SetUrl(url.c_str(), url.size());
      }

      // Chain into response.arrayBuffer().
      Local<Value> ab_fn;
      if (!resp->Get(ctx, s8(iso, "arrayBuffer")).ToLocal(&ab_fn) || !ab_fn->IsFunction()) {
        st->streaming->Abort(
            type_error(iso, "WebAssembly.compileStreaming: response.arrayBuffer not callable"));
        terminate(st);
        return;
      }
      Local<Value> ab_promise_v;
      {
        TryCatch tc(iso);
        if (!ab_fn.As<Function>()->Call(ctx, resp, 0, nullptr).ToLocal(&ab_promise_v)) {
          Local<Value> reason = tc.HasCaught() ? tc.Exception()
                                               : type_error(iso, "WebAssembly.compileStreaming: "
                                                                 "arrayBuffer threw");
          st->streaming->Abort(reason);
          terminate(st);
          return;
        }
      }
      // arrayBuffer may be sync (returns ArrayBuffer) or async (returns Promise);
      // Promise.resolve() unifies both paths.
      auto wrap_resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      wrap_resolver->Resolve(ctx, ab_promise_v).Check();
      auto p = wrap_resolver->GetPromise();

      auto data = External::New(iso, st, v8::kExternalPointerTypeTagDefault);
      auto on_ok = Function::New(ctx, on_bytes_resolved, data).ToLocalChecked();
      auto on_err = Function::New(ctx, on_bytes_rejected, data).ToLocalChecked();
      (void)p->Then(ctx, on_ok, on_err);
    }

    void on_response_rejected(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* st = state_from(info);
      Local<Value> reason = info.Length() >= 1
                                ? info[0]
                                : type_error(iso, "WebAssembly.compileStreaming: source rejected");
      st->streaming->Abort(reason);
      terminate(st);
    }

    void on_bytes_resolved(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* st = state_from(info);

      if (info.Length() < 1) {
        st->streaming->Abort(
            type_error(iso, "WebAssembly.compileStreaming: empty arrayBuffer result"));
        terminate(st);
        return;
      }
      Local<Value> v = info[0];
      const uint8_t* data = nullptr;
      size_t size = 0;
      std::shared_ptr<BackingStore> bs;
      if (v->IsArrayBuffer()) {
        auto ab = v.As<ArrayBuffer>();
        bs = ab->GetBackingStore();
        data = static_cast<const uint8_t*>(bs->Data());
        size = bs->ByteLength();
      } else if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        bs = view->Buffer()->GetBackingStore();
        data = static_cast<const uint8_t*>(bs->Data()) + view->ByteOffset();
        size = view->ByteLength();
      } else {
        st->streaming->Abort(type_error(
            iso, "WebAssembly.compileStreaming: arrayBuffer did not resolve to a BufferSource"));
        terminate(st);
        return;
      }

      if (size > 0)
        st->streaming->OnBytesReceived(data, size);
      st->streaming->Finish({});
      terminate(st);
    }

    void on_bytes_rejected(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* st = state_from(info);
      Local<Value> reason =
          info.Length() >= 1
              ? info[0]
              : type_error(iso, "WebAssembly.compileStreaming: arrayBuffer rejected");
      st->streaming->Abort(reason);
      terminate(st);
    }

    // V8 invokes this for both `WebAssembly.compileStreaming(arg)` and
    // `WebAssembly.instantiateStreaming(arg, importObject)`. The streaming
    // object lives in info.Data() per V8's API contract.
    void wasm_streaming_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto streaming = WasmStreaming::Unpack(iso, info.Data());
      auto* st = new streaming_state{std::move(streaming)};

      if (info.Length() < 1) {
        st->streaming->Abort(type_error(iso, "WebAssembly.compileStreaming: missing argument"));
        terminate(st);
        return;
      }

      // Coerce arg to Promise<Response> via Promise.resolve(); spec accepts
      // Response or Promise<Response>.
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      resolver->Resolve(ctx, info[0]).Check();
      auto p = resolver->GetPromise();

      auto data = External::New(iso, st, v8::kExternalPointerTypeTagDefault);
      auto on_ok = Function::New(ctx, on_response_resolved, data).ToLocalChecked();
      auto on_err = Function::New(ctx, on_response_rejected, data).ToLocalChecked();
      (void)p->Then(ctx, on_ok, on_err);
    }
  } // namespace

  void install_wasm_streaming(Isolate* iso) {
    iso->SetWasmStreamingCallback(wasm_streaming_callback);
  }
} // namespace fxe::js
