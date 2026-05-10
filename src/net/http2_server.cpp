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
#include <future>
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

    bool is_forbidden_user_header(std::string_view name) {
      return name == ":method" || name == ":path" || name == ":scheme" || name == ":authority" ||
             is_connection_specific(name);
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

    std::optional<std::string>
    find_header_value(const std::vector<std::pair<std::string, std::string>>& headers,
                      std::string_view name) {
      for (const auto& [header_name, header_value] : headers) {
        if (lower_ascii(header_name) == name)
          return header_value;
      }
      return std::nullopt;
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
      std::function<i32(const http2_request&, const http2_response&, std::string&)> push_cb;
      std::function<bool(i32, std::string&)> set_window_cb;
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

      i32 submit_push_promise(i32 parent_stream_id, const http2_request& promised_request,
                              const http2_response& promised_response, std::string& err) override {
        clear_last_error();
        err.clear();
        std::shared_ptr<response_slot> slot;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = slots_by_stream_.find(parent_stream_id);
          if (it != slots_by_stream_.end())
            slot = it->second;
        }
        if (!slot || !slot->push_cb) {
          err = "unknown HTTP/2 stream id";
          set_last_error(err);
          return -1;
        }
        const i32 pushed_stream_id = slot->push_cb(promised_request, promised_response, err);
        if (pushed_stream_id < 0 && err.empty())
          err = last_error();
        return pushed_stream_id;
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
          for (const auto& [_, slot] : slots_by_request_)
            slots.push_back(slot);
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

      void set_on_data_consumed(on_data_consumed handler) override {
        std::lock_guard<std::mutex> lock(mutex_);
        on_data_consumed_ = std::move(handler);
      }

      void set_stream_window(i32 stream_id, i32 window_size) override {
        clear_last_error();
        std::shared_ptr<response_slot> slot;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = slots_by_stream_.find(stream_id);
          if (it != slots_by_stream_.end())
            slot = it->second;
        }
        if (!slot || !slot->set_window_cb) {
          set_last_error("unknown HTTP/2 stream id");
          return;
        }
        std::string err;
        if (!slot->set_window_cb(window_size, err)) {
          if (err.empty())
            err = last_error();
          if (!err.empty())
            set_last_error(err);
        }
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

      on_data_consumed data_consumed_handler() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return on_data_consumed_;
      }

      std::shared_ptr<response_slot>
      enqueue(http2_incoming_request request, std::function<bool(i32, u32)> cancel_cb,
              std::function<i32(const http2_request&, const http2_response&, std::string&)> push_cb,
              std::function<bool(i32, std::string&)> set_window_cb) {
        auto slot = std::make_shared<response_slot>();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          request.id = next_request_id_++;
          slot->request_id = request.id;
          slot->stream_id = request.stream_id;
          slot->cancel_cb = std::move(cancel_cb);
          slot->push_cb = std::move(push_cb);
          slot->set_window_cb = std::move(set_window_cb);
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
          session_thread_id_ = std::this_thread::get_id();
          if (nghttp2_session_callbacks_new(&callbacks_) != 0)
            return;
          nghttp2_session_callbacks_set_send_callback(callbacks_, send_callback);
          nghttp2_session_callbacks_set_recv_callback(callbacks_, recv_callback);
          nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_, on_begin_headers);
          nghttp2_session_callbacks_set_on_header_callback(callbacks_, on_header);
          nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_, on_data_chunk);
          nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, on_frame_recv);
          nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_, on_stream_close);
          nghttp2_option_new(&option_);
          if (option_)
            nghttp2_option_set_no_auto_window_update(option_, 1);
          if (nghttp2_session_server_new2(&session_, callbacks_, this, option_) != 0)
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
            if (owner_.settings_.stream_max_recv_window_size &&
                !set_local_window_size_locked(
                    0, static_cast<i32>(*owner_.settings_.stream_max_recv_window_size),
                    "set HTTP/2 server connection window"))
              return;
          }

          while (!owner_.is_closed()) {
            int rv = 0;
            bool stop = false;
            {
              std::unique_lock<std::mutex> session_lock(session_mutex_);
              if (!session_)
                break;
              current_session_lock_ = &session_lock;
              process_pending_tasks_locked();
              rv = nghttp2_session_recv(session_);
              current_session_lock_ = nullptr;
              if (rv == NGHTTP2_ERR_EOF) {
                stop = true;
              } else if (rv < 0 && rv != NGHTTP2_ERR_WOULDBLOCK) {
                owner_.set_last_error(describe_nghttp2_error(rv, "receive HTTP/2 frames"));
                stop = true;
              } else {
                process_pending_tasks_locked();
                if (!flush_locked()) {
                  owner_.set_last_error("send HTTP/2 frames failed");
                  stop = true;
                }
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

        i32 submit_push_promise(i32 parent_stream_id, const http2_request& promised_request,
                                const http2_response& promised_response, std::string& err) {
          if (std::this_thread::get_id() == session_thread_id_)
            return submit_push_promise_locked(parent_stream_id, promised_request, promised_response,
                                              err);

          auto promise = std::make_shared<std::promise<i32>>();
          auto future = promise->get_future();
          {
            std::lock_guard<std::mutex> session_lock(session_mutex_);
            if (!session_) {
              err = "HTTP/2 session is closed";
              owner_.set_last_error(err);
              return -1;
            }
            pending_tasks_.emplace_back([this, promise, parent_stream_id, promised_request,
                                         promised_response, &err]() mutable {
              promise->set_value(submit_push_promise_locked(parent_stream_id, promised_request,
                                                            promised_response, err));
            });
          }
          return future.get();
        }

        bool set_stream_window(i32 stream_id, i32 window_size, std::string& err) {
          if (std::this_thread::get_id() == session_thread_id_) {
            const bool ok = set_local_window_size_locked(stream_id, window_size,
                                                         "set HTTP/2 server stream window");
            if (!ok)
              err = owner_.last_error();
            return ok;
          }

          auto promise = std::make_shared<std::promise<bool>>();
          auto future = promise->get_future();
          {
            std::lock_guard<std::mutex> session_lock(session_mutex_);
            if (!session_) {
              err = "HTTP/2 session is closed";
              owner_.set_last_error(err);
              return false;
            }
            pending_tasks_.emplace_back([this, promise, stream_id, window_size, &err]() mutable {
              const bool ok = set_local_window_size_locked(stream_id, window_size,
                                                           "set HTTP/2 server stream window");
              if (!ok)
                err = owner_.last_error();
              promise->set_value(ok);
            });
          }
          return future.get();
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

        static int on_data_chunk(nghttp2_session* session, u8, i32 stream_id, const u8* data,
                                 usize len, void* user_data) {
          auto* self = static_cast<session_worker*>(user_data);
          self->requests_[stream_id].body.append(reinterpret_cast<const char*>(data), len);
          const int consume_rv = nghttp2_session_consume(session, stream_id, len);
          if (consume_rv != 0)
            self->owner_.set_last_error(describe_nghttp2_error(consume_rv, "consume HTTP/2 data"));
          auto callback = self->owner_.data_consumed_handler();
          if (callback)
            callback(stream_id, len);
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
          auto slot = owner_.enqueue(
              std::move(request),
              [this](i32 stream_id, u32 error_code) {
                return cancel_stream(stream_id, error_code);
              },
              [this](const http2_request& promised_request, const http2_response& promised_response,
                     std::string& err) {
                return submit_push_promise(current_stream_id_, promised_request, promised_response,
                                           err);
              },
              [this](i32 window_size, std::string& err) {
                return set_stream_window(current_stream_id_, window_size, err);
              });
          // While waiting for the application handler to produce a response we
          // must (a) release the outer session_mutex_ so other threads can
          // call submit_push_promise / set_stream_window / cancel_stream, and
          // (b) periodically drain pending_tasks_ so those out-of-thread
          // submissions actually reach nghttp2 (otherwise the app handler
          // blocks on a future that this thread is supposed to satisfy).
          std::unique_lock<std::mutex> lock(slot->mutex);
          auto* outer = current_session_lock_;
          while (!slot->ready && !slot->closed && !owner_.is_closed()) {
            if (outer && outer->owns_lock()) {
              process_pending_tasks_locked();
              (void)flush_locked();
              outer->unlock();
            }
            slot->cv.wait_for(lock, std::chrono::milliseconds(5));
            if (outer && !outer->owns_lock())
              outer->lock();
          }
          if (!slot->ready)
            return false;
          auto response = slot->response;
          lock.unlock();
          return submit_response(current_stream_id_, response, "submit HTTP/2 response");
        }

        bool submit_response(i32 stream_id, const http2_response& response,
                             std::string_view action) {
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
            response_bodies_[stream_id] = body_state{response.body, 0};
            provider.read_callback = response_body_read;
            provider_ptr = &provider;
          }
          const int rv =
              nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), provider_ptr);
          if (rv != 0) {
            owner_.set_last_error(describe_nghttp2_error(rv, action));
            response_bodies_.erase(stream_id);
            return false;
          }
          return flush_locked();
        }

        i32 submit_push_promise_locked(i32 parent_stream_id, const http2_request& promised_request,
                                       const http2_response& promised_response, std::string& err) {
          err.clear();
          if (!session_) {
            err = "HTTP/2 session is closed";
            owner_.set_last_error(err);
            return -1;
          }
          if (nghttp2_session_get_remote_settings(session_, NGHTTP2_SETTINGS_ENABLE_PUSH) == 0) {
            err = "peer disabled push";
            owner_.set_last_error(err);
            return -1;
          }

          const std::string method =
              promised_request.method.empty() ? "GET" : promised_request.method;
          const std::string path = promised_request.path.empty() ? "/" : promised_request.path;
          const std::string scheme =
              find_header_value(promised_request.headers, ":scheme").value_or("https");
          auto authority = find_header_value(promised_request.headers, ":authority");
          if (!authority)
            authority = find_header_value(promised_request.headers, "host");
          if (!authority || authority->empty()) {
            err = "push promise requires :authority header";
            owner_.set_last_error(err);
            return -1;
          }

          std::vector<std::pair<std::string, std::string>> storage;
          storage.emplace_back(":method", method);
          storage.emplace_back(":path", path);
          storage.emplace_back(":scheme", scheme);
          storage.emplace_back(":authority", *authority);
          for (const auto& [name, value] : promised_request.headers) {
            auto lowered = lower_ascii(name);
            if (lowered == ":scheme" || lowered == ":authority")
              continue;
            if (is_forbidden_user_header(lowered))
              continue;
            storage.emplace_back(std::move(lowered), value);
          }

          std::vector<nghttp2_nv> request_nva;
          request_nva.reserve(storage.size());
          for (const auto& [name, value] : storage)
            request_nva.push_back(make_nv(name, value));

          const i32 promised_stream_id =
              nghttp2_submit_push_promise(session_, NGHTTP2_FLAG_NONE, parent_stream_id,
                                          request_nva.data(), request_nva.size(), nullptr);
          if (promised_stream_id < 0) {
            err = describe_nghttp2_error(promised_stream_id, "submit HTTP/2 push promise");
            owner_.set_last_error(err);
            return -1;
          }

          if (!submit_response(promised_stream_id, promised_response,
                               "submit HTTP/2 pushed response")) {
            err = owner_.last_error();
            return -1;
          }
          return promised_stream_id;
        }

        void process_pending_tasks_locked() {
          while (!pending_tasks_.empty()) {
            auto task = std::move(pending_tasks_.front());
            pending_tasks_.pop_front();
            task();
          }
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

        bool set_local_window_size_locked(i32 stream_id, i32 window_size, std::string_view action) {
          if (!session_)
            return false;
          const int rv = nghttp2_session_set_local_window_size(session_, NGHTTP2_FLAG_NONE,
                                                               stream_id, window_size);
          if (rv != 0) {
            owner_.set_last_error(describe_nghttp2_error(rv, action));
            return false;
          }
          return flush_locked();
        }

        bool handle_request_with_stream(http2_incoming_request request) {
          current_stream_id_ = request.stream_id;
          return handle_request(std::move(request));
        }

        void cleanup() {
          std::lock_guard<std::mutex> session_lock(session_mutex_);
          process_pending_tasks_locked();
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
          if (stream_)
            stream_->close();
        }

        http2_server_impl& owner_;
        std::unique_ptr<tls_client> stream_;
        nghttp2_session_callbacks* callbacks_ = nullptr;
        nghttp2_option* option_ = nullptr;
        nghttp2_session* session_ = nullptr;
        std::mutex session_mutex_;
        std::thread::id session_thread_id_{};
        std::deque<std::function<void()>> pending_tasks_;
        std::map<i32, http2_incoming_request> requests_;
        std::map<i32, body_state> response_bodies_;
        i32 current_stream_id_ = 0;
        std::unique_lock<std::mutex>* current_session_lock_ = nullptr;
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
      on_data_consumed on_data_consumed_;
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
