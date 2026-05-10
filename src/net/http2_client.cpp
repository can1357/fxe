#include "net/http2_client.hpp"

#include "net/tls_client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fxe/types.hpp>
#include <fxe/string_utils.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <nghttp2/nghttp2.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fxe::net {
  namespace {
    constexpr int kReadPollTimeoutMs = 50;

    std::string describe_nghttp2_error(int rv, std::string_view action) {
      std::string out(action);
      out.append(": ");
      out.append(nghttp2_strerror(rv));
      return out;
    }

    i64 monotonic_ms() {
      using clock = std::chrono::steady_clock;
      return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
          .count();
    }

    bool is_forbidden_user_header(std::string_view name) {
      return name == ":method" || name == ":path" || name == ":scheme" || name == ":authority" ||
             name == "connection" || name == "upgrade" || name == "http2-settings" ||
             name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding";
    }

    nghttp2_nv make_nv(const std::string& name, const std::string& value) {
      return nghttp2_nv{reinterpret_cast<u8*>(const_cast<char*>(name.data())),
                        reinterpret_cast<u8*>(const_cast<char*>(value.data())), name.size(),
                        value.size(), NGHTTP2_NV_FLAG_NONE};
    }

    void append_setting(std::vector<nghttp2_settings_entry>& entries, i32 id,
                        const std::optional<u32>& value) {
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
      i64 start_ms = 0;
      int timeout_ms = 0;
      std::condition_variable cv;
    };

    struct pushed_stream_state {
      i32 stream_id = 0;
      i32 associated_stream_id = 0;
      http2_request promised_request;
      http2_response response;
      bool response_started = false;
      bool complete = false;
      bool refused = false;
      bool delivered = false;
    };

    struct body_state {
      std::string body;
      usize offset = 0;
    };

    class nghttp2_client final : public http2_client {
    public:
      nghttp2_client(std::unique_ptr<tls_client> stream, std::string host, http2_settings settings)
          : stream_(std::move(stream)), host_(std::move(host)), settings_(std::move(settings)) {
        nghttp2_session_callbacks_new(&callbacks_);
        nghttp2_session_callbacks_set_send_callback(callbacks_, send_callback);
        nghttp2_session_callbacks_set_recv_callback(callbacks_, recv_callback);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_,
                                                                on_begin_headers_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks_, on_header_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_,
                                                                  on_data_chunk_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_,
                                                               on_stream_close_callback);
        nghttp2_option_new(&option_);
        if (option_)
          nghttp2_option_set_no_auto_window_update(option_, 1);
        nghttp2_session_client_new2(&session_, callbacks_, this, option_);
      }

      ~nghttp2_client() override {
        close();
      }

      bool start(std::string& err) {
        clear_last_error();
        if (!session_)
          return fail(err, "nghttp2_session_client_new2 failed");
        const auto entries = settings_entries(settings_);
        {
          std::lock_guard<std::mutex> session_lock(session_mutex_);
          const int rv =
              nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, entries.data(), entries.size());
          if (rv != 0)
            return fail(err, describe_nghttp2_error(rv, "submit HTTP/2 client settings"));
          if (!flush_locked(err))
            return false;
          if (settings_.stream_max_recv_window_size &&
              !set_local_window_size_locked(
                  0, static_cast<i32>(*settings_.stream_max_recv_window_size), err,
                  "set HTTP/2 client connection window"))
            return false;
        }
        worker_thread_ = std::thread([this] { session_loop(); });
        return true;
      }

      i32 submit(const http2_request& request) override {
        clear_last_error();
        std::vector<std::pair<std::string, std::string>> storage;
        storage.emplace_back(":method", request.method.empty() ? "GET" : request.method);
        storage.emplace_back(":scheme", "https");
        storage.emplace_back(":authority", host_);
        storage.emplace_back(":path", request.path.empty() ? "/" : request.path);
        for (const auto& [name, value] : request.headers) {
          auto lowered = ascii_lower(name);
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

        const auto state = std::make_shared<stream_response_state>();
        state->start_ms = monotonic_ms();
        state->timeout_ms = request.timeout_ms;

        std::string flush_err;
        i32 stream_id = 0;
        {
          std::lock_guard<std::mutex> session_lock(session_mutex_);
          if (closed_ || !session_) {
            set_last_error("HTTP/2 client is closed");
            return -1;
          }
          stream_id = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                                             provider_ptr, nullptr);
          if (stream_id < 0) {
            set_last_error(describe_nghttp2_error(stream_id, "submit HTTP/2 request"));
            return -1;
          }
          {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            responses_[stream_id] = state;
          }
          if (!request.body.empty())
            request_bodies_[stream_id] = body_state{request.body, 0};
          if (!flush_locked(flush_err)) {
            request_bodies_.erase(stream_id);
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            responses_.erase(stream_id);
            if (!flush_err.empty())
              set_last_error(flush_err);
            return -1;
          }
        }
        return stream_id;
      }

      http2_response wait(i32 stream_id, std::string& err) override {
        clear_last_error();
        err.clear();
        std::shared_ptr<stream_response_state> state;
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          auto it = responses_.find(stream_id);
          if (it == responses_.end()) {
            err = "unknown HTTP/2 stream id";
            last_error_ = err;
            return {};
          }
          state = it->second;
        }

        http2_response response;
        {
          std::unique_lock<std::mutex> lock(state_mutex_);
          state->cv.wait(lock, [&] { return closed_.load() || state->complete || state->failed; });
          response = state->response;
          if (state->failed) {
            err = state->error.empty() ? "HTTP/2 stream failed" : state->error;
            last_error_ = err;
          }
          responses_.erase(stream_id);
        }
        return response;
      }

      void cancel(i32 stream_id, u32 error_code = NGHTTP2_CANCEL) override {
        std::lock_guard<std::mutex> session_lock(session_mutex_);
        if (closed_.load() || !session_)
          return;
        (void)cancel_locked(stream_id, error_code, "ABORT_ERR");
      }

      void close() override {
        {
          std::lock_guard<std::mutex> session_lock(session_mutex_);
          if (closed_.load())
            return;
          closed_.store(true);
          if (session_) {
            nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, 0, NGHTTP2_NO_ERROR, nullptr, 0);
            std::string ignored;
            (void)flush_locked(ignored);
          }
        }
        if (stream_)
          stream_->close();
        if (worker_thread_.joinable())
          worker_thread_.join();
        fail_all_pending("HTTP/2 client is closed");
        std::lock_guard<std::mutex> session_lock(session_mutex_);
        request_bodies_.clear();
        if (session_) {
          nghttp2_session_del(session_);
          session_ = nullptr;
        }
        if (callbacks_) {
          nghttp2_session_callbacks_del(callbacks_);
          callbacks_ = nullptr;
        }
        if (option_) {
          nghttp2_option_del(option_);
          option_ = nullptr;
        }
      }

      std::string last_error() const override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_error_;
      }

      void set_push_handler(push_handler handler) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        push_handler_ = std::move(handler);
      }

      void set_on_data_consumed(on_data_consumed handler) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        on_data_consumed_ = std::move(handler);
      }

      void set_stream_window(i32 stream_id, i32 window_size) override {
        clear_last_error();
        std::string err;
        std::lock_guard<std::mutex> session_lock(session_mutex_);
        if (!session_) {
          set_last_error("HTTP/2 client is closed");
          return;
        }
        (void)set_local_window_size_locked(stream_id, window_size, err,
                                           "set HTTP/2 client stream window");
      }

    private:
      static ssize_t send_callback(nghttp2_session*, const u8* data, usize length, int,
                                   void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        ssize_t written = self->stream_->write(data, length);
        if (written < 0)
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        return written;
      }

      static ssize_t recv_callback(nghttp2_session*, u8* data, usize length, int, void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        ssize_t got = self->stream_->read_with_timeout(data, length, kReadPollTimeoutMs);
        if (got == tls_client::read_timed_out)
          return NGHTTP2_ERR_WOULDBLOCK;
        if (got == 0)
          return NGHTTP2_ERR_EOF;
        if (got < 0)
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        return got;
      }

      static int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                           void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        if (frame->hd.type == NGHTTP2_PUSH_PROMISE) {
          bool should_refuse = false;
          {
            std::lock_guard<std::mutex> lock(self->state_mutex_);
            auto state = std::make_shared<pushed_stream_state>();
            state->stream_id = frame->push_promise.promised_stream_id;
            state->associated_stream_id = frame->hd.stream_id;
            should_refuse = !self->push_handler_ ||
                            (self->settings_.enable_push && *self->settings_.enable_push == 0);
            state->refused = should_refuse;
            self->pushed_responses_[state->stream_id] = std::move(state);
          }
          if (should_refuse)
            self->refuse_promised_stream(session, frame->push_promise.promised_stream_id,
                                         NGHTTP2_REFUSED_STREAM);
          return 0;
        }
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_PUSH_RESPONSE) {
          std::lock_guard<std::mutex> lock(self->state_mutex_);
          auto it = self->pushed_responses_.find(frame->hd.stream_id);
          if (it != self->pushed_responses_.end())
            it->second->response_started = true;
        }
        return 0;
      }

      static int on_header_callback(nghttp2_session*, const nghttp2_frame* frame, const u8* name,
                                    usize namelen, const u8* value, usize valuelen, u8,
                                    void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        std::string header_name(reinterpret_cast<const char*>(name), namelen);
        std::string header_value(reinterpret_cast<const char*>(value), valuelen);
        std::lock_guard<std::mutex> lock(self->state_mutex_);
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
          auto it = self->responses_.find(frame->hd.stream_id);
          if (it == self->responses_.end())
            return 0;
          auto& response = it->second->response;
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
        if (frame->hd.type == NGHTTP2_PUSH_PROMISE) {
          auto it = self->pushed_responses_.find(frame->push_promise.promised_stream_id);
          if (it == self->pushed_responses_.end())
            return 0;
          auto& request = it->second->promised_request;
          if (header_name == ":method")
            request.method = std::move(header_value);
          else if (header_name == ":path")
            request.path = std::move(header_value);
          else
            request.headers.emplace_back(std::move(header_name), std::move(header_value));
          return 0;
        }
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_PUSH_RESPONSE) {
          auto it = self->pushed_responses_.find(frame->hd.stream_id);
          if (it == self->pushed_responses_.end())
            return 0;
          auto& response = it->second->response;
          it->second->response_started = true;
          if (header_name == ":status") {
            try {
              response.status = std::stoi(header_value);
            } catch (...) {
              response.status = 0;
            }
          } else {
            response.headers.emplace_back(std::move(header_name), std::move(header_value));
          }
        }
        return 0;
      }

      static int on_data_chunk_callback(nghttp2_session* session, u8, i32 stream_id, const u8* data,
                                        usize len, void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        on_data_consumed callback;
        bool refuse_protocol = false;
        {
          std::lock_guard<std::mutex> lock(self->state_mutex_);
          if (auto it = self->responses_.find(stream_id); it != self->responses_.end()) {
            it->second->response.body.append(reinterpret_cast<const char*>(data), len);
            callback = self->on_data_consumed_;
          } else if (auto push_it = self->pushed_responses_.find(stream_id);
                     push_it != self->pushed_responses_.end()) {
            if (!push_it->second->response_started) {
              push_it->second->refused = true;
              refuse_protocol = true;
            } else if (!push_it->second->refused) {
              push_it->second->response.body.append(reinterpret_cast<const char*>(data), len);
              callback = self->on_data_consumed_;
            }
          } else {
            return 0;
          }
        }
        if (refuse_protocol) {
          self->refuse_promised_stream(session, stream_id, NGHTTP2_PROTOCOL_ERROR);
          return 0;
        }
        const int consume_rv = nghttp2_session_consume(session, stream_id, len);
        if (consume_rv != 0)
          self->set_last_error(describe_nghttp2_error(consume_rv, "consume HTTP/2 data"));
        if (callback)
          callback(stream_id, len);
        return 0;
      }

      static int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame,
                                        void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        push_handler handler;
        http2_pushed pushed;
        bool should_deliver = false;
        if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
          std::lock_guard<std::mutex> lock(self->state_mutex_);
          auto it = self->pushed_responses_.find(frame->hd.stream_id);
          if (it != self->pushed_responses_.end()) {
            it->second->complete = true;
            should_deliver = self->prepare_push_delivery_locked(*it->second, pushed, handler);
          }
        }
        if (should_deliver && handler)
          handler(std::move(pushed));
        return 0;
      }

      static int on_stream_close_callback(nghttp2_session*, i32 stream_id, u32 error_code,
                                          void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        push_handler handler;
        http2_pushed pushed;
        bool should_deliver = false;
        {
          std::lock_guard<std::mutex> state_lock(self->state_mutex_);
          auto it = self->responses_.find(stream_id);
          if (it != self->responses_.end()) {
            auto& state = *it->second;
            state.complete = true;
            if (error_code != NGHTTP2_NO_ERROR && !state.failed) {
              state.failed = true;
              state.error = "HTTP/2 stream closed with error code " + std::to_string(error_code);
            }
            state.cv.notify_all();
          }
          auto push_it = self->pushed_responses_.find(stream_id);
          if (push_it != self->pushed_responses_.end()) {
            if (error_code != NGHTTP2_NO_ERROR)
              push_it->second->refused = true;
            should_deliver = self->prepare_push_delivery_locked(*push_it->second, pushed, handler);
            self->pushed_responses_.erase(push_it);
          }
        }
        self->request_bodies_.erase(stream_id);
        if (should_deliver && handler)
          handler(std::move(pushed));
        return 0;
      }

      static ssize_t data_source_read_callback(nghttp2_session*, i32 stream_id, u8* buf,
                                               usize length, u32* data_flags, nghttp2_data_source*,
                                               void* user_data) {
        auto* self = static_cast<nghttp2_client*>(user_data);
        auto it = self->request_bodies_.find(stream_id);
        if (it == self->request_bodies_.end()) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
          return 0;
        }
        auto& body = it->second;
        const usize remaining = body.body.size() - body.offset;
        const usize n = std::min(length, remaining);
        if (n > 0) {
          std::memcpy(buf, body.body.data() + body.offset, n);
          body.offset += n;
        }
        if (body.offset >= body.body.size())
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<ssize_t>(n);
      }

      void session_loop() {
        for (;;) {
          int rv = 0;
          bool stop = false;
          {
            std::lock_guard<std::mutex> session_lock(session_mutex_);
            if (closed_.load() || !session_)
              break;
            rv = nghttp2_session_recv(session_);
            if (rv == NGHTTP2_ERR_EOF) {
              fail_all_pending("HTTP/2 connection closed before stream completed");
              stop = true;
            } else if (rv < 0 && rv != NGHTTP2_ERR_WOULDBLOCK) {
              fail_all_pending(describe_nghttp2_error(rv, "receive HTTP/2 frames"));
              stop = true;
            } else {
              std::string err;
              if (!flush_locked(err)) {
                fail_all_pending(err.empty() ? "send HTTP/2 frames failed" : err);
                stop = true;
              } else {
                cancel_expired_locked();
              }
            }
          }
          if (stop)
            break;
        }
      }

      void cancel_expired_locked() {
        const i64 now = monotonic_ms();
        std::vector<i32> expired;
        {
          std::lock_guard<std::mutex> state_lock(state_mutex_);
          for (const auto& [stream_id, state] : responses_) {
            if (state->complete || state->failed || state->timeout_ms <= 0)
              continue;
            if (now - state->start_ms > state->timeout_ms)
              expired.push_back(stream_id);
          }
        }
        for (i32 stream_id : expired)
          (void)cancel_locked(stream_id, NGHTTP2_CANCEL, "ERR_HTTP2_STREAM_TIMEOUT");
      }

      bool cancel_locked(i32 stream_id, u32 error_code, std::string error_text) {
        const int rv =
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id, error_code);
        if (rv != 0) {
          set_last_error(describe_nghttp2_error(rv, "submit HTTP/2 RST_STREAM"));
          return false;
        }
        request_bodies_.erase(stream_id);
        std::string err;
        if (!flush_locked(err)) {
          if (!err.empty())
            set_last_error(err);
          return false;
        }
        finish_stream(stream_id, std::move(error_text));
        return true;
      }

      void finish_stream(i32 stream_id, std::string error_text) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = responses_.find(stream_id);
        if (it == responses_.end())
          return;
        auto& state = *it->second;
        if (state.complete || state.failed)
          return;
        state.complete = true;
        state.failed = true;
        state.error = std::move(error_text);
        state.cv.notify_all();
      }

      void fail_all_pending(const std::string& message) {
        set_last_error(message);
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& [_, state] : responses_) {
          if (state->complete || state->failed)
            continue;
          state->complete = true;
          state->failed = true;
          state->error = message;
          state->cv.notify_all();
        }
        pushed_responses_.clear();
      }

      bool flush_locked(std::string& err) {
        while (!closed_.load() && session_ && nghttp2_session_want_write(session_)) {
          const int rv = nghttp2_session_send(session_);
          if (rv < 0) {
            err = describe_nghttp2_error(rv, "send HTTP/2 frames");
            set_last_error(err);
            return false;
          }
        }
        return true;
      }

      bool set_local_window_size_locked(i32 stream_id, i32 window_size, std::string& err,
                                        std::string_view action) {
        const int rv = nghttp2_session_set_local_window_size(session_, NGHTTP2_FLAG_NONE, stream_id,
                                                             window_size);
        if (rv != 0) {
          err = describe_nghttp2_error(rv, action);
          set_last_error(err);
          return false;
        }
        return flush_locked(err);
      }

      void refuse_promised_stream(nghttp2_session* session, i32 stream_id, u32 error_code) {
        const int rv = nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, error_code);
        if (rv != 0)
          set_last_error(describe_nghttp2_error(rv, "submit HTTP/2 pushed-stream RST_STREAM"));
      }

      bool prepare_push_delivery_locked(pushed_stream_state& state, http2_pushed& pushed,
                                        push_handler& handler) {
        if (state.delivered || state.refused || !state.complete || !state.response_started ||
            !push_handler_)
          return false;
        if (state.promised_request.method.empty())
          state.promised_request.method = "GET";
        if (state.promised_request.path.empty())
          state.promised_request.path = "/";
        state.delivered = true;
        handler = push_handler_;
        pushed.stream_id = state.stream_id;
        pushed.associated_stream_id = state.associated_stream_id;
        pushed.promised_request = state.promised_request;
        pushed.response = state.response;
        return true;
      }

      void clear_last_error() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_.clear();
      }

      void set_last_error(std::string error) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = std::move(error);
      }

      bool fail(std::string& err, std::string message) {
        set_last_error(std::move(message));
        err = last_error();
        return false;
      }

      std::unique_ptr<tls_client> stream_;
      std::string host_;
      http2_settings settings_;
      nghttp2_session_callbacks* callbacks_ = nullptr;
      nghttp2_option* option_ = nullptr;
      nghttp2_session* session_ = nullptr;
      std::atomic<bool> closed_{false};
      std::map<i32, std::shared_ptr<stream_response_state>> responses_;
      std::map<i32, std::shared_ptr<pushed_stream_state>> pushed_responses_;
      std::map<i32, body_state> request_bodies_;
      std::thread worker_thread_;
      mutable std::mutex state_mutex_;
      std::mutex session_mutex_;
      push_handler push_handler_;
      on_data_consumed on_data_consumed_;
      std::string last_error_;
    };
  } // namespace

  http2_client::~http2_client() = default;

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, u16 port,
                                                      std::string& err) {
    return connect(host, port, std::string{}, true, http2_settings{}, std::string{}, err);
  }

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, u16 port,
                                                      const http2_settings& settings,
                                                      std::string& err) {
    return connect(host, port, std::string{}, true, settings, std::string{}, err);
  }

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, u16 port,
                                                      const std::string& ca_pem,
                                                      bool reject_unauthorized, std::string& err) {
    return connect(host, port, ca_pem, reject_unauthorized, http2_settings{}, std::string{}, err);
  }

  std::unique_ptr<http2_client> http2_client::connect(const std::string& host, u16 port,
                                                      const std::string& ca_pem,
                                                      bool reject_unauthorized,
                                                      const http2_settings& settings,
                                                      std::string& err) {
    return connect(host, port, ca_pem, reject_unauthorized, settings, std::string{}, err);
  }

  std::unique_ptr<http2_client>
  http2_client::connect(const std::string& host, u16 port, const std::string& ca_pem,
                        bool reject_unauthorized, const http2_settings& settings,
                        std::string session_namespace, std::string& err) {
    tls_options options;
    options.host = host;
    options.port = port;
    options.ca_pem = ca_pem;
    options.reject_unauthorized = reject_unauthorized;
    options.alpn = {"h2"};
    options.session_namespace = std::move(session_namespace);
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
