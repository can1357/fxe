#include "tls.hpp"

#include "net/tls_client.hpp"
#include "net/tls_server.hpp"
#include "os/os.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <fxe/types.hpp>
#include <fxe/v8_strings.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fxe::runtime {
  namespace {
    using namespace v8;

    struct secure_context_state {
      std::string ca_pem;
      std::string cert_pem;
      std::string key_pem;
    };

    struct tls_socket_state {
      int handle = 0;
      Isolate* isolate = nullptr;
      Global<Context> context;
      Global<Function> on_connect;
      Global<Function> on_error;
      Global<Function> on_data;
      Global<Function> on_close;
      std::mutex client_mutex;
      std::shared_ptr<fxe::net::tls_client> client;
      std::atomic<bool> closed{false};
      std::atomic<bool> close_notified{false};
    };

    std::mutex g_tls_mutex;
    int g_next_tls_handle = 1;
    int g_next_secure_context_handle = 1;
    std::unordered_map<int, std::shared_ptr<tls_socket_state>> g_tls_sockets;
    std::unordered_map<int, secure_context_state> g_secure_contexts;

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      FunctionCallback cb) {
      auto fn = Function::New(ctx, cb).ToLocalChecked();
      (void)obj->Set(ctx, str(iso, name), fn);
    }

    bool get_property(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      Local<Value>& out) {
      return obj->Get(ctx, str(iso, name)).ToLocal(&out) && !out->IsUndefined() && !out->IsNull();
    }

    std::string value_to_string(Isolate* iso, Local<Context> ctx, Local<Value> value) {
      if (value.IsEmpty() || value->IsUndefined() || value->IsNull())
        return {};
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        const auto* bytes = static_cast<const char*>(backing->Data()) + view->ByteOffset();
        return std::string(bytes, bytes + view->ByteLength());
      }
      if (value->IsArrayBuffer()) {
        auto backing = value.As<ArrayBuffer>()->GetBackingStore();
        const auto* bytes = static_cast<const char*>(backing->Data());
        return std::string(bytes, bytes + backing->ByteLength());
      }
      if (value->IsArray()) {
        auto arr = value.As<Array>();
        std::string out;
        for (u32 i = 0; i < arr->Length(); ++i) {
          Local<Value> item;
          if (!arr->Get(ctx, i).ToLocal(&item))
            continue;
          auto part = value_to_string(iso, ctx, item);
          if (part.empty())
            continue;
          if (!out.empty())
            out.push_back('\n');
          out.append(part);
        }
        return out;
      }
      String::Utf8Value utf8(iso, value);
      return *utf8 ? std::string(*utf8, static_cast<usize>(utf8.length())) : std::string{};
    }

    std::string string_option(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                              const char* name) {
      Local<Value> value;
      if (!get_property(iso, ctx, obj, name, value))
        return {};
      return value_to_string(iso, ctx, value);
    }

    bool bool_option(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                     bool fallback) {
      Local<Value> value;
      if (!get_property(iso, ctx, obj, name, value))
        return fallback;
      return value->BooleanValue(iso);
    }

    u16 port_option(Isolate* iso, Local<Context> ctx, Local<Object> obj) {
      Local<Value> value;
      if (!get_property(iso, ctx, obj, "port", value))
        return 443;
      const int port = value->Int32Value(ctx).FromMaybe(443);
      if (port <= 0 || port > 65535)
        return 443;
      return static_cast<u16>(port);
    }

    std::vector<std::string> string_list_option(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                                const char* name) {
      Local<Value> value;
      if (!get_property(iso, ctx, obj, name, value))
        return {};
      if (!value->IsArray()) {
        auto single = value_to_string(iso, ctx, value);
        if (single.empty())
          return {};
        return {single};
      }
      auto arr = value.As<Array>();
      std::vector<std::string> out;
      out.reserve(arr->Length());
      for (u32 i = 0; i < arr->Length(); ++i) {
        Local<Value> item;
        if (!arr->Get(ctx, i).ToLocal(&item))
          continue;
        auto text = value_to_string(iso, ctx, item);
        if (!text.empty())
          out.push_back(std::move(text));
      }
      return out;
    }

    secure_context_state secure_context_from_options(Isolate* iso, Local<Context> ctx,
                                                     Local<Object> options) {
      secure_context_state state;
      state.ca_pem = string_option(iso, ctx, options, "ca");
      state.cert_pem = string_option(iso, ctx, options, "cert");
      state.key_pem = string_option(iso, ctx, options, "key");
      return state;
    }

    fxe::net::tls_options tls_options_from_js(Isolate* iso, Local<Context> ctx,
                                              Local<Object> options) {
      fxe::net::tls_options out;
      out.host = string_option(iso, ctx, options, "host");
      if (out.host.empty())
        out.host = string_option(iso, ctx, options, "hostname");
      if (out.host.empty())
        out.host = "localhost";
      out.port = port_option(iso, ctx, options);
      out.ca_pem = string_option(iso, ctx, options, "ca");
      out.reject_unauthorized = bool_option(iso, ctx, options, "rejectUnauthorized", true);
      out.alpn = string_list_option(iso, ctx, options, "ALPNProtocols");
      return out;
    }

    std::shared_ptr<tls_socket_state> lookup_socket(int handle) {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      auto it = g_tls_sockets.find(handle);
      if (it == g_tls_sockets.end())
        return nullptr;
      return it->second;
    }

    void erase_socket(int handle) {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      g_tls_sockets.erase(handle);
    }

    void reset_callbacks(const std::shared_ptr<tls_socket_state>& state) {
      state->on_connect.Reset();
      state->on_error.Reset();
      state->on_data.Reset();
      state->on_close.Reset();
      state->context.Reset();
    }

    void with_js(const std::shared_ptr<tls_socket_state>& state,
                 const std::function<void(Isolate*, Local<Context>)>& fn) {
      Isolate* iso = state->isolate;
      Isolate::Scope isolate_scope(iso);
      HandleScope handle_scope(iso);
      auto ctx = state->context.Get(iso);
      if (ctx.IsEmpty())
        return;
      Context::Scope context_scope(ctx);
      fn(iso, ctx);
    }

    void dispatch_error(const std::shared_ptr<tls_socket_state>& state, std::string message) {
      fxe::os::post_main_thread_dispatch([state, message = std::move(message)] {
        with_js(state, [&](Isolate* iso, Local<Context> ctx) {
          auto fn = state->on_error.Get(iso);
          if (fn.IsEmpty())
            return;
          Local<Value> argv[] = {str(iso, message)};
          Local<Value> ignored;
          (void)fn->Call(ctx, Undefined(iso), 1, argv).ToLocal(&ignored);
        });
      });
    }

    void dispatch_close(const std::shared_ptr<tls_socket_state>& state) {
      if (state->close_notified.exchange(true))
        return;
      state->closed.store(true);
      erase_socket(state->handle);
      fxe::os::post_main_thread_dispatch([state] {
        with_js(state, [&](Isolate* iso, Local<Context> ctx) {
          auto fn = state->on_close.Get(iso);
          if (!fn.IsEmpty()) {
            Local<Value> ignored;
            (void)fn->Call(ctx, Undefined(iso), 0, nullptr).ToLocal(&ignored);
          }
          reset_callbacks(state);
        });
      });
    }

    void dispatch_connect(const std::shared_ptr<tls_socket_state>& state, std::string alpn,
                          std::string subject, std::string subject_error) {
      fxe::os::post_main_thread_dispatch([state, alpn = std::move(alpn),
                                          subject = std::move(subject),
                                          subject_error = std::move(subject_error)] {
        if (state->closed.load())
          return;
        with_js(state, [&](Isolate* iso, Local<Context> ctx) {
          auto fn = state->on_connect.Get(iso);
          if (fn.IsEmpty())
            return;
          auto info = Object::New(iso);
          (void)info->Set(ctx, "alpnProtocol"_v8(iso), str(iso, alpn));
          (void)info->Set(ctx, "peerCertificateSubject"_v8(iso), str(iso, subject));
          if (!subject_error.empty())
            (void)info->Set(ctx, "peerCertificateError"_v8(iso), str(iso, subject_error));
          Local<Value> argv[] = {info};
          Local<Value> ignored;
          (void)fn->Call(ctx, Undefined(iso), 1, argv).ToLocal(&ignored);
        });
      });
    }

    void dispatch_data(const std::shared_ptr<tls_socket_state>& state, std::vector<u8> data) {
      fxe::os::post_main_thread_dispatch([state, data = std::move(data)] {
        if (state->closed.load())
          return;
        with_js(state, [&](Isolate* iso, Local<Context> ctx) {
          auto fn = state->on_data.Get(iso);
          if (fn.IsEmpty())
            return;
          auto buffer = ArrayBuffer::New(iso, data.size());
          if (!data.empty())
            std::memcpy(buffer->GetBackingStore()->Data(), data.data(), data.size());
          auto view = Uint8Array::New(buffer, 0, data.size());
          Local<Value> argv[] = {view};
          Local<Value> ignored;
          (void)fn->Call(ctx, Undefined(iso), 1, argv).ToLocal(&ignored);
        });
      });
    }

    void read_loop(std::shared_ptr<tls_socket_state> state) {
      std::vector<u8> buf(16 * 1024);
      while (!state->closed.load()) {
        std::shared_ptr<fxe::net::tls_client> client;
        {
          std::lock_guard<std::mutex> lock(state->client_mutex);
          client = state->client;
        }
        if (!client) {
          dispatch_error(state, "TLS socket closed before read");
          dispatch_close(state);
          return;
        }
        const auto n = client->read(buf.data(), buf.size());
        if (state->closed.load()) {
          dispatch_close(state);
          return;
        }
        if (n > 0) {
          dispatch_data(state, std::vector<u8>(buf.begin(), buf.begin() + n));
          continue;
        }
        if (n < 0)
          dispatch_error(state, client->last_error().empty() ? "TLS socket read failed"
                                                             : client->last_error());
        dispatch_close(state);
        return;
      }
    }

    bool copy_bytes(Isolate* iso, Local<Context> ctx, Local<Value> value, std::vector<u8>& out) {
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        const auto* bytes = static_cast<const u8*>(backing->Data()) + view->ByteOffset();
        out.assign(bytes, bytes + view->ByteLength());
        return true;
      }
      if (value->IsArrayBuffer()) {
        auto backing = value.As<ArrayBuffer>()->GetBackingStore();
        const auto* bytes = static_cast<const u8*>(backing->Data());
        out.assign(bytes, bytes + backing->ByteLength());
        return true;
      }
      auto text = value_to_string(iso, ctx, value);
      out.assign(text.begin(), text.end());
      return true;
    }

    void tls_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 5 || !info[0]->IsObject() || !info[1]->IsFunction() ||
          !info[2]->IsFunction() || !info[3]->IsFunction() || !info[4]->IsFunction()) {
        iso->ThrowException(Exception::TypeError(
            "__fxe_native.tls.connect(options, onConnect, onError, onData, onClose) required"_v8(
                iso)));
        return;
      }

      auto state = std::make_shared<tls_socket_state>();
      state->isolate = iso;
      state->context.Reset(iso, ctx);
      state->on_connect.Reset(iso, info[1].As<Function>());
      state->on_error.Reset(iso, info[2].As<Function>());
      state->on_data.Reset(iso, info[3].As<Function>());
      state->on_close.Reset(iso, info[4].As<Function>());
      const auto options = tls_options_from_js(iso, ctx, info[0].As<Object>());
      {
        std::lock_guard<std::mutex> lock(g_tls_mutex);
        state->handle = g_next_tls_handle++;
        g_tls_sockets[state->handle] = state;
      }

      std::thread([state, options] {
        std::string err;
        auto raw = fxe::net::tls_client::connect(options, err);
        if (state->closed.load()) {
          dispatch_close(state);
          return;
        }
        if (!raw) {
          dispatch_error(state, err.empty() ? "TLS connection failed" : err);
          dispatch_close(state);
          return;
        }
        std::shared_ptr<fxe::net::tls_client> client(std::move(raw));
        if (state->closed.load()) {
          client->close();
          dispatch_close(state);
          return;
        }
        const auto alpn = client->negotiated_alpn();
        const auto subject = client->peer_cert_subject();
        const std::string subject_error = subject ? std::string{} : client->last_error();
        {
          std::lock_guard<std::mutex> lock(state->client_mutex);
          state->client = client;
        }
        dispatch_connect(state, alpn, subject.value_or(std::string{}), subject_error);
        std::thread(read_loop, state).detach();
      }).detach();

      info.GetReturnValue().Set(Integer::New(iso, state->handle));
    }

    void tls_write(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsNumber()) {
        iso->ThrowException(
            Exception::TypeError("__fxe_native.tls.write(handle, data) required"_v8(iso)));
        return;
      }
      std::vector<u8> bytes;
      if (!copy_bytes(iso, ctx, info[1], bytes)) {
        iso->ThrowException(
            Exception::TypeError("__fxe_native.tls.write data must be bytes"_v8(iso)));
        return;
      }
      auto state = lookup_socket(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!state || state->closed.load()) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      std::thread([state, bytes = std::move(bytes)] {
        std::shared_ptr<fxe::net::tls_client> client;
        {
          std::lock_guard<std::mutex> lock(state->client_mutex);
          client = state->client;
        }
        if (!client || state->closed.load())
          return;
        const auto n = client->write(bytes.data(), bytes.size());
        if (n < 0) {
          dispatch_error(state, client->last_error().empty() ? "TLS socket write failed"
                                                             : client->last_error());
          dispatch_close(state);
        }
      }).detach();
      info.GetReturnValue().Set(True(iso));
    }

    void tls_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsNumber())
        return;
      auto state = lookup_socket(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!state)
        return;
      state->closed.store(true);
      std::shared_ptr<fxe::net::tls_client> client;
      {
        std::lock_guard<std::mutex> lock(state->client_mutex);
        client = state->client;
      }
      if (client)
        client->close();
      dispatch_close(state);
    }

    void tls_create_secure_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      secure_context_state state;
      if (info.Length() > 0 && info[0]->IsObject())
        state = secure_context_from_options(iso, ctx, info[0].As<Object>());
      int handle = 0;
      {
        std::lock_guard<std::mutex> lock(g_tls_mutex);
        handle = g_next_secure_context_handle++;
        g_secure_contexts[handle] = std::move(state);
      }
      info.GetReturnValue().Set(Integer::New(iso, handle));
    }

    void tls_root_certificates(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      info.GetReturnValue().Set(Array::New(iso));
    }

    Local<Object> make_tls_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "connect", tls_connect);
      add_function(iso, ctx, ns, "write", tls_write);
      add_function(iso, ctx, ns, "close", tls_close);
      add_function(iso, ctx, ns, "createSecureContext", tls_create_secure_context);
      add_function(iso, ctx, ns, "rootCertificates", tls_root_certificates);
      return ns;
    }
  } // namespace

  void install_native_tls(Isolate* iso, Local<Context> ctx) {
    Local<Value> native_value;
    Local<Object> native;
    if (ctx->Global()->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) &&
        native_value->IsObject()) {
      native = native_value.As<Object>();
    } else {
      native = Object::New(iso);
      (void)ctx->Global()->DefineOwnProperty(ctx, "__fxe_native"_v8(iso), native,
                                             static_cast<PropertyAttribute>(DontEnum));
    }
    (void)native->Set(ctx, "tls"_v8(iso), make_tls_namespace(iso, ctx));
  }

} // namespace fxe::runtime
