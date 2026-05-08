#include "net/http2_server.hpp"

#include "net/tls_server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <fxe/types.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <nghttp2/nghttp2.h>
#include <optional>
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

    std::string lower_ascii(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }

    bool is_connection_specific(std::string_view name) {
      return name == "connection" || name == "upgrade" || name == "http2-settings" ||
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

    struct response_slot {
      std::mutex mutex;
      std::condition_variable cv;
      bool ready = false;
      bool closed = false;
      int64_t deadline_ms = 0;
      u64 request_id = 0;
      i32 stream_id = 0;
      http2_response response;
      std::function<bool(i32, u32)> cancel_cb;
    };

    struct queued_request {
      http2_incoming_request request;
      std::shared_ptr<response_slot> slot;
    };

    struct body_state {
      std::string body;
      usize offset = 0;
    };

    class http2_server_impl final : public http2_server {
    public:
      http2_server_impl(std::unique_ptr<tls_server> listener, http2_settings settings)
          : listener_(std::move(listener)), settings_(std::move(settings)) {}

      ~http2_server_impl() override {
        close();
      }

      bool start() {
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
      }

      u16 local_port() const override {
        clear_last_error();
        return listener_ ? listener_->local_port() : 0;
      }

      std::optional<http2_incoming_request> poll(std::string& err) override {
        clear_last_error();
        err.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty())
          return std::nullopt;
        auto request = std::move(queue_.front().request);
        queue_.pop_front();
        return request;
      }

      bool respond(u64 request_id, const http2_response& response, std::string& err) override {
        clear_last_error();
        err.clear();
        std::shared_ptr<response_slot> slot;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          slot = remove_slot_by_request_locked(request_id);
        }
        if (!slot) {
          err = "unknown HTTP/2 request id";
          set_last_error(err);
          return false;
        }
        {
          std::lock_guard<std::mutex> lock(slot->mutex);
          slot->response = response;
          slot->ready = true;
        }
        slot->cv.notify_one();
        return true;
      }

      bool cancel(i32 stream_id, u32 error_code = NGHTTP2_CANCEL) override {
        clear_last_error();
        std::shared_ptr<response_slot> slot;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          slot = remove_slot_by_stream_locked(stream_id);
        }
        if (!slot) {
          set_last_error("unknown HTTP/2 stream id");
          return false;
        }
        bool ok = true;
        if (slot->cancel_cb)
          ok = slot->cancel_cb(stream_id, error_code);
        {
          std::lock_guard<std::mutex> lock(slot->mutex);
          slot->closed = true;
        }
        slot->cv.notify_all();
        if (!ok && last_error().empty())
          set_last_error("failed to cancel HTTP/2 stream");
        return ok;
      }

      void close() override {
        clear_last_error();
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true))
          return;
        if (listener_)
          listener_->close();

        std::vector<std::shared_ptr<response_slot>> slots;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          for (const auto& [_, slot] : slots_by_request_) {
            slots.push_back(slot);
          }
          slots_by_request_.clear();
          slots_by_stream_.clear();
          queue_.clear();
        }
        for (const auto& slot : slots) {
          std::lock_guard<std::mutex> lock(slot->mutex);
          slot->closed = true;
          slot->cv.notify_all();
        }
        if (accept_thread_.joinable())
          accept_thread_.join();
        for (auto& worker : workers_) {
          if (worker.joinable())
            worker.join();
        }
      }

      std::string last_error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
      }

      bool is_closed() const {
        return closed_.load();
      }

      void set_last_error(std::string error) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = std::move(error);
      }

      void clear_last_error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_.clear();
      }

      std::shared_ptr<response_slot> enqueue(http2_incoming_request request,
                                             std::function<bool(i32, u32)> cancel_cb) {
        auto slot = std::make_shared<response_slot>();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          request.id = next_request_id_++;
          slot->request_id = request.id;
          slot->stream_id = request.stream_id;
          slot->cancel_cb = std::move(cancel_cb);
          slots_by_request_[request.id] = slot;
          slots_by_stream_[request.stream_id] = slot;
          queue_.push_back(queued_request{std::move(request), slot});
        }
        return slot;
      }

      void finish_stream(i32 stream_id) {
        std::shared_ptr<response_slot> slot;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          slot = remove_slot_by_stream_locked(stream_id);
        }
        if (!slot)
          return;
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->closed = true;
        slot->cv.notify_all();
      }

      std::vector<i32> expired_streams() {
        const i64 now = monotonic_ms();
        std::vector<i32> out;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [stream_id, slot] : slots_by_stream_) {
          if (slot->deadline_ms != 0 && now > slot->deadline_ms)
            out.push_back(stream_id);
        }
        return out;
      }

    private:
      class session_worker {
      public:
        session_worker(http2_server_impl& owner, std::unique_ptr<tls_client> stream)
            : owner_(owner), stream_(std::move(stream)) {}

        ~session_worker() {
          cleanup();
        }

        void run() {
          if (nghttp2_session_callbacks_new(&callbacks_) != 0)
            return;
          nghttp2_session_callbacks_set_send_callback(callbacks_, send_callback);
          nghttp2_session_callbacks_set_recv_callback(callbacks_, recv_callback);
          nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_, on_begin_headers);
          nghttp2_session_callbacks_set_on_header_callback(callbacks_, on_header);
          nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_, on_data_chunk);
          nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, on_frame_recv);
          nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_, on_stream_close);
          if (nghttp2_session_server_new(&session_, callbacks_, this) != 0)
            return;
          const auto entries = settings_entries(owner_.settings_);
          {
            std::lock_guard<std::mutex> session_lock(session_mutex_);
            const int settings_rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE,
                                                            entries.data(), entries.size());
            if (settings_rv != 0) {
              owner_.set_last_error(
                  describe_nghttp2_error(settings_rv, "submit HTTP/2 server settings"));
              return;
            }
            if (!flush_locked())
              return;
          }

          while (!owner_.is_closed()) {
            int rv = 0;
            bool stop = false;
            {
              std::lock_guard<std::mutex> session_lock(session_mutex_);
              if (!session_)
                break;
              rv = nghttp2_session_recv(session_);
              if (rv == NGHTTP2_ERR_EOF) {
                stop = true;
              } else if (rv < 0 && rv != NGHTTP2_ERR_WOULDBLOCK) {
                owner_.set_last_error(describe_nghttp2_error(rv, "receive HTTP/2 frames"));
                stop = true;
              } else if (!flush_locked()) {
                owner_.set_last_error("send HTTP/2 frames failed");
                stop = true;
              }
            }
            for (i32 stream_id : owner_.expired_streams())
              (void)owner_.cancel(stream_id, NGHTTP2_CANCEL);
            if (stop)
              break;
          }
        }

        bool cancel_stream(i32 stream_id, u32 error_code) {
          std::lock_guard<std::mutex> session_lock(session_mutex_);
          if (!session_)
            return false;
          const int rv =
              nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id, error_code);
          if (rv != 0) {
            owner_.set_last_error(describe_nghttp2_error(rv, "submit HTTP/2 RST_STREAM"));
            return false;
          }
          return flush_locked();
        }

      private:
        static ssize_t send_callback(nghttp2_session*, const u8* data, usize length, int,
                                     void* user_data) {
          auto* self = static_cast<session_worker*>(user_data);
          const ssize_t written = self->stream_->write(data, length);
          if (written < 0)
            return NGHTTP2_ERR_CALLBACK_FAILURE;
          return written;
        }

        static ssize_t recv_callback(nghttp2_session*, u8* data, usize length, int,
                                     void* user_data) {
          auto* self = static_cast<session_worker*>(user_data);
          const ssize_t got = self->stream_->read_with_timeout(data, length, kReadPollTimeoutMs);
          if (got == tls_client::read_timed_out)
            return NGHTTP2_ERR_WOULDBLOCK;
          if (got == 0)
            return NGHTTP2_ERR_EOF;
          if (got < 0)
            return NGHTTP2_ERR_CALLBACK_FAILURE;
          return got;
        }

        static int on_begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
          if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
            return 0;
          auto* self = static_cast<session_worker*>(user_data);
          self->requests_[frame->hd.stream_id] = http2_incoming_request{};
          self->requests_[frame->hd.stream_id].stream_id = frame->hd.stream_id;
          return 0;
        }

        static int on_header(nghttp2_session*, const nghttp2_frame* frame, const u8* name,
                             usize namelen, const u8* value, usize valuelen, u8, void* user_data) {
          if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
            return 0;
          auto* self = static_cast<session_worker*>(user_data);
          auto& request = self->requests_[frame->hd.stream_id];
          std::string header_name(reinterpret_cast<const char*>(name), namelen);
          std::string header_value(reinterpret_cast<const char*>(value), valuelen);
          if (header_name == ":method")
            request.method = std::move(header_value);
          else if (header_name == ":path")
            request.path = std::move(header_value);
          else if (!header_name.empty() && header_name.front() != ':')
            request.headers.emplace_back(std::move(header_name), std::move(header_value));
          return 0;
        }

        static int on_data_chunk(nghttp2_session*, u8, i32 stream_id, const u8* data, usize len,
                                 void* user_data) {
          auto* self = static_cast<session_worker*>(user_data);
          self->requests_[stream_id].body.append(reinterpret_cast<const char*>(data), len);
          return 0;
        }

        static int on_frame_recv(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
          if ((frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) ||
              (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0)
            return 0;
          auto* self = static_cast<session_worker*>(user_data);
          auto it = self->requests_.find(frame->hd.stream_id);
          if (it == self->requests_.end())
            return 0;
          auto request = std::move(it->second);
          self->requests_.erase(it);
          return self->handle_request_with_stream(std::move(request))
                     ? 0
                     : NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        static int on_stream_close(nghttp2_session*, i32 stream_id, u32, void* user_data) {
          auto* self = static_cast<session_worker*>(user_data);
          self->requests_.erase(stream_id);
          self->response_bodies_.erase(stream_id);
          self->owner_.finish_stream(stream_id);
          return 0;
        }

        static ssize_t response_body_read(nghttp2_session*, i32 stream_id, u8* buf, usize length,
                                          u32* data_flags, nghttp2_data_source*, void* user_data) {
          auto* self = static_cast<session_worker*>(user_data);
          auto it = self->response_bodies_.find(stream_id);
          if (it == self->response_bodies_.end()) {
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

        bool handle_request(http2_incoming_request request) {
          if (request.method.empty())
            request.method = "GET";
          if (request.path.empty())
            request.path = "/";
          auto slot = owner_.enqueue(std::move(request), [this](i32 stream_id, u32 error_code) {
            return cancel_stream(stream_id, error_code);
          });
          std::unique_lock<std::mutex> lock(slot->mutex);
          slot->cv.wait(lock, [&] { return slot->ready || slot->closed || owner_.is_closed(); });
          if (!slot->ready)
            return false;
          auto response = slot->response;
          lock.unlock();
          return submit_response(response);
        }

        bool submit_response(const http2_response& response) {
          std::vector<std::pair<std::string, std::string>> storage;
          storage.emplace_back(":status",
                               std::to_string(response.status == 0 ? 200 : response.status));
          for (const auto& [name, value] : response.headers) {
            auto lowered = lower_ascii(name);
            if (!lowered.empty() && lowered.front() == ':')
              continue;
            if (is_connection_specific(lowered))
              continue;
            storage.emplace_back(std::move(lowered), value);
          }
          if (!response.body.empty())
            storage.emplace_back("content-length", std::to_string(response.body.size()));
          std::vector<nghttp2_nv> nva;
          nva.reserve(storage.size());
          for (const auto& [name, value] : storage)
            nva.push_back(make_nv(name, value));

          nghttp2_data_provider provider{};
          nghttp2_data_provider* provider_ptr = nullptr;
          if (!response.body.empty()) {
            response_bodies_[current_stream_id_] = body_state{response.body, 0};
            provider.read_callback = response_body_read;
            provider_ptr = &provider;
          }
          const int rv = nghttp2_submit_response(session_, current_stream_id_, nva.data(),
                                                 nva.size(), provider_ptr);
          if (rv != 0) {
            owner_.set_last_error(describe_nghttp2_error(rv, "submit HTTP/2 response"));
            return false;
          }
          return flush_locked();
        }

        bool flush_locked() {
          while (!owner_.is_closed() && session_ && nghttp2_session_want_write(session_)) {
            const int rv = nghttp2_session_send(session_);
            if (rv < 0) {
              owner_.set_last_error(describe_nghttp2_error(rv, "send HTTP/2 frames"));
              return false;
            }
          }
          return true;
        }

        bool handle_request_with_stream(http2_incoming_request request) {
          current_stream_id_ = request.stream_id;
          return handle_request(std::move(request));
        }

        void cleanup() {
          if (session_) {
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

        http2_server_impl& owner_;
        std::unique_ptr<tls_client> stream_;
        nghttp2_session_callbacks* callbacks_ = nullptr;
        nghttp2_session* session_ = nullptr;
        std::mutex session_mutex_;
        std::map<i32, http2_incoming_request> requests_;
        std::map<i32, body_state> response_bodies_;
        i32 current_stream_id_ = 0;
      };

      std::shared_ptr<response_slot> remove_slot_by_request_locked(u64 request_id) {
        auto it = slots_by_request_.find(request_id);
        if (it == slots_by_request_.end())
          return nullptr;
        auto slot = it->second;
        slots_by_request_.erase(it);
        slots_by_stream_.erase(slot->stream_id);
        erase_queued_request_locked(slot->request_id);
        return slot;
      }

      std::shared_ptr<response_slot> remove_slot_by_stream_locked(i32 stream_id) {
        auto it = slots_by_stream_.find(stream_id);
        if (it == slots_by_stream_.end())
          return nullptr;
        auto slot = it->second;
        slots_by_stream_.erase(it);
        slots_by_request_.erase(slot->request_id);
        erase_queued_request_locked(slot->request_id);
        return slot;
      }

      void erase_queued_request_locked(u64 request_id) {
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const queued_request& entry) {
          return entry.request.id == request_id;
        });
        if (it != queue_.end())
          queue_.erase(it);
      }

      void accept_loop() {
        while (!closed_.load()) {
          std::string err;
          auto stream = listener_->accept(err);
          if (!stream) {
            if (closed_.load())
              break;
            if (!err.empty())
              set_last_error(err);
            continue;
          }
          workers_.emplace_back([this, stream = std::move(stream)]() mutable {
            session_worker worker(*this, std::move(stream));
            worker.run();
          });
        }
      }

      std::unique_ptr<tls_server> listener_;
      http2_settings settings_;
      std::atomic<bool> closed_{false};
      mutable std::mutex mutex_;
      std::deque<queued_request> queue_;
      std::map<u64, std::shared_ptr<response_slot>> slots_by_request_;
      std::map<i32, std::shared_ptr<response_slot>> slots_by_stream_;
      u64 next_request_id_ = 1;
      std::thread accept_thread_;
      std::vector<std::thread> workers_;
      mutable std::string last_error_;
    };
  } // namespace

  http2_server::~http2_server() = default;

  std::unique_ptr<http2_server> http2_server::listen(const http2_server_options& options,
                                                     std::string& err) {
    tls_server_options tls_options;
    tls_options.cert_pem = options.cert_pem;
    tls_options.key_pem = options.key_pem;
    tls_options.port = options.port;
    tls_options.alpn = options.alpn.empty() ? std::vector<std::string>{"h2"} : options.alpn;
    auto tls = tls_server::listen(tls_options, err);
    if (!tls)
      return nullptr;
    auto server = std::make_unique<http2_server_impl>(std::move(tls), options.settings);
    if (!server->start()) {
      err = "failed to start HTTP/2 accept loop";
      return nullptr;
    }
    return server;
  }
} // namespace fxe::net
