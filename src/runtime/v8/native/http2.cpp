#include "runtime/v8/native/http2.hpp"

#include "debug/dispatch.hpp"
#include "net/http2_client.hpp"
#include "net/http2_server.hpp"
#include <cctype>

#include <cstdint>
#include <cstring>
#include <fxe/string_utils.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <v8.h>
#include <vector>

namespace fxe::runtime {
  namespace {
    using namespace v8;
    using namespace fxe::js;

    std::string string_arg(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      if (*utf8 == nullptr)
        return {};
      return std::string(*utf8, static_cast<usize>(utf8.length()));
    }

    std::string string_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                            std::string fallback = {}) {
      auto value = get_prop<Local<Value>>(ctx, obj, key);
      if (!value || (*value)->IsNullOrUndefined())
        return fallback;
      return string_arg(iso, *value);
    }

    int int_prop(Local<Context> ctx, Local<Object> obj, const char* key, int fallback) {
      auto value = get_prop<Local<Value>>(ctx, obj, key);
      if (!value || (*value)->IsNullOrUndefined())
        return fallback;
      return (*value)->Int32Value(ctx).FromMaybe(fallback);
    }

    bool bool_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                   bool fallback) {
      auto value = get_prop<Local<Value>>(ctx, obj, key);
      if (!value || (*value)->IsNullOrUndefined())
        return fallback;
      return (*value)->BooleanValue(iso);
    }

    bool optional_uint32_prop(Local<Context> ctx, Local<Object> obj, const char* key,
                              std::optional<u32>& out, std::string& err) {
      auto value = get_prop<Local<Value>>(ctx, obj, key);
      if (!value || (*value)->IsNullOrUndefined())
        return true;
      const auto number = (*value)->IntegerValue(ctx);
      if (number.IsNothing() || number.FromJust() < 0 || number.FromJust() > UINT32_MAX) {
        err = std::string("invalid HTTP/2 setting ") + key;
        return false;
      }
      out = static_cast<u32>(number.FromJust());
      return true;
    }

    std::optional<fxe::net::http2_settings> settings_prop(Local<Context> ctx, Local<Object> options,
                                                          std::string& err) {
      auto settings_value = get_prop<Local<Value>>(ctx, options, "settings");
      if (!settings_value || (*settings_value)->IsNullOrUndefined())
        return fxe::net::http2_settings{};
      if (!(*settings_value)->IsObject()) {
        err = "HTTP/2 settings must be an object";
        return std::nullopt;
      }
      auto settings_obj = (*settings_value).As<Object>();
      fxe::net::http2_settings settings;
      if (!optional_uint32_prop(ctx, settings_obj, "headerTableSize", settings.header_table_size,
                                err) ||
          !optional_uint32_prop(ctx, settings_obj, "header_table_size", settings.header_table_size,
                                err) ||
          !optional_uint32_prop(ctx, settings_obj, "enablePush", settings.enable_push, err) ||
          !optional_uint32_prop(ctx, settings_obj, "enable_push", settings.enable_push, err) ||
          !optional_uint32_prop(ctx, settings_obj, "maxConcurrentStreams",
                                settings.max_concurrent_streams, err) ||
          !optional_uint32_prop(ctx, settings_obj, "max_concurrent_streams",
                                settings.max_concurrent_streams, err) ||
          !optional_uint32_prop(ctx, settings_obj, "initialWindowSize",
                                settings.initial_window_size, err) ||
          !optional_uint32_prop(ctx, settings_obj, "initial_window_size",
                                settings.initial_window_size, err) ||
          !optional_uint32_prop(ctx, settings_obj, "maxFrameSize", settings.max_frame_size, err) ||
          !optional_uint32_prop(ctx, settings_obj, "max_frame_size", settings.max_frame_size,
                                err) ||
          !optional_uint32_prop(ctx, settings_obj, "maxHeaderListSize",
                                settings.max_header_list_size, err) ||
          !optional_uint32_prop(ctx, settings_obj, "max_header_list_size",
                                settings.max_header_list_size, err))
        return std::nullopt;
      return settings;
    }

    std::string bytes_value(Isolate* iso, Local<Context> ctx, Local<Value> value) {
      if (value->IsUndefined() || value->IsNull())
        return {};
      if (value->IsString())
        return string_arg(iso, value);
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        const auto* data = static_cast<const char*>(backing->Data()) + view->ByteOffset();
        return std::string(data, data + view->ByteLength());
      }
      if (value->IsArrayBuffer()) {
        auto buffer = value.As<ArrayBuffer>();
        auto backing = buffer->GetBackingStore();
        const auto* data = static_cast<const char*>(backing->Data());
        return std::string(data, data + backing->ByteLength());
      }
      (void)ctx;
      return string_arg(iso, value);
    }

    Local<Uint8Array> uint8_array(Isolate* iso, std::string_view bytes) {
      auto buffer = ArrayBuffer::New(iso, bytes.size());
      auto backing = buffer->GetBackingStore();
      if (!bytes.empty())
        std::memcpy(backing->Data(), bytes.data(), bytes.size());
      return Uint8Array::New(buffer, 0, bytes.size());
    }
    std::string header_value(const std::vector<std::pair<std::string, std::string>>& headers,
                             std::string_view lower_name) {
      for (const auto& [name, value] : headers) {
        if (ascii_lower(name) == lower_name)
          return value;
      }
      return {};
    }

    std::vector<std::pair<std::string, std::string>>
    headers_from_object(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                        std::initializer_list<std::string_view> skip_keys) {
      std::vector<std::pair<std::string, std::string>> out;
      auto names = obj->GetOwnPropertyNames(ctx).ToLocalChecked();
      for (u32 i = 0; i < names->Length(); ++i) {
        auto key_value = names->Get(ctx, i).ToLocalChecked();
        auto key = string_arg(iso, key_value);
        bool skip = false;
        for (std::string_view skip_key : skip_keys) {
          if (key == skip_key) {
            skip = true;
            break;
          }
        }
        if (skip)
          continue;
        auto value = obj->Get(ctx, key_value).ToLocalChecked();
        out.emplace_back(std::move(key), string_arg(iso, value));
      }
      return out;
    }

    std::string build_request_url(std::string_view authority, std::string_view path) {
      std::string out = "https://";
      out.append(authority.empty() ? "127.0.0.1" : authority);
      if (path.empty() || path.front() != '/')
        out.push_back('/');
      out.append(path.empty() ? "/" : path);
      return out;
    }

    struct debug_request_entry {
      std::string request_id;
      std::string url;
    };

    struct authority_parts {
      std::string host;
      u16 port = 443;
    };

    std::optional<authority_parts> parse_authority(std::string value) {
      auto scheme = value.find("://");
      if (scheme != std::string::npos)
        value = value.substr(scheme + 3);
      auto slash = value.find('/');
      if (slash != std::string::npos)
        value = value.substr(0, slash);
      auto at = value.rfind('@');
      if (at != std::string::npos)
        value = value.substr(at + 1);
      if (value.empty())
        return std::nullopt;
      authority_parts out;
      auto colon = value.rfind(':');
      if (colon != std::string::npos && value.find(']') == std::string::npos) {
        out.host = value.substr(0, colon);
        const auto port_value = value.substr(colon + 1);
        try {
          out.port = static_cast<u16>(std::stoi(port_value));
        } catch (...) {
          return std::nullopt;
        }
      } else {
        out.host = value;
      }
      if (out.host == "localhost")
        out.host = "127.0.0.1";
      return out;
    }

    struct client_entry {
      std::mutex mutex;
      std::mutex debug_mutex;
      std::unique_ptr<fxe::net::http2_client> client;
      std::string authority;
      std::map<i32, debug_request_entry> debug_requests;
    };

    struct server_entry {
      std::mutex mutex;
      std::unique_ptr<fxe::net::http2_server> server;
    };

    struct read_entry {
      std::mutex mutex;
      bool done = false;
      std::string err;
      std::string code;
      fxe::net::http2_response response;
      std::thread worker;
      ~read_entry() {
        // If the JS side never called readResult (e.g. session torn down on
        // exit while a stream was still pending), detach the worker so its
        // destructor doesn't terminate(). Joining is unsafe here because the
        // worker may still be blocked in nghttp2 IO.
        if (worker.joinable())
          worker.detach();
      }
    };

    std::mutex registry_mutex;
    int next_client_handle = 1;
    int next_server_handle = 1;
    int next_read_handle = 1;
    std::map<int, std::shared_ptr<client_entry>> clients;
    std::map<int, std::shared_ptr<server_entry>> servers;
    std::map<int, std::shared_ptr<read_entry>> reads;

    std::shared_ptr<client_entry> find_client(int handle) {
      std::lock_guard<std::mutex> lock(registry_mutex);
      auto it = clients.find(handle);
      return it == clients.end() ? nullptr : it->second;
    }

    std::shared_ptr<server_entry> find_server(int handle) {
      std::lock_guard<std::mutex> lock(registry_mutex);
      auto it = servers.find(handle);
      return it == servers.end() ? nullptr : it->second;
    }

    void http2_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.http2.connect requires an authority string");
        return;
      }
      auto parsed = parse_authority(string_arg(iso, info[0]));
      if (!parsed) {
        throw_error(iso, "invalid HTTP/2 authority");
        return;
      }
      bool reject_unauthorized = true;
      std::string ca_pem;
      fxe::net::http2_settings settings;
      std::string err;
      if (info.Length() > 1 && info[1]->IsObject()) {
        auto options = info[1].As<Object>();
        reject_unauthorized = bool_prop(iso, ctx, options, "rejectUnauthorized", true);
        ca_pem = string_prop(iso, ctx, options, "ca");
        auto parsed_settings = settings_prop(ctx, options, err);
        if (!parsed_settings) {
          throw_error(iso, err);
          return;
        }
        settings = *parsed_settings;
      }
      auto client = fxe::net::http2_client::connect(parsed->host, parsed->port, ca_pem,
                                                    reject_unauthorized, settings, err);
      if (!client) {
        throw_error(iso, err.empty() ? "HTTP/2 connect failed" : err);
        return;
      }
      auto entry = std::make_shared<client_entry>();
      entry->client = std::move(client);
      entry->authority =
          parsed->host + (parsed->port == 443 ? std::string{} : ":" + std::to_string(parsed->port));
      int handle = 0;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        handle = next_client_handle++;
        clients[handle] = entry;
      }
      info.GetReturnValue().Set(Integer::New(iso, handle));
    }

    void http2_submit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsNumber() || !info[1]->IsObject()) {
        throw_error(iso, "__fxe_native.http2.submit requires handle and headers");
        return;
      }
      auto client = find_client(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!client) {
        throw_error(iso, "unknown HTTP/2 client handle");
        return;
      }
      auto headers = info[1].As<Object>();
      fxe::net::http2_request request;
      request.method = string_prop(iso, ctx, headers, ":method", "GET");
      request.path = string_prop(iso, ctx, headers, ":path", "/");
      auto body_value = get_prop<Local<Value>>(ctx, headers, "__body");
      request.body = bytes_value(iso, ctx, body_value ? *body_value : Undefined(iso));
      request.timeout_ms = int_prop(ctx, headers, "__timeoutMs", 0);
      auto debug_request_headers =
          headers_from_object(iso, ctx, headers, {"__body", "__timeoutMs"});
      request.headers =
          headers_from_object(iso, ctx, headers, {":method", ":path", "__body", "__timeoutMs"});
      const std::string authority = string_prop(iso, ctx, headers, ":authority", client->authority);
      const std::string request_url = build_request_url(authority, request.path);
      const std::string debug_request_id = fxe::debug::network::fresh_request_id();
      fxe::debug::network::emit_request_will_be_sent(
          debug_request_id, request_url, request.method, debug_request_headers,
          request.body.empty() ? std::optional<std::string_view>{}
                               : std::optional<std::string_view>{request.body},
          "XHR");
      i32 stream_id = 0;
      std::string submit_error;
      {
        std::lock_guard<std::mutex> lock(client->mutex);
        stream_id = client->client->submit(request);
        if (stream_id < 0)
          submit_error = client->client->last_error();
      }
      if (stream_id >= 0) {
        std::lock_guard<std::mutex> lock(client->debug_mutex);
        client->debug_requests.emplace(stream_id,
                                       debug_request_entry{debug_request_id, request_url});
      }
      if (stream_id < 0) {
        const std::string error_text =
            submit_error.empty() ? "HTTP/2 request submission failed" : submit_error;
        fxe::debug::network::emit_loading_failed(debug_request_id, "XHR", error_text, false);
        throw_error(iso, error_text);
        return;
      }
      info.GetReturnValue().Set(Integer::New(iso, stream_id));
    }

    void http2_cancel(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsNumber() || !info[1]->IsNumber()) {
        throw_error(iso, "__fxe_native.http2.cancel requires handle and stream id");
        return;
      }
      auto client = find_client(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!client) {
        throw_error(iso, "unknown HTTP/2 client handle");
        return;
      }
      const i32 stream_id = info[1]->Int32Value(ctx).FromMaybe(0);
      const u32 error_code = info.Length() > 2 && info[2]->IsUint32()
                                 ? info[2]->Uint32Value(ctx).FromMaybe(NGHTTP2_CANCEL)
                                 : NGHTTP2_CANCEL;
      {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->client->cancel(stream_id, error_code);
      }
      info.GetReturnValue().Set(True(iso));
    }

    void http2_read(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsNumber() || !info[1]->IsNumber()) {
        throw_error(iso, "__fxe_native.http2.read requires handle and stream id");
        return;
      }
      auto client = find_client(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!client) {
        throw_error(iso, "unknown HTTP/2 client handle");
        return;
      }
      const i32 stream_id = info[1]->Int32Value(ctx).FromMaybe(0);
      auto read = std::make_shared<read_entry>();
      debug_request_entry debug_request;
      bool have_debug_request = false;
      int handle = 0;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        handle = next_read_handle++;
        reads[handle] = read;
      }
      {
        std::lock_guard<std::mutex> lock(client->debug_mutex);
        auto it = client->debug_requests.find(stream_id);
        if (it != client->debug_requests.end()) {
          debug_request = it->second;
          have_debug_request = true;
        }
      }
      read->worker = std::thread([client, read, stream_id, debug_request = std::move(debug_request),
                                  have_debug_request] {
        std::string err;
        fxe::net::http2_response response;
        response = client->client->wait(stream_id, err);
        if (err.empty())
          err = client->client->last_error();
        {
          std::lock_guard<std::mutex> lock(client->debug_mutex);
          client->debug_requests.erase(stream_id);
        }
        if (have_debug_request) {
          if (!err.empty()) {
            const bool canceled = err == "ABORT_ERR";
            fxe::debug::network::emit_loading_failed(debug_request.request_id, "XHR", err,
                                                     canceled);
          } else {
            const i64 encoded_length = static_cast<i64>(response.body.size());
            fxe::debug::network::emit_response_received(
                debug_request.request_id, debug_request.url, response.status, "", response.headers,
                header_value(response.headers, "content-type"), "XHR", encoded_length);
            fxe::debug::network::emit_loading_finished(debug_request.request_id, encoded_length);
          }
        }
        {
          std::lock_guard<std::mutex> lock(read->mutex);
          read->response = std::move(response);
          read->err = std::move(err);
          if (read->err == "ABORT_ERR" || read->err == "ERR_HTTP2_STREAM_TIMEOUT")
            read->code = read->err;
          read->done = true;
        }
      });
      info.GetReturnValue().Set(Integer::New(iso, handle));
    }

    void http2_read_result(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsNumber()) {
        throw_error(iso, "__fxe_native.http2.readResult requires a read handle");
        return;
      }
      const int handle = info[0]->Int32Value(ctx).FromMaybe(0);
      std::shared_ptr<read_entry> read;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = reads.find(handle);
        if (it == reads.end()) {
          throw_error(iso, "unknown HTTP/2 read handle");
          return;
        }
        read = it->second;
      }
      {
        std::lock_guard<std::mutex> lock(read->mutex);
        if (!read->done) {
          info.GetReturnValue().Set(Null(iso));
          return;
        }
      }
      if (read->worker.joinable())
        read->worker.join();
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        reads.erase(handle);
      }
      if (!read->err.empty()) {
        auto out = Object::New(iso);
        set_prop(ctx, out, "ok", false);
        set_prop(ctx, out, "error", read->err);
        if (!read->code.empty())
          set_prop(ctx, out, "code", read->code);
        info.GetReturnValue().Set(out);
        return;
      }
      auto out = Object::New(iso);
      set_prop(ctx, out, "ok", true);
      set_prop(ctx, out, "status", read->response.status);
      auto headers = Object::New(iso);
      for (const auto& [name, value] : read->response.headers)
        set_prop(ctx, headers, name.c_str(), value);
      set_prop(ctx, out, "headers", headers);
      set_prop(ctx, out, "body", uint8_array(iso, read->response.body));
      info.GetReturnValue().Set(out);
    }

    void http2_write(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      usize size = 0;
      if (info.Length() > 2)
        size = bytes_value(iso, ctx, info[2]).size();
      info.GetReturnValue().Set(Integer::New(iso, static_cast<int>(size)));
    }

    void http2_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsNumber())
        return;
      const int handle = info[0]->Int32Value(ctx).FromMaybe(0);
      std::shared_ptr<client_entry> client;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = clients.find(handle);
        if (it != clients.end()) {
          client = it->second;
          clients.erase(it);
        }
      }
      if (client) {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->client->close();
      }
    }

    void http2_server_create(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        throw_error(iso, "__fxe_native.http2.createServer requires options");
        return;
      }
      auto options = info[0].As<Object>();
      fxe::net::http2_server_options server_options;
      server_options.cert_pem = string_prop(iso, ctx, options, "cert");
      server_options.key_pem = string_prop(iso, ctx, options, "key");
      server_options.port = static_cast<u16>(int_prop(ctx, options, "port", 0));
      std::string err;
      auto parsed_settings = settings_prop(ctx, options, err);
      if (!parsed_settings) {
        throw_error(iso, err);
        return;
      }
      server_options.settings = *parsed_settings;
      auto server = fxe::net::http2_server::listen(server_options, err);
      if (!server) {
        throw_error(iso, err.empty() ? "HTTP/2 server listen failed" : err);
        return;
      }
      auto entry = std::make_shared<server_entry>();
      entry->server = std::move(server);
      int handle = 0;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        handle = next_server_handle++;
        servers[handle] = entry;
      }
      info.GetReturnValue().Set(Integer::New(iso, handle));
    }

    void http2_server_address(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto server = info.Length() > 0 && info[0]->IsNumber()
                        ? find_server(info[0]->Int32Value(ctx).FromMaybe(0))
                        : nullptr;
      if (!server) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto out = Object::New(iso);
      std::lock_guard<std::mutex> lock(server->mutex);
      set_prop(ctx, out, "address", "127.0.0.1");
      set_prop(ctx, out, "family", "IPv4");
      set_prop(ctx, out, "port", server->server->local_port());
      info.GetReturnValue().Set(out);
    }

    void http2_server_poll(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto server = info.Length() > 0 && info[0]->IsNumber()
                        ? find_server(info[0]->Int32Value(ctx).FromMaybe(0))
                        : nullptr;
      if (!server) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      std::string err;
      std::optional<fxe::net::http2_incoming_request> request;
      {
        std::lock_guard<std::mutex> lock(server->mutex);
        request = server->server->poll(err);
      }
      if (!err.empty()) {
        const auto last_error = server->server->last_error();
        throw_error(iso, last_error.empty() ? err : last_error);
        return;
      }
      if (!request) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto out = Object::New(iso);
      set_prop(ctx, out, "id", static_cast<double>(request->id));
      set_prop(ctx, out, "streamId", request->stream_id);
      set_prop(ctx, out, "method", request->method);
      set_prop(ctx, out, "path", request->path);
      auto headers = Object::New(iso);
      for (const auto& [name, value] : request->headers)
        set_prop(ctx, headers, name.c_str(), value);
      set_prop(ctx, out, "headers", headers);
      set_prop(ctx, out, "body", uint8_array(iso, request->body));
      info.GetReturnValue().Set(out);
    }

    void http2_server_respond(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 4 || !info[0]->IsNumber() || !info[1]->IsNumber() ||
          !info[2]->IsObject()) {
        throw_error(iso, "__fxe_native.http2.serverRespond requires handle, id, headers, body");
        return;
      }
      auto server = find_server(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!server) {
        throw_error(iso, "unknown HTTP/2 server handle");
        return;
      }
      fxe::net::http2_response response;
      auto headers_obj = info[2].As<Object>();
      response.status = int_prop(ctx, headers_obj, ":status", 200);
      response.body = bytes_value(iso, ctx, info[3]);
      auto names = headers_obj->GetOwnPropertyNames(ctx).ToLocalChecked();
      for (u32 i = 0; i < names->Length(); ++i) {
        auto key_value = names->Get(ctx, i).ToLocalChecked();
        auto key = string_arg(iso, key_value);
        if (key == ":status")
          continue;
        auto value = headers_obj->Get(ctx, key_value).ToLocalChecked();
        response.headers.emplace_back(std::move(key), string_arg(iso, value));
      }
      std::string err;
      const auto request_id = static_cast<u64>(info[1]->IntegerValue(ctx).FromMaybe(0));
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(server->mutex);
        ok = server->server->respond(request_id, response, err);
      }
      if (!ok) {
        const auto last_error = server->server->last_error();
        throw_error(iso, !last_error.empty()
                             ? last_error
                             : (err.empty() ? "HTTP/2 server respond failed" : err));
        return;
      }
      info.GetReturnValue().Set(True(iso));
    }

    void http2_server_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsNumber())
        return;
      const int handle = info[0]->Int32Value(ctx).FromMaybe(0);
      std::shared_ptr<server_entry> server;
      {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = servers.find(handle);
        if (it != servers.end()) {
          server = it->second;
          servers.erase(it);
        }
      }
      if (server) {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->server->close();
      }
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> ns, const char* name,
                      FunctionCallback callback) {
      auto fn = Function::New(ctx, callback).ToLocalChecked();
      (void)ns->Set(ctx, to_v8_string(iso, name), fn);
    }

    Local<Object> make_http2_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "connect", http2_connect);
      add_function(iso, ctx, ns, "submit", http2_submit);
      add_function(iso, ctx, ns, "cancel", http2_cancel);
      add_function(iso, ctx, ns, "read", http2_read);
      add_function(iso, ctx, ns, "readResult", http2_read_result);
      add_function(iso, ctx, ns, "write", http2_write);
      add_function(iso, ctx, ns, "close", http2_close);
      add_function(iso, ctx, ns, "createServer", http2_server_create);
      add_function(iso, ctx, ns, "serverAddress", http2_server_address);
      add_function(iso, ctx, ns, "serverPoll", http2_server_poll);
      add_function(iso, ctx, ns, "serverRespond", http2_server_respond);
      add_function(iso, ctx, ns, "serverClose", http2_server_close);
      set_prop(ctx, ns, "available", true);
      set_prop(ctx, ns, "notImplemented", false);
      return ns;
    }
  } // namespace

  void install_native_http2(Isolate* iso, Local<Context> ctx) {
    auto global = ctx->Global();
    Local<Value> native_value;
    if (!global->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) ||
        !native_value->IsObject()) {
      native_value = Object::New(iso);
      define_prop(ctx, global, "__fxe_native"_v8, native_value,
                  static_cast<PropertyAttribute>(DontEnum));
    }
    auto native = native_value.As<Object>();
    (void)native->Set(ctx, "http2"_v8(iso), make_http2_namespace(iso, ctx));
  }
} // namespace fxe::runtime
