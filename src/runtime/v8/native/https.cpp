#include "runtime/v8/native/https.hpp"
#include "runtime/v8/native/https_transport.hpp"

#include "net/tls_client.hpp"
#include "net/tls_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <deque>
#include <fxe/types.hpp>
#include <fxe/v8_literals.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fxe::runtime {
  namespace {
    using namespace v8;

    struct http_request_event {
      int request_id = 0;
      std::string method;
      std::string path;
      std::map<std::string, std::string> headers;
      std::string body;
    };

    struct server_event {
      std::string type;
      std::string message;
      http_request_event request;
    };

    struct https_server_state {
      int id = 0;
      std::string cert_pem;
      std::string key_pem;
      std::unique_ptr<fxe::net::tls_server> server;
      std::thread thread;
      std::mutex mutex;
      std::deque<server_event> events;
      std::unordered_map<int, std::unique_ptr<fxe::net::tls_client>> pending;
      std::atomic<bool> closing{false};
      u16 port = 0;
      int next_request_id = 1;
    };

    std::mutex& registry_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<int, std::shared_ptr<https_server_state>>& registry() {
      static std::unordered_map<int, std::shared_ptr<https_server_state>> servers;
      return servers;
    }

    int next_server_id() {
      static std::atomic<int> id{1};
      return id.fetch_add(1);
    }

    Local<String> str(Isolate* iso, std::string_view value) {
      return String::NewFromUtf8(iso, value.data(), NewStringType::kNormal,
                                 static_cast<int>(value.size()))
          .ToLocalChecked();
    }

    void set(Local<Context> ctx, Local<Object> obj, const char* key, Local<Value> value) {
      (void)obj->Set(ctx, str(Isolate::GetCurrent(), key), value);
    }

    void set_string(Local<Context> ctx, Local<Object> obj, const char* key,
                    std::string_view value) {
      set(ctx, obj, key, str(Isolate::GetCurrent(), value));
    }

    void set_number(Local<Context> ctx, Local<Object> obj, const char* key, double value) {
      set(ctx, obj, key, Number::New(Isolate::GetCurrent(), value));
    }

    void throw_error(Isolate* iso, std::string_view message) {
      iso->ThrowException(Exception::Error(str(iso, message)));
    }

    std::string string_arg(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      return *utf8 ? std::string(*utf8, static_cast<usize>(utf8.length())) : std::string{};
    }

    std::optional<Local<Value>> get_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                         const char* name) {
      Local<Value> value;
      if (!obj->Get(ctx, str(iso, name)).ToLocal(&value))
        return std::nullopt;
      return value;
    }

    std::string object_string_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                   const char* name) {
      auto value = get_prop(iso, ctx, obj, name);
      if (!value || (*value)->IsNullOrUndefined())
        return {};
      return string_arg(iso, *value);
    }

    int object_int_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                        int fallback = 0) {
      auto value = get_prop(iso, ctx, obj, name);
      if (!value || (*value)->IsNullOrUndefined())
        return fallback;
      return (*value)->Int32Value(ctx).FromMaybe(fallback);
    }

    bool object_bool_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                          bool fallback = false) {
      auto value = get_prop(iso, ctx, obj, name);
      if (!value || (*value)->IsNullOrUndefined())
        return fallback;
      return (*value)->BooleanValue(iso);
    }

    std::string lower_ascii(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      return value;
    }

    std::string trim_ascii(std::string_view value) {
      usize first = 0;
      while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
      usize last = value.size();
      while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
      return std::string(value.substr(first, last - first));
    }

    std::string header_value_to_string(Isolate* iso, Local<Context> ctx, Local<Value> value) {
      if (value->IsNullOrUndefined())
        return {};
      if (value->IsArray()) {
        auto arr = value.As<Array>();
        std::string out;
        const auto length = arr->Length();
        for (u32 i = 0; i < length; ++i) {
          Local<Value> item;
          if (!arr->Get(ctx, i).ToLocal(&item) || item->IsNullOrUndefined())
            continue;
          if (!out.empty())
            out.append(", ");
          out.append(string_arg(iso, item));
        }
        return out;
      }
      return string_arg(iso, value);
    }

    std::map<std::string, std::string> headers_from_value(Isolate* iso, Local<Context> ctx,
                                                          Local<Value> value) {
      std::map<std::string, std::string> headers;
      if (!value->IsObject())
        return headers;
      auto obj = value.As<Object>();
      Local<Array> names;
      if (!obj->GetOwnPropertyNames(ctx).ToLocal(&names))
        return headers;
      for (u32 i = 0; i < names->Length(); ++i) {
        Local<Value> key_value;
        if (!names->Get(ctx, i).ToLocal(&key_value))
          continue;
        auto key = lower_ascii(string_arg(iso, key_value));
        Local<Value> val;
        if (!obj->Get(ctx, key_value).ToLocal(&val))
          continue;
        auto text = header_value_to_string(iso, ctx, val);
        if (!key.empty() && !text.empty())
          headers[key] = text;
      }
      return headers;
    }

    std::string bytes_from_value(Isolate* iso, Local<Context> ctx, Local<Value> value) {
      (void)ctx;
      if (value->IsNullOrUndefined())
        return {};
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto buffer = view->Buffer();
        auto backing = buffer->GetBackingStore();
        const auto offset = view->ByteOffset();
        const auto length = view->ByteLength();
        const auto* bytes = static_cast<const char*>(backing->Data()) + offset;
        return std::string(bytes, bytes + length);
      }
      if (value->IsArrayBuffer()) {
        auto buffer = value.As<ArrayBuffer>();
        auto backing = buffer->GetBackingStore();
        const auto* bytes = static_cast<const char*>(backing->Data());
        return std::string(bytes, bytes + backing->ByteLength());
      }
      return string_arg(iso, value);
    }

    std::shared_ptr<https_server_state> find_server(int id) {
      std::lock_guard<std::mutex> lock(registry_mutex());
      auto it = registry().find(id);
      return it == registry().end() ? nullptr : it->second;
    }

    void enqueue_event(const std::shared_ptr<https_server_state>& state, server_event event) {
      if (!state)
        return;
      std::lock_guard<std::mutex> lock(state->mutex);
      state->events.push_back(std::move(event));
    }

    bool write_all(fxe::net::tls_client& client, const void* data, usize size,
                   std::string* error = nullptr) {
      const auto* bytes = static_cast<const char*>(data);
      usize written = 0;
      while (written < size) {
        const auto n = client.write(bytes + written, size - written);
        if (n <= 0) {
          if (error)
            *error = client.last_error().empty() ? "TLS write failed" : client.last_error();
          return false;
        }
        written += static_cast<usize>(n);
      }
      return true;
    }

    bool write_all(fxe::net::tls_client& client, const std::string& data,
                   std::string* error = nullptr) {
      return write_all(client, data.data(), data.size(), error);
    }

    std::optional<usize> parse_content_length(const std::map<std::string, std::string>& headers) {
      auto it = headers.find("content-length");
      if (it == headers.end())
        return 0;
      usize value = 0;
      auto first = it->second.data();
      auto last = first + it->second.size();
      auto [ptr, ec] = std::from_chars(first, last, value);
      if (ec != std::errc{} || ptr != last)
        return std::nullopt;
      return value;
    }

    std::optional<http_request_event> parse_http_request(std::string& buffer, std::string& error) {
      const auto header_end = buffer.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        error = "incomplete HTTP request headers";
        return std::nullopt;
      }

      std::string_view header_block(buffer.data(), header_end);
      usize line_start = 0;
      auto line_end = header_block.find("\r\n");
      auto request_line = header_block.substr(
          0, line_end == std::string_view::npos ? header_block.size() : line_end);
      const auto first_space = request_line.find(' ');
      const auto second_space = first_space == std::string_view::npos
                                    ? std::string_view::npos
                                    : request_line.find(' ', first_space + 1);
      if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        error = "malformed HTTP request line";
        return std::nullopt;
      }

      http_request_event request;
      request.method = std::string(request_line.substr(0, first_space));
      request.path =
          std::string(request_line.substr(first_space + 1, second_space - first_space - 1));

      line_start = line_end == std::string_view::npos ? header_block.size() : line_end + 2;
      while (line_start < header_block.size()) {
        line_end = header_block.find("\r\n", line_start);
        if (line_end == std::string_view::npos)
          line_end = header_block.size();
        auto line = header_block.substr(line_start, line_end - line_start);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
          auto key = lower_ascii(trim_ascii(line.substr(0, colon)));
          auto value = trim_ascii(line.substr(colon + 1));
          if (!key.empty())
            request.headers[key] = value;
        }
        line_start = line_end + 2;
      }

      auto content_length = parse_content_length(request.headers);
      if (!content_length) {
        error = "invalid HTTP content-length";
        return std::nullopt;
      }
      const auto body_start = header_end + 4;
      if (buffer.size() < body_start + *content_length) {
        error = "incomplete HTTP request body";
        return std::nullopt;
      }
      request.body.assign(buffer.data() + body_start, *content_length);
      return request;
    }

    std::optional<http_request_event> read_http_request(fxe::net::tls_client& client,
                                                        std::string& error) {
      std::string buffer;
      std::array<char, 4096> chunk{};
      std::optional<usize> needed;
      for (;;) {
        const auto header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          std::map<std::string, std::string> headers;
          std::string parse_error;
          auto parsed = parse_http_request(buffer, parse_error);
          if (parsed)
            return parsed;
          if (parse_error != "incomplete HTTP request body") {
            error = parse_error;
            return std::nullopt;
          }
          if (!needed) {
            std::string header_part = buffer.substr(0, header_end + 4);
            auto bodyless = parse_http_request(header_part, parse_error);
            (void)bodyless;
            auto header_block = std::string_view(buffer.data(), header_end);
            usize line_start = 0;
            while (line_start < header_block.size()) {
              auto line_end = header_block.find("\r\n", line_start);
              if (line_end == std::string_view::npos)
                line_end = header_block.size();
              auto line = header_block.substr(line_start, line_end - line_start);
              const auto colon = line.find(':');
              if (colon != std::string_view::npos)
                headers[lower_ascii(trim_ascii(line.substr(0, colon)))] =
                    trim_ascii(line.substr(colon + 1));
              line_start = line_end + 2;
            }
            auto content_length = parse_content_length(headers);
            if (!content_length) {
              error = "invalid HTTP content-length";
              return std::nullopt;
            }
            needed = header_end + 4 + *content_length;
          }
          if (needed && buffer.size() >= *needed)
            continue;
        }
        if (buffer.size() > 16 * 1024 * 1024) {
          error = "HTTP request too large";
          return std::nullopt;
        }
        const auto n = client.read(chunk.data(), chunk.size());
        if (n <= 0) {
          error = n < 0 && !client.last_error().empty()
                      ? client.last_error()
                      : "TLS client closed before complete HTTP request";
          return std::nullopt;
        }
        buffer.append(chunk.data(), static_cast<usize>(n));
      }
    }

    std::string reason_phrase(int status) {
      switch (status) {
      case 200:
        return "OK";
      case 201:
        return "Created";
      case 204:
        return "No Content";
      case 400:
        return "Bad Request";
      case 404:
        return "Not Found";
      case 500:
        return "Internal Server Error";
      default:
        return "OK";
      }
    }

    void accept_loop(std::shared_ptr<https_server_state> state) {
      while (!state->closing.load()) {
        std::string err;
        auto client = state->server ? state->server->accept(err) : nullptr;
        if (!client) {
          if (!state->closing.load() && !err.empty())
            enqueue_event(state, server_event{"error", err, {}});
          break;
        }
        std::string parse_error;
        auto request = read_http_request(*client, parse_error);
        if (!request) {
          std::string body = "Bad Request";
          std::string response =
              "HTTP/1.1 400 Bad Request\r\nContent-Length: " + std::to_string(body.size()) +
              "\r\nConnection: close\r\n\r\n" + body;
          (void)write_all(*client, response);
          client->close();
          enqueue_event(state, server_event{"error", parse_error, {}});
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          request->request_id = state->next_request_id++;
          state->pending.emplace(request->request_id, std::move(client));
          state->events.push_back(server_event{"request", {}, std::move(*request)});
        }
      }
      enqueue_event(state, server_event{"close", {}, {}});
    }

    struct parsed_url {
      std::string host;
      u16 port = 443;
      std::string path = "/";
    };

    std::optional<parsed_url> parse_https_url(std::string_view url, std::string& error) {
      constexpr std::string_view prefix = "https://";
      if (!url.starts_with(prefix)) {
        error = "node:https native request requires https: URL";
        return std::nullopt;
      }
      url.remove_prefix(prefix.size());
      const auto path_start = url.find_first_of("/?#");
      auto authority = path_start == std::string_view::npos ? url : url.substr(0, path_start);
      parsed_url out;
      out.path = path_start == std::string_view::npos ? "/" : std::string(url.substr(path_start));
      if (authority.empty()) {
        error = "HTTPS URL missing host";
        return std::nullopt;
      }
      if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos) {
          error = "invalid HTTPS URL host";
          return std::nullopt;
        }
        out.host = std::string(authority.substr(1, end - 1));
        if (end + 1 < authority.size()) {
          if (authority[end + 1] != ':') {
            error = "invalid HTTPS URL host";
            return std::nullopt;
          }
          auto port_text = authority.substr(end + 2);
          unsigned int port = 0;
          auto [ptr, ec] =
              std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
          if (ec != std::errc{} || ptr != port_text.data() + port_text.size() || port > 65535) {
            error = "invalid HTTPS URL port";
            return std::nullopt;
          }
          out.port = static_cast<u16>(port);
        }
      } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
          out.host = std::string(authority.substr(0, colon));
          auto port_text = authority.substr(colon + 1);
          unsigned int port = 0;
          auto [ptr, ec] =
              std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
          if (ec != std::errc{} || ptr != port_text.data() + port_text.size() || port > 65535) {
            error = "invalid HTTPS URL port";
            return std::nullopt;
          }
          out.port = static_cast<u16>(port);
        } else {
          out.host = std::string(authority);
        }
      }
      if (out.host.empty()) {
        error = "HTTPS URL missing host";
        return std::nullopt;
      }
      return out;
    }

    std::optional<std::string> read_tls_until_close(fxe::net::tls_client& client,
                                                    std::string& error) {
      std::string out;
      std::array<char, 4096> chunk{};
      for (;;) {
        const auto n = client.read(chunk.data(), chunk.size());
        if (n == 0)
          return out;
        if (n < 0) {
          error = client.last_error().empty() ? "TLS read failed" : client.last_error();
          return std::nullopt;
        }
        out.append(chunk.data(), static_cast<usize>(n));
        if (out.size() > 32 * 1024 * 1024) {
          error = "HTTP response too large";
          return std::nullopt;
        }
      }
    }

    Local<Object> headers_to_object(Isolate* iso, Local<Context> ctx,
                                    const std::map<std::string, std::string>& headers) {
      auto obj = Object::New(iso);
      for (const auto& [key, value] : headers)
        set_string(ctx, obj, key.c_str(), value);
      return obj;
    }

    void native_create_server(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        throw_error(iso, "__fxe_native.https.createServer requires options");
        return;
      }
      auto options = info[0].As<Object>();
      auto cert = object_string_prop(iso, ctx, options, "cert");
      auto key = object_string_prop(iso, ctx, options, "key");
      if (cert.empty() || key.empty()) {
        throw_error(iso, "native TLS server implementation requires { cert, key } PEM");
        return;
      }
      auto state = std::make_shared<https_server_state>();
      state->id = next_server_id();
      state->cert_pem = std::move(cert);
      state->key_pem = std::move(key);
      {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[state->id] = state;
      }
      info.GetReturnValue().Set(Integer::New(iso, state->id));
    }

    void native_listen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[1]->IsObject()) {
        throw_error(iso, "__fxe_native.https.listen requires server id and options");
        return;
      }
      auto state = find_server(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!state) {
        throw_error(iso, "native HTTPS server handle not found");
        return;
      }
      if (state->server) {
        auto out = Object::New(iso);
        set_number(ctx, out, "port", state->port);
        info.GetReturnValue().Set(out);
        return;
      }
      auto options = info[1].As<Object>();
      fxe::net::tls_server_options tls_options;
      tls_options.cert_pem = state->cert_pem;
      tls_options.key_pem = state->key_pem;
      tls_options.port =
          static_cast<u16>(std::clamp(object_int_prop(iso, ctx, options, "port", 0), 0, 65535));
      tls_options.alpn = {"http/1.1"};
      std::string err;
      auto server = fxe::net::tls_server::listen(tls_options, err);
      if (!server) {
        throw_error(iso, err.empty() ? "native HTTPS server listen failed" : err);
        return;
      }
      state->port = server->local_port();
      state->server = std::move(server);
      state->closing.store(false);
      try {
        state->thread = std::thread(accept_loop, state);
      } catch (const std::exception& e) {
        state->server->close();
        state->server.reset();
        throw_error(iso, std::string("native HTTPS server thread start failed: ") + e.what());
        return;
      }
      auto out = Object::New(iso);
      set_number(ctx, out, "port", state->port);
      info.GetReturnValue().Set(out);
    }

    void native_address(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto state = find_server(info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(0) : 0);
      if (!state || !state->server) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      auto out = Object::New(iso);
      set_string(ctx, out, "address", "0.0.0.0");
      set_string(ctx, out, "family", "IPv4");
      set_number(ctx, out, "port", state->port);
      info.GetReturnValue().Set(out);
    }

    void native_drain(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto state = find_server(info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(0) : 0);
      if (!state) {
        info.GetReturnValue().Set(Array::New(iso, 0));
        return;
      }
      std::deque<server_event> events;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        events.swap(state->events);
      }
      auto arr = Array::New(iso, static_cast<int>(events.size()));
      for (u32 i = 0; i < events.size(); ++i) {
        auto obj = Object::New(iso);
        set_string(ctx, obj, "type", events[i].type);
        if (!events[i].message.empty())
          set_string(ctx, obj, "message", events[i].message);
        if (events[i].type == "request") {
          set_number(ctx, obj, "requestId", events[i].request.request_id);
          set_string(ctx, obj, "method", events[i].request.method);
          set_string(ctx, obj, "url", events[i].request.path);
          set(ctx, obj, "headers", headers_to_object(iso, ctx, events[i].request.headers));
          set_string(ctx, obj, "body", events[i].request.body);
        }
        (void)arr->Set(ctx, i, obj);
      }
      info.GetReturnValue().Set(arr);
    }

    void native_respond(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 5) {
        throw_error(
            iso,
            "__fxe_native.https.respond requires server id, request id, status, headers, body");
        return;
      }
      auto state = find_server(info[0]->Int32Value(ctx).FromMaybe(0));
      if (!state) {
        throw_error(iso, "native HTTPS server handle not found");
        return;
      }
      const int request_id = info[1]->Int32Value(ctx).FromMaybe(0);
      std::unique_ptr<fxe::net::tls_client> client;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto it = state->pending.find(request_id);
        if (it == state->pending.end()) {
          throw_error(iso, "native HTTPS request handle not found");
          return;
        }
        client = std::move(it->second);
        state->pending.erase(it);
      }
      const int status = std::clamp(info[2]->Int32Value(ctx).FromMaybe(200), 100, 999);
      auto headers = headers_from_value(iso, ctx, info[3]);
      auto body = bytes_from_value(iso, ctx, info[4]);
      headers["content-length"] = std::to_string(body.size());
      headers["connection"] = "close";
      std::ostringstream response;
      response << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n";
      for (const auto& [key, value] : headers)
        response << key << ": " << value << "\r\n";
      response << "\r\n";
      const auto head = response.str();
      std::string write_error;
      const bool ok =
          write_all(*client, head, &write_error) && write_all(*client, body, &write_error);
      client->close();
      if (!ok)
        throw_error(iso, write_error.empty() ? "native HTTPS response write failed" : write_error);
    }

    void native_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int id = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(0) : 0;
      std::shared_ptr<https_server_state> state;
      {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto it = registry().find(id);
        if (it != registry().end()) {
          state = it->second;
          registry().erase(it);
        }
      }
      if (!state) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      state->closing.store(true);
      if (state->server)
        state->server->close();
      if (state->thread.joinable())
        state->thread.join();
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto& [_, client] : state->pending) {
          if (client)
            client->close();
        }
        state->pending.clear();
      }
      info.GetReturnValue().Set(True(iso));
    }

    void native_request(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString() || !info[1]->IsObject()) {
        throw_error(iso, "__fxe_native.https.request requires url, options, body");
        return;
      }
      auto options = info[1].As<Object>();
      fxe::net::http_request request;
      request.url = string_arg(iso, info[0]);
      request.method = object_string_prop(iso, ctx, options, "method");
      if (request.method.empty())
        request.method = "GET";
      auto headers_value = get_prop(iso, ctx, options, "headers");
      auto headers = headers_value ? headers_from_value(iso, ctx, *headers_value)
                                   : std::map<std::string, std::string>{};
      for (auto& [key, value] : headers)
        request.headers.emplace_back(std::move(key), std::move(value));
      request.body = bytes_from_value(iso, ctx, info[2]);

      native_https_request_options native_options;
      native_options.reject_unauthorized =
          object_bool_prop(iso, ctx, options, "rejectUnauthorized", true);
      native_options.session_namespace = object_string_prop(iso, ctx, options, "sessionNamespace");
      auto response =
          perform_native_https_request(std::move(request), nullptr, std::move(native_options));
      if (!response.error.empty()) {
        throw_error(iso, response.error);
        return;
      }

      std::map<std::string, std::string> response_headers;
      for (const auto& [key, value] : response.headers)
        response_headers[lower_ascii(key)] = value;
      auto out = Object::New(iso);
      set_number(ctx, out, "statusCode", response.status);
      set_string(ctx, out, "statusMessage", response.status_text);
      set(ctx, out, "headers", headers_to_object(iso, ctx, response_headers));
      set_string(ctx, out, "body", response.body);
      info.GetReturnValue().Set(out);
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> ns, const char* name,
                      FunctionCallback callback) {
      auto fn = Function::New(ctx, callback).ToLocalChecked();
      (void)ns->Set(ctx, str(iso, name), fn);
    }

    Local<Object> make_https_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "createServer", native_create_server);
      add_function(iso, ctx, ns, "listen", native_listen);
      add_function(iso, ctx, ns, "address", native_address);
      add_function(iso, ctx, ns, "drain", native_drain);
      add_function(iso, ctx, ns, "respond", native_respond);
      add_function(iso, ctx, ns, "close", native_close);
      add_function(iso, ctx, ns, "request", native_request);
      auto agent = Object::New(iso);
      set(ctx, ns, "globalAgent", agent);
      return ns;
    }

  } // namespace

  void install_native_https(Isolate* iso, Local<Context> ctx) {
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
    (void)native->Set(ctx, "https"_v8(iso), make_https_namespace(iso, ctx));
  }

} // namespace fxe::runtime
