#include "net/http2_client.hpp"

#include "net/tls_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fxe::net {
  // TODO(http2): implement server push, flow-control callbacks, and per-stream timeout.
  namespace {
    std::string describe_nghttp2_error(int rv, std::string_view action) {
      std::string out(action);
      out.append(": ");
      out.append(nghttp2_strerror(rv));
      return out;
    }

    std::string lower_ascii(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }

    bool is_forbidden_user_header(std::string_view name) {
      return name == ":method" || name == ":path" || name == ":scheme" || name == ":authority" ||
             name == "connection" || name == "upgrade" || name == "http2-settings" ||
             name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding";
    }

    nghttp2_nv make_nv(const std::string& name, const std::string& value) {
      return nghttp2_nv{reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())),
                        reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(),
                        value.size(), NGHTTP2_NV_FLAG_NONE};
    }

    void append_setting(std::vector<nghttp2_settings_entry>& entries, int32_t id,
                        const std::optional<uint32_t>& value) {
      if (value)
        entries.push_back(nghttp2_settings_entry{id, *value});
    }

    std::vector<nghttp2_settings_entry> settings_entries(const http2_settings& settings) {
      std::vector<nghttp2_settings_entry> entries;
      append_setting(entries, NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, settings.header_table_size);
      append_setting(entries, NGHTTP2_SETTINGS_ENABLE_PUSH, settings.enable_push);
      append_setting(entries, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
                     settings.max_concurrent_streams);
      append_setting(entries, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, settings.initial_window_size);
      append_setting(entries, NGHTTP2_SETTINGS_MAX_FRAME_SIZE, settings.max_frame_size);
      append_setting(entries, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, settings.max_header_list_size);
      return entries;
    }

    struct stream_response_state {
      http2_response response;
      bool complete = false;
      bool failed = false;
      std::string error;
    };

    struct body_state {
      std::string body;
      size_t offset = 0;
    };

    class nghttp2_client final : public http2_client {
    public:
      nghttp2_client(std::unique_ptr<tls_client> stream, std::string host, http2_settings settings)
          : stream_(std::move(stream)), host_(std::move(host)), settings_(std::move(settings)) {
        nghttp2_session_callbacks_new(&callbacks_);
        nghttp2_session_callbacks_set_send_callback(callbacks_, send_callback);
        nghttp2_session_callbacks_set_recv_callback(callbacks_, recv_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks_, on_header_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_,
                                                                  on_data_chunk_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_,
                                                               on_stream_close_callback);
        nghttp2_session_client_new(&session_, callbacks_, this);
      }

      ~nghttp2_client() override {
        close();
      }

      bool start(std::string& err) {
        last_error_.clear();
        if (!session_)
          return fail(err, "nghttp2_session_client_new failed");
        const auto entries = settings_entries(settings_);
        const int rv =
            nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, entries.data(), entries.size());
        if (rv != 0)
          return fail(err, describe_nghttp2_error(rv, "submit HTTP/2 client settings"));
        return flush(err);
      }

      int32_t submit(const http2_request& request) override {
        last_error_.clear();
        if (closed_ || !session_) {
          last_error_ = "HTTP/2 client is closed";
          return -1;
        }

        std::vector<std::pair<std::string, std::string>> storage;
        storage.emplace_back(":method", request.method.empty() ? "GET" : request.method);
        storage.emplace_back(":scheme", "https");
        storage.emplace_back(":authority", host_);
        storage.emplace_back(":path", request.path.empty() ? "/" : request.path);
        for (const auto& [name, value] : request.headers) {
          auto lowered = lower_ascii(name);
          if (is_forbidden_user_header(lowered))
            continue;
          storage.emplace_back(std::move(lowered), value);
        }
        if (!request.body.empty())
          storage.emplace_back("content-length", std::to_string(request.body.size()));

        std::vector<nghttp2_nv> nva;
        nva.reserve(storage.size());
        for (const auto& [name, value] : storage)
          nva.push_back(make_nv(name, value));

        nghttp2_data_provider provider{};
        nghttp2_data_provider* provider_ptr = nullptr;
        if (!request.body.empty()) {
          provider.read_callback = data_source_read_callback;
          provider_ptr = &provider;
        }

        const int32_t stream_id = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                                                         provider_ptr, nullptr);
        if (stream_id < 0) {
          last_error_ = describe_nghttp2_error(stream_id, "submit HTTP/2 request");
          return -1;
        }
        if (!request.body.empty())
          request_bodies_[stream_id] = body_state{request.body, 0};

        std::string ignored;
        if (!flush(ignored)) {
          if (!ignored.empty())
            last_error_ = ignored;
          return -1;
        }
        responses_.try_emplace(stream_id);
        return stream_id;
      }

      http2_response wait(int32_t stream_id, std::string& err) override {
        last_error_.clear();
        auto& state = responses_[stream_id];
        while (!closed_ && !state.complete && !state.failed) {
          const int rv = nghttp2_session_recv(session_);
          if (rv == NGHTTP2_ERR_EOF) {
            err = "HTTP/2 connection closed before stream completed";
            last_error_ = err;
            state.failed = true;
            break;
          }
          if (rv < 0) {
            err = describe_nghttp2_error(rv, "receive HTTP/2 frames");
            last_error_ = err;
            state.failed = true;
            break;
          }
          if (!flush(err)) {
            last_error_ = err;
            state.failed = true;
            break;
          }
        }
        if (state.failed) {
          if (err.empty())
            err = state.error.empty() ? "HTTP/2 stream failed" : state.error;
          last_error_ = err;
        }
        return state.response;
      }

      void close() override {
        last_error_.clear();
        if (closed_)
          return;
        closed_ = true;
        if (session_) {
          nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, 0, NGHTTP2_NO_ERROR, nullptr, 0);
          std::string ignored;
          (void)flush(ignored);
          nghttp2_session_del(session_);
          session_ = nullptr;
        }
        if (callbacks_) {
          nghttp2_session_callbacks_del(callbacks_);
          callbacks_ = nullptr;
        }
        if (stream_)
          stream_->close();
      }

      std::string last_error() const override {
        return last_error_;
      }

    private:
      static ssize_t send_callback(nghttp2_session*, const uint8_t* data, size_t length, int,
                                   void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        ssize_t written = self->stream_->write(data, length);
        if (written < 0)
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        return written;
      }

      static ssize_t recv_callback(nghttp2_session*, uint8_t* data, size_t length, int,
                                   void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        ssize_t got = self->stream_->read(data, length);
        if (got == 0)
          return NGHTTP2_ERR_EOF;
        if (got < 0)
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        return got;
      }

      static int on_header_callback(nghttp2_session*, const nghttp2_frame* frame,
                                    const uint8_t* name, size_t namelen, const uint8_t* value,
                                    size_t valuelen, uint8_t, void* user_data) {
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
          return 0;
        auto* self = static_cast<nghttp2_client*>(user_data);
        auto& response = self->responses_[frame->hd.stream_id].response;
        std::string header_name(reinterpret_cast<const char*>(name), namelen);
        std::string header_value(reinterpret_cast<const char*>(value), valuelen);
        if (header_name == ":status") {
          try {
            response.status = std::stoi(header_value);
          } catch (...) {
            response.status = 0;
          }
        } else {
          response.headers.emplace_back(std::move(header_name), std::move(header_value));
        }
        return 0;
      }

      static int on_data_chunk_callback(nghttp2_session*, uint8_t, int32_t stream_id,
                                        const uint8_t* data, size_t len, void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        self->responses_[stream_id].response.body.append(reinterpret_cast<const char*>(data), len);
        return 0;
      }

      static int on_stream_close_callback(nghttp2_session*, int32_t stream_id, uint32_t error_code,
                                          void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        auto& state = self->responses_[stream_id];
        state.complete = true;
        if (error_code != NGHTTP2_NO_ERROR) {
          state.failed = true;
          state.error = "HTTP/2 stream closed with error code " + std::to_string(error_code);
        }
        self->request_bodies_.erase(stream_id);
        return 0;
      }

      static ssize_t data_source_read_callback(nghttp2_session*, int32_t stream_id, uint8_t* buf,
                                               size_t length, uint32_t* data_flags,
                                               nghttp2_data_source*, void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        auto it = self->request_bodies_.find(stream_id);
        if (it == self->request_bodies_.end()) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
          return 0;
        }
        auto& body = it->second;
        const size_t remaining = body.body.size() - body.offset;
        const size_t n = std::min(length, remaining);
        if (n > 0) {
          std::memcpy(buf, body.body.data() + body.offset, n);
          body.offset += n;
        }
        if (body.offset >= body.body.size())
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<ssize_t>(n);
      }

      bool flush(std::string& err) {
        while (!closed_ && nghttp2_session_want_write(session_)) {
          const int rv = nghttp2_session_send(session_);
          if (rv < 0) {
            err = describe_nghttp2_error(rv, "send HTTP/2 frames");
            last_error_ = err;
            return false;
          }
        }
        return true;
      }

      bool fail(std::string& err, std::string message) {
        last_error_ = std::move(message);
        err = last_error_;
        return false;
      }

      std::unique_ptr<tls_client> stream_;
      std::string host_;
      http2_settings settings_;
      nghttp2_session_callbacks* callbacks_ = nullptr;
      nghttp2_session* session_ = nullptr;
      bool closed_ = false;
      std::map<int32_t, stream_response_state> responses_;
      std::map<int32_t, body_state> request_bodies_;
      std::string last_error_;
    };
  } // namespace

  http2_client::~http2_client() = default;

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, uint16_t port,
                                                      std::string& err) {
    return connect(host, port, std::string{}, true, http2_settings{}, err);
  }

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, uint16_t port,
                                                      const http2_settings& settings,
                                                      std::string& err) {
    return connect(host, port, std::string{}, true, settings, err);
  }

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, uint16_t port,
                                                      const std::string& ca_pem,
                                                      bool reject_unauthorized, std::string& err) {
    return connect(host, port, ca_pem, reject_unauthorized, http2_settings{}, err);
  }

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, uint16_t port,
                                                      const std::string& ca_pem,
                                                      bool reject_unauthorized,
                                                      const http2_settings& settings,
                                                      std::string& err) {
    tls_options options;
    options.host = host;
    options.port = port;
    options.ca_pem = ca_pem;
    options.reject_unauthorized = reject_unauthorized;
    options.alpn = {"h2"};
    auto tls = tls_client::connect(options, err);
    if (!tls)
      return nullptr;
    const auto negotiated = tls->negotiated_alpn();
    if (negotiated != "h2") {
      err = negotiated.empty() ? "TLS ALPN did not negotiate h2"
                               : "TLS ALPN negotiated '" + negotiated + "' instead of 'h2'";
      tls->close();
      return nullptr;
    }
    const auto authority = port == 443 ? host : host + ":" + std::to_string(port);
    auto client = std::make_unique<nghttp2_client>(std::move(tls), authority, settings);
    if (!client->start(err))
      return nullptr;
    return client;
  }
} // namespace fxe::net
