// fxe debug server — NDJSON or CDP WebSocket over TCP.
//
// Layout:
//   * server::impl owns a listening socket and an accept thread.
//   * Each accepted protocol connection becomes an explicit session with its
//     own reader thread, writer thread, outbox, and event subscriptions.
//   * Readers parse JSON-RPC/CDP payloads, validate the method, and push a
//     `pending_call` tagged with the origin session onto the MPSC queue
//     consumed by pump_tasks() on the render thread.
//   * Replies are produced on the render thread and routed only to the origin
//     session; events are broadcast to sessions whose per-session subscriptions
//     allow them.
//
// Robustness notes:
//   * If a client disconnects while tasks are queued, only that session's
//     pending calls are aborted; other sessions and their outboxes remain live.
//   * Bind errors leave the server in a not-running state; last_error()
//     reports the cause.
//
// Threading:
//   * Render thread calls pump_tasks(), is_paused(), emit_event/console,
//     attach_*, set_wake_callback, start, stop, dtor.
//   * Accept thread runs accept() loop and allocates sessions up to the
//     configured cap.
//   * Each session has a reader thread and writer thread.
// All shared server state is guarded by `mu` except atomics noted below.

#include <fxe/debug.hpp>
#include <fxe/types.hpp>

#include "cdp_ws.hpp"
#include "dispatch.hpp"
#include "server.hpp"
#include "server_internal.hpp"

#include <fxe/renderer.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/window.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <errno.h>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define close_socket closesocket
#define socket_errno() (WSAGetLastError())
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#define close_socket ::close
#define socket_errno() (errno)
#endif

namespace fxe::debug {
  namespace {

    bool set_socket_close_on_exec(socket_t s) {
#if defined(_WIN32)
      (void)s;
      return true;
#else
      int flags = ::fcntl(s, F_GETFD);
      if (flags < 0)
        return false;
      return ::fcntl(s, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
    }

#if !defined(_WIN32)
    // Avoid SIGPIPE on send() when the peer has closed.
    void set_socket_no_sigpipe(socket_t s) {
#if defined(SO_NOSIGPIPE)
      int yes = 1;
      ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#else
      (void)s; // Linux uses MSG_NOSIGNAL on send() instead.
#endif
    }
#endif

    int send_flags() {
#if defined(MSG_NOSIGNAL)
      return MSG_NOSIGNAL;
#else
      return 0;
#endif
    }

    std::string ws_authority(std::string_view host, u16 port) {
      std::string authority = host.empty() ? std::string("127.0.0.1") : std::string(host);
      if (authority.find(':') == std::string::npos)
        authority += ":" + std::to_string(port);
      return authority;
    }

    std::string ws_url(std::string_view host, u16 port, std::string_view id = "fxe-main") {
      return "ws://" + ws_authority(host, port) + "/devtools/page/" + std::string(id);
    }

    std::string format_peer_address(const sockaddr_in& peer) {
      char host[INET_ADDRSTRLEN] = {};
      const char* text = inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
      std::string out = text ? std::string(text) : std::string("<unknown>");
      out += ":";
      out += std::to_string(ntohs(peer.sin_port));
      return out;
    }

    std::string cdp_not_found_body(std::string_view path) {
      return "fxe debug server does not serve " + std::string(path) +
             ". Use Chrome's chrome://inspect remote target flow.";
    }
  } // namespace

  json make_cdp_target_descriptor(std::string_view host, u16 port, std::string_view id,
                                  std::string_view type, std::string_view title) {
    json out{json::object()};
    out["id"] = std::string(id);
    out["type"] = std::string(type);
    out["title"] = std::string(title);
    out["description"] = "Minimal discovery target for fxe.";
    out["url"] = std::string("fxe://debug");
    out["devtoolsFrontendUrl"] = "/devtools/inspector.html?ws=" + ws_authority(host, port) +
                                 "/devtools/page/" + std::string(id);
    out["webSocketDebuggerUrl"] = ws_url(host, port, id);
    out["protocol"] = std::string("cdp-websocket");
    out["host"] = std::string(host);
    out["port"] = static_cast<double>(port);
    return out;
  }

  json make_cdp_version_descriptor(std::string_view host, u16 port) {
    json out{json::object()};
    out["Browser"] = std::string("fxe/0.1.0");
    out["Protocol-Version"] = std::string("1.3");
    out["User-Agent"] = std::string("fxe-debug");
    out["V8-Version"] = std::string("not exposed");
    out["WebKit-Version"] = std::string("not applicable");
    out["webSocketDebuggerUrl"] = ws_url(host, port, "fxe-main");
    return out;
  }

  json make_cdp_version_descriptor() {
    return make_cdp_version_descriptor("127.0.0.1", 9229);
  }

  std::optional<std::string> make_cdp_discovery_http_response(std::string_view path,
                                                              std::string_view host, u16 port) {
    json body;
    if (path == "/json" || path == "/json/list") {
      body = json::array();
      body.push_back(make_cdp_target_descriptor(host, port));
    } else if (path == "/json/version") {
      body = make_cdp_version_descriptor(host, port);
    } else if (path == "/json/protocol") {
      body = json::object();
      body["version"] = {{"major", "1"}, {"minor", "3"}};
      body["domains"] = json::array();
    } else {
      return std::nullopt;
    }

    std::string payload = body.dump();
    return cdp_ws::http_response(200, "OK", "application/json; charset=utf-8", payload);
  }

  constexpr uint32_t event_channel_bit(event_channel c) {
    return uint32_t(1) << static_cast<int>(c);
  }

  std::optional<event_channel> channel_for_event_method(std::string_view method) {
    if (method.rfind("Window.", 0) == 0)
      return event_channel::window;
    if (method.rfind("Fetch.", 0) == 0)
      return event_channel::fetch;
    if (method.rfind("Fs.", 0) == 0)
      return event_channel::fs;
    if (method.rfind("Performance.", 0) == 0)
      return event_channel::perf;
    return std::nullopt;
  }

  struct outgoing_message {
    std::string line; // serialized JSON envelope, no transport delimiter
  };

  // -------------------------------------------------------------------------
  // Session state: one accepted NDJSON or WebSocket client.
  // -------------------------------------------------------------------------
  struct session {
    session_id id = 0;
    socket_t sock = kInvalidSocket;
    std::atomic<bool> is_websocket{false};
    std::optional<cdp_ws::http_request> initial_http_request;

    std::thread reader_thread;
    std::thread writer_thread;

    std::mutex close_mu;
    std::mutex outbox_mu;
    std::condition_variable outbox_cv;
    std::deque<outgoing_message> outbox;

    std::atomic<uint32_t> channel_mask{0};
    std::atomic<bool> socket_closed{false};
    std::atomic<bool> console_enabled{false};
    std::atomic<bool> alive{true};
  };

  // -------------------------------------------------------------------------
  // Pending request: parsed off the wire by a session reader, executed on the
  // render thread, and routed back to the origin session by id.
  // -------------------------------------------------------------------------
  struct pending_call {
    std::string method;
    json params;
    json id; // null when a notification (no reply expected)
    session_id origin = 0;
    std::weak_ptr<session> origin_session;
    std::atomic<bool> aborted{false};
    // If set, pump_tasks() must not run this call until steady_clock has
    // reached this point. Used to honour Page.screenshot delayMs without
    // blocking the render thread.
    std::optional<std::chrono::steady_clock::time_point> not_before{};
  };

  struct server::impl {
    server_options opts;
    std::atomic<bool> running{false};
    std::atomic<u16> port{0};
    std::string last_error;

    js::host* host = nullptr;
    fxe::window* win = nullptr;
    fxe::renderer* rdr = nullptr;
    std::function<void()> wake;

    // Listening socket + accept thread.
    socket_t listen_sock = kInvalidSocket;
    std::thread accept_thread;

    // Session state. `mu` protects session map, id allocation, and all_sessions_.
    std::mutex mu;
    session_id next_session_id = 1;
    std::unordered_map<session_id, std::shared_ptr<session>> sessions_;
    std::vector<std::shared_ptr<session>> all_sessions_;

    // Render-thread inbound queue.
    std::mutex inbox_mu;
    std::deque<std::shared_ptr<pending_call>> inbox;

    // Debugger flags. paused/step_once affect the single target process
    // globally: multiple clients are controlling one process, not isolated
    // debugging targets.
    std::atomic<bool> paused{false};
    std::atomic<bool> step_once{false};

    impl(server_options o) : opts(std::move(o)) {
      if (opts.max_clients == 0)
        opts.max_clients = 8;
      paused.store(opts.start_paused);
    }

    ~impl() {
      shutdown();
    }

    u16 max_sessions() const {
      return opts.max_clients == 0 ? 8 : opts.max_clients;
    }

    void shutdown() {
      bool was_running = running.exchange(false);
      if (!was_running)
        return;
      if (listen_sock != kInvalidSocket) {
        close_socket(listen_sock);
        listen_sock = kInvalidSocket;
      }

      std::vector<std::shared_ptr<session>> to_stop;
      {
        std::lock_guard<std::mutex> g(mu);
        to_stop = all_sessions_;
        sessions_.clear();
        all_sessions_.clear();
      }
      for (auto& sess : to_stop) {
        sess->alive.store(false);
        close_session_socket(*sess);
        sess->outbox_cv.notify_all();
      }
      abort_pending_calls();
      update_global_channel_mask(0);

      if (accept_thread.joinable())
        accept_thread.join();
      join_sessions(to_stop);
    }

    void abort_pending_calls(session_id id = 0) {
      std::deque<std::shared_ptr<pending_call>> aborted;
      {
        std::lock_guard<std::mutex> g(inbox_mu);
        if (id == 0) {
          aborted = std::move(inbox);
          inbox.clear();
        } else {
          std::deque<std::shared_ptr<pending_call>> retain;
          for (auto& call : inbox) {
            if (call->origin == id)
              aborted.push_back(std::move(call));
            else
              retain.push_back(std::move(call));
          }
          inbox = std::move(retain);
        }
      }
      for (auto& call : aborted)
        call->aborted.store(true);
    }

    void enqueue_outgoing(session_id id, std::string line) {
      std::shared_ptr<session> sess;
      {
        std::lock_guard<std::mutex> g(mu);
        auto it = sessions_.find(id);
        if (it == sessions_.end())
          return;
        sess = it->second;
      }
      if (!sess->alive.load())
        return;
      {
        std::lock_guard<std::mutex> g(sess->outbox_mu);
        if (!sess->alive.load())
          return;
        sess->outbox.push_back({std::move(line)});
      }
      sess->outbox_cv.notify_all();
    }

    bool event_allowed_for_session(const session& sess, std::string_view method) const {
      if (!sess.alive.load())
        return false;
      if (method == "Console.messageAdded")
        return sess.console_enabled.load(std::memory_order_acquire);
      if (auto channel = channel_for_event_method(method)) {
        return (sess.channel_mask.load(std::memory_order_acquire) & event_channel_bit(*channel)) !=
               0;
      }
      return true;
    }

    void broadcast_event(std::string_view method, std::string line) {
      std::vector<std::shared_ptr<session>> targets;
      {
        std::lock_guard<std::mutex> g(mu);
        targets.reserve(sessions_.size());
        for (auto& [_, sess] : sessions_) {
          if (event_allowed_for_session(*sess, method))
            targets.push_back(sess);
        }
      }
      for (auto& sess : targets) {
        {
          std::lock_guard<std::mutex> g(sess->outbox_mu);
          if (!sess->alive.load())
            continue;
          sess->outbox.push_back({line});
        }
        sess->outbox_cv.notify_all();
      }
    }

    void broadcast_event(std::string line) {
      broadcast_event("", std::move(line));
    }

    void set_session_console_enabled(session_id id, bool on) {
      std::shared_ptr<session> sess;
      {
        std::lock_guard<std::mutex> g(mu);
        auto it = sessions_.find(id);
        if (it == sessions_.end())
          return;
        sess = it->second;
      }
      sess->console_enabled.store(on, std::memory_order_release);
    }

    void set_all_console_enabled(bool on) {
      std::lock_guard<std::mutex> g(mu);
      for (auto& [_, sess] : sessions_)
        sess->console_enabled.store(on, std::memory_order_release);
    }

    void set_session_channel_enabled(session_id id, event_channel channel, bool enabled) {
      uint32_t mask = 0;
      {
        std::lock_guard<std::mutex> g(mu);
        auto it = sessions_.find(id);
        if (it == sessions_.end())
          return;
        auto& sess = *it->second;
        const uint32_t bit = event_channel_bit(channel);
        if (enabled)
          sess.channel_mask.fetch_or(bit, std::memory_order_acq_rel);
        else
          sess.channel_mask.fetch_and(~bit, std::memory_order_acq_rel);
        mask = aggregate_channel_mask_locked();
      }
      update_global_channel_mask(mask);
    }

    uint32_t aggregate_channel_mask_locked() const {
      uint32_t mask = 0;
      for (const auto& [_, sess] : sessions_)
        mask |= sess->channel_mask.load(std::memory_order_acquire);
      return mask;
    }

    void update_global_channel_mask(uint32_t mask) {
      set_channel_enabled(event_channel::window,
                          (mask & event_channel_bit(event_channel::window)) != 0);
      set_channel_enabled(event_channel::fetch,
                          (mask & event_channel_bit(event_channel::fetch)) != 0);
      set_channel_enabled(event_channel::fs, (mask & event_channel_bit(event_channel::fs)) != 0);
      set_channel_enabled(event_channel::perf,
                          (mask & event_channel_bit(event_channel::perf)) != 0);
    }

    // ----- accept thread -------------------------------------------------
    void accept_loop() {
      while (running.load()) {

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        socket_t s = ::accept(listen_sock, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (s == kInvalidSocket) {
          if (!running.load())
            break;
          int e = socket_errno();
          if (e == EINTR)
            continue;
          if (opts.log_level > 0)
            std::fprintf(stderr, "fxe.debug: accept failed: %d\n", e);
          continue;
        }
        set_socket_close_on_exec(s);
#if !defined(_WIN32)
        set_socket_no_sigpipe(s);
#endif
        const std::string peer_addr = format_peer_address(peer);

        bool full = false;
        {
          std::lock_guard<std::mutex> g(mu);
          full = sessions_.size() >= max_sessions();
        }
        if (full) {
          if (opts.log_level > 0)
            std::fprintf(stderr, "fxe.debug: rejecting client %s: server full\n",
                         peer_addr.c_str());
          if (peek_first_byte_nonblocking(s) == 'G') {
            cdp_ws::http_request req;
            std::string error;
            if (cdp_ws::read_http_request(s, req, error)) {
              auto authority = request_host_authority(req);
              if (auto response =
                      make_cdp_discovery_http_response(req.path, authority, port.load())) {
                send_http_response(s, *response);
              } else if (path_is_ws_endpoint(req.path) && !req.header("upgrade").empty()) {
                send_http_response(s, cdp_ws::http_response(
                                          503, "Service Unavailable", "text/plain; charset=utf-8",
                                          "fxe debug server is full; maximum clients attached."));
              } else {
                send_http_response(s, cdp_ws::http_response(404, "Not Found",
                                                            "text/plain; charset=utf-8",
                                                            cdp_not_found_body(req.path)));
              }
            } else {
              send_http_response(
                  s, cdp_ws::http_response(400, "Bad Request", "text/plain; charset=utf-8",
                                           error.empty() ? "bad HTTP request" : error));
            }
          } else {
            static const char kFull[] =
                "{\"error\":{\"code\":-32003,\"message\":\"server full\"}}\n";
            cdp_ws::send_all(s, kFull);
          }
          close_accepted_socket(s);
          continue;
        }

        auto sess = std::make_shared<session>();
        sess->sock = s;

        {
          std::lock_guard<std::mutex> g(mu);
          sess->id = next_session_id++;
          sessions_.emplace(sess->id, sess);
          all_sessions_.push_back(sess);
        }

        sess->writer_thread = std::thread(&impl::writer_loop, this, sess);
        sess->reader_thread = std::thread(&impl::session_loop, this, sess);
      }
    }

    // ----- session thread ------------------------------------------------
    int peek_first_byte(socket_t s) {
      char ch = 0;
#if defined(_WIN32)
      int n = ::recv(s, &ch, 1, MSG_PEEK);
#else
      ssize_t n = ::recv(s, &ch, 1, MSG_PEEK);
#endif
      return n == 1 ? static_cast<unsigned char>(ch) : -1;
    }

    int peek_first_byte_nonblocking(socket_t s) {
      char ch = 0;
#if defined(_WIN32)
      u_long one = 1;
      u_long zero = 0;
      ioctlsocket(s, FIONBIO, &one);
      int n = ::recv(s, &ch, 1, MSG_PEEK);
      ioctlsocket(s, FIONBIO, &zero);
#else
      int flags = ::fcntl(s, F_GETFL, 0);
      if (flags < 0)
        return -1;
      ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
      ssize_t n = ::recv(s, &ch, 1, MSG_PEEK);
      ::fcntl(s, F_SETFL, flags);
#endif
      return n == 1 ? static_cast<unsigned char>(ch) : -1;
    }

    std::string request_host_authority(const cdp_ws::http_request& req) const {
      auto authority = req.header("host");
      if (!authority.empty())
        return authority;
      return ws_authority(opts.host, port.load());
    }

    bool send_http_response(socket_t s, const std::string& response) {
      return cdp_ws::send_all(s, response);
    }

    void close_accepted_socket(socket_t s) {
      ::shutdown(s, 2);
      close_socket(s);
    }

    void close_session_socket(session& sess) {
      std::lock_guard<std::mutex> g(sess.close_mu);
      if (sess.sock != kInvalidSocket && !sess.socket_closed.exchange(true)) {
        ::shutdown(sess.sock, 2);
        close_socket(sess.sock);
      }
    }

    bool path_is_ws_endpoint(std::string_view path) const {
      return path == "/devtools/browser" || path == "/devtools/page/fxe-main";
    }

    bool handle_http_connection(const std::shared_ptr<session>& sess) {
      cdp_ws::http_request req;
      if (sess->initial_http_request) {
        req = *sess->initial_http_request;
      } else {
        std::string read_error;
        if (!cdp_ws::read_http_request(sess->sock, req, read_error)) {
          send_http_response(sess->sock, cdp_ws::http_response(
                                             400, "Bad Request", "text/plain; charset=utf-8",
                                             read_error.empty() ? "bad HTTP request" : read_error));
          return false;
        }
      }
      auto authority = request_host_authority(req);
      if (auto response = make_cdp_discovery_http_response(req.path, authority, port.load())) {
        send_http_response(sess->sock, *response);
        return false;
      }
      std::string error;
      if (path_is_ws_endpoint(req.path) && !req.header("upgrade").empty()) {
        if (!cdp_ws::handshake(sess->sock, req, error)) {
          send_http_response(
              sess->sock, cdp_ws::http_response(400, "Bad Request", "text/plain; charset=utf-8",
                                                error.empty() ? "bad WebSocket handshake" : error));
          return false;
        }
        sess->is_websocket.store(true);
        return true;
      }
      send_http_response(sess->sock,
                         cdp_ws::http_response(404, "Not Found", "text/plain; charset=utf-8",
                                               cdp_not_found_body(req.path)));
      return false;
    }

    void finish_session(const std::shared_ptr<session>& sess) {
      sess->alive.store(false);
      close_session_socket(*sess);
      sess->outbox_cv.notify_all();
      abort_pending_calls(sess->id);

      uint32_t mask = 0;
      bool removed = false;
      {
        std::lock_guard<std::mutex> g(mu);
        auto it = sessions_.find(sess->id);
        if (it != sessions_.end()) {
          sessions_.erase(it);
          removed = true;
          mask = aggregate_channel_mask_locked();
        }
      }
      if (removed)
        update_global_channel_mask(mask);
    }

    void join_sessions(std::vector<std::shared_ptr<session>>& sessions) {
      const auto self = std::this_thread::get_id();
      for (auto& sess : sessions) {
        if (sess->writer_thread.joinable() && sess->writer_thread.get_id() != self)
          sess->writer_thread.join();
        if (sess->reader_thread.joinable() && sess->reader_thread.get_id() != self)
          sess->reader_thread.join();
      }
      sessions.clear();
    }

    void session_loop(std::shared_ptr<session> sess) {
      if (peek_first_byte(sess->sock) == 'G' && !handle_http_connection(sess)) {
        finish_session(sess);
        return;
      }

      if (sess->is_websocket.load())
        websocket_read_loop(sess);
      else
        ndjson_read_loop(sess);
      finish_session(sess);
    }

    void ndjson_read_loop(const std::shared_ptr<session>& sess) {
      std::string buf;
      buf.reserve(4096);
      char chunk[4096];
      bool close_requested = false;
      while (running.load() && sess->alive.load()) {
#if defined(_WIN32)
        int n = ::recv(sess->sock, chunk, sizeof(chunk), 0);
#else
        ssize_t n = ::recv(sess->sock, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0)
          break;
        buf.append(chunk, static_cast<usize>(n));
        for (;;) {
          auto nl = buf.find('\n');
          if (nl == std::string::npos || close_requested)
            break;
          std::string line = buf.substr(0, nl);
          buf.erase(0, nl + 1);
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (line.empty())
            continue;
          close_requested = !handle_line(sess, std::move(line));
        }
        if (close_requested)
          break;
      }
    }

    void websocket_read_loop(const std::shared_ptr<session>& sess) {
      cdp_ws::reader r(true);
      while (running.load() && sess->alive.load()) {
        auto rr = r.read_text(sess->sock);
        if (rr.state == cdp_ws::read_result::status::message) {
          if (!handle_line(sess, std::move(rr.message)))
            break;
          continue;
        }
        if (rr.state == cdp_ws::read_result::status::error)
          cdp_ws::write_close(sess->sock, 1002, rr.error);
        break;
      }
    }

    bool handle_line(const std::shared_ptr<session>& sess, std::string line) {
      json parsed;
      try {
        parsed = json::parse(line);
      } catch (const nlohmann::json::parse_error& e) {
        json reply{json::object()};
        json err{json::object()};
        err["code"] = static_cast<double>(static_cast<int>(err_code::parse_error));
        err["message"] = std::string(e.what());
        reply["error"] = std::move(err);
        enqueue_outgoing(sess->id, reply.dump());
        return true;
      }
      if (!parsed.is_object()) {
        send_error(sess->id, json{nullptr}, err_code::invalid_request, "expected object");
        return true;
      }
      auto mp = parsed.find("method");
      json id_val{nullptr};
      if (auto ip = parsed.find("id"); ip != parsed.end())
        id_val = *ip;
      if (mp == parsed.end() || !mp->is_string()) {
        send_error(sess->id, id_val, err_code::invalid_request, "missing method");
        return true;
      }
      std::string method = mp->get<std::string>();
      json params{json::object()};
      if (auto pp = parsed.find("params"); pp != parsed.end())
        params = *pp;
      if (!method_exists(method)) {
        send_error(sess->id, id_val, err_code::method_not_found, method);
        return true;
      }

      auto call = std::make_shared<pending_call>();
      call->method = std::move(method);
      call->params = std::move(params);
      call->id = id_val;
      call->origin = sess->id;
      call->origin_session = sess;

      // Defer screenshot dispatch by `delayMs` to allow the script to render
      // additional frames before capture. Larger schedulers (e.g. CDP-style
      // animations) live client-side; this is the simple "wait then snap".
      if (call->method == "Page.screenshot") {
        double delay_ms = 0.0;
        if (auto it = call->params.find("delayMs"); it != call->params.end() && it->is_number())
          delay_ms = it->get<double>();
        if (delay_ms > 0.0) {
          if (delay_ms > 60000.0)
            delay_ms = 60000.0; // hard cap: 60s
          call->not_before = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(static_cast<long long>(delay_ms));
        }
      }

      {
        std::lock_guard<std::mutex> g(inbox_mu);
        inbox.push_back(call);
      }
      if (wake)
        wake();
      return true;
    }

    void send_error(session_id id, const json& request_id, err_code code, std::string_view msg) {
      json reply{json::object()};
      if (!request_id.is_null())
        reply["id"] = request_id;
      json err{json::object()};
      err["code"] = static_cast<double>(static_cast<int>(code));
      err["message"] = std::string(msg);
      reply["error"] = std::move(err);
      enqueue_outgoing(id, reply.dump());
    }

    void writer_loop(std::shared_ptr<session> sess) {
      while (running.load() && sess->alive.load()) {
        outgoing_message msg;
        {
          std::unique_lock<std::mutex> g(sess->outbox_mu);
          sess->outbox_cv.wait(
              g, [&] { return !running.load() || !sess->alive.load() || !sess->outbox.empty(); });
          if ((!running.load() || !sess->alive.load()) && sess->outbox.empty())
            return;
          if (sess->outbox.empty())
            continue;
          msg = std::move(sess->outbox.front());
          sess->outbox.pop_front();
        }
        if (sess->is_websocket.load()) {
          if (!cdp_ws::write_text(sess->sock, msg.line))
            return;
          continue;
        }
        std::string wire = std::move(msg.line);
        wire.push_back('\n');
        const char* p = wire.data();
        usize left = wire.size();
        while (left > 0) {
#if defined(_WIN32)
          int n = ::send(sess->sock, p, static_cast<int>(left), send_flags());
#else
          ssize_t n = ::send(sess->sock, p, left, send_flags());
#endif
          if (n <= 0)
            return;
          p += n;
          left -= static_cast<usize>(n);
        }
      }
    }
  };

  // ---------------- detail helpers ---------------------------------------
  namespace detail {
    void server_set_pause(server* s, bool paused, bool single_step) {
      if (s)
        s->_internal_set_pause(paused, single_step);
    }
    void server_set_console_enabled(server* s, session_id id, bool on) {
      if (s)
        s->_internal_set_session_console_enabled(id, on);
    }
    void server_set_channel_enabled(server* s, session_id id, event_channel channel, bool enabled) {
      if (s)
        s->_internal_set_session_channel_enabled(id, static_cast<int>(channel), enabled);
    }
  } // namespace detail

  // ---------------- public API -------------------------------------------
  server::server(server_options opts) : p_(std::make_unique<impl>(std::move(opts))) {}
  server::~server() = default;

  void server::attach_host(js::host* h) noexcept {
    p_->host = h;
  }
  void server::attach_window(window* w) noexcept {
    p_->win = w;
  }
  void server::attach_renderer(renderer* r) noexcept {
    p_->rdr = r;
  }
  void server::set_wake_callback(std::function<void()> wake) noexcept {
    p_->wake = std::move(wake);
  }

  // ---- Cross-binding event bridge ----------------------------------------
  // Bindings (window/fetch/fs) call emit_event_if_attached() without holding a
  // server pointer. We track the currently-running server via an atomic and
  // route through it. Multiple servers in the same process are not supported
  // (start() leaves the prior pointer in place if one is already attached).
  namespace {
    std::atomic<server*> g_active_server{nullptr};
    std::atomic<uint32_t> g_channel_mask{0}; // bit i = channel i enabled

    constexpr uint32_t channel_bit(event_channel c) {
      return event_channel_bit(c);
    }
  } // namespace

  bool channel_enabled(event_channel channel) {
    return (g_channel_mask.load(std::memory_order_acquire) & channel_bit(channel)) != 0;
  }

  void set_channel_enabled(event_channel channel, bool enabled) {
    if (enabled)
      g_channel_mask.fetch_or(channel_bit(channel), std::memory_order_release);
    else
      g_channel_mask.fetch_and(~channel_bit(channel), std::memory_order_release);
  }

  void emit_event_if_attached(event_channel channel, std::string_view method, json params) {
    if (!channel_enabled(channel))
      return;
    auto* srv = g_active_server.load(std::memory_order_acquire);
    if (!srv)
      return;
    srv->emit_event(method, std::move(params));
  }
  bool server::start() {
#if defined(_WIN32)
    static std::once_flag wsa_once;
    std::call_once(wsa_once, [] {
      WSADATA wsa{};
      WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif

    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
      p_->last_error = "socket() failed";
      return false;
    }
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(p_->opts.port);
    if (p_->opts.host.empty() || p_->opts.host == "127.0.0.1" || p_->opts.host == "localhost") {
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (p_->opts.host == "0.0.0.0") {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
      ::inet_pton(AF_INET, p_->opts.host.c_str(), &addr.sin_addr);
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      p_->last_error = "bind() failed: " + std::to_string(socket_errno());
      close_socket(s);
      return false;
    }
    if (::listen(s, static_cast<int>(p_->max_sessions())) != 0) {
      p_->last_error = "listen() failed";
      close_socket(s);
      return false;
    }
    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) {
      p_->last_error = "getsockname() failed";
      close_socket(s);
      return false;
    }
    p_->port.store(ntohs(actual.sin_port));
    p_->listen_sock = s;
    p_->running.store(true);
    g_active_server.store(this, std::memory_order_release);
    p_->accept_thread = std::thread(&impl::accept_loop, p_.get());

    std::printf("FXE_DEBUG_PORT=%u\n", unsigned(p_->port.load()));
    std::fflush(stdout);
    return true;
  }

  void server::stop() noexcept {
    // Clear active-server pointer if it points to us. Channels reset so the
    // next started server doesn't inherit stale subscriptions.
    server* expected = this;
    g_active_server.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    set_channel_enabled(event_channel::window, false);
    set_channel_enabled(event_channel::fetch, false);
    set_channel_enabled(event_channel::fs, false);
    set_channel_enabled(event_channel::perf, false);
    p_->shutdown();
  }

  bool server::running() const noexcept {
    return p_->running.load();
  }
  u16 server::bound_port() const noexcept {
    return p_->port.load();
  }
  std::string server::last_error() const {
    return p_->last_error;
  }

  void server::_internal_set_pause(bool paused, bool single_step) noexcept {
    p_->paused.store(paused);
    p_->step_once.store(single_step);
    json params{json::object()};
    emit_event(paused ? "Debugger.paused" : "Debugger.resumed", std::move(params));
    if (p_->wake)
      p_->wake();
  }

  void server::_internal_set_console_enabled(bool on) noexcept {
    p_->set_all_console_enabled(on);
  }

  void server::_internal_set_session_console_enabled(std::uint64_t id, bool on) noexcept {
    p_->set_session_console_enabled(static_cast<session_id>(id), on);
  }

  void server::_internal_set_session_channel_enabled(std::uint64_t id, int channel,
                                                     bool on) noexcept {
    if (channel < static_cast<int>(event_channel::window) ||
        channel > static_cast<int>(event_channel::perf))
      return;
    p_->set_session_channel_enabled(static_cast<session_id>(id),
                                    static_cast<event_channel>(channel), on);
  }

  void server::pump_tasks() {
    std::deque<std::shared_ptr<pending_call>> drained;
    {
      std::lock_guard<std::mutex> g(p_->inbox_mu);
      auto now = std::chrono::steady_clock::now();
      std::deque<std::shared_ptr<pending_call>> retain;
      for (auto& c : p_->inbox) {
        if (c->aborted.load())
          continue;
        auto origin = c->origin_session.lock();
        if (!origin || !origin->alive.load())
          continue;
        if (c->not_before && *c->not_before > now)
          retain.push_back(std::move(c));
        else
          drained.push_back(std::move(c));
      }
      p_->inbox = std::move(retain);
    }
    for (auto& call : drained) {
      auto origin = call->origin_session.lock();
      if (call->aborted.load() || !origin || !origin->alive.load())
        continue;
      // Prefer host-tracked active window/renderer (set by JS bindings) so
      // protocol methods automatically see whatever the script just created.
      // Fall back to anything explicitly attached on the server.
      window* win = p_->win;
      renderer* rdr = p_->rdr;
      if (p_->host) {
        if (auto* w = p_->host->active_window())
          win = w;
        if (auto* r = p_->host->active_renderer())
          rdr = r;
      }
      dispatch_context cx{this, p_->host, win, rdr, call->origin};
      json reply_envelope{json::object()};
      if (!call->id.is_null())
        reply_envelope["id"] = call->id;
      try {
        json result = dispatch(cx, call->method, call->params);
        reply_envelope["result"] = std::move(result);
      } catch (const dispatch_error& e) {
        json err{json::object()};
        err["code"] = static_cast<double>(static_cast<int>(e.code));
        err["message"] = e.message;
        if (!e.data.empty())
          err["data"] = e.data;
        reply_envelope["error"] = std::move(err);
      } catch (const std::exception& e) {
        json err{json::object()};
        err["code"] = static_cast<double>(static_cast<int>(err_code::internal));
        err["message"] = std::string("internal: ") + e.what();
        reply_envelope["error"] = std::move(err);
      } catch (...) {
        json err{json::object()};
        err["code"] = static_cast<double>(static_cast<int>(err_code::internal));
        err["message"] = std::string("unknown internal error");
        reply_envelope["error"] = std::move(err);
      }

      std::string serialized;
      if (!call->id.is_null())
        serialized = reply_envelope.dump();
      if (!serialized.empty() && !call->aborted.load())
        p_->enqueue_outgoing(call->origin, std::move(serialized));
    }
  }

  bool server::is_paused() const noexcept {
    return p_->paused.load();
  }

  void server::emit_console(std::string_view level, std::string_view text) {
    json params{json::object()};
    params["level"] = std::string(level);
    params["text"] = std::string(text);
    using clock = std::chrono::steady_clock;
    static std::once_flag start_once;
    static clock::time_point start;
    std::call_once(start_once, [] { start = clock::now(); });
    double ts = std::chrono::duration<double, std::milli>(clock::now() - start).count();
    params["ts"] = ts;
    emit_event("Console.messageAdded", std::move(params));
  }

  void server::emit_event(std::string_view method, json params) {
    json envelope{json::object()};
    envelope["method"] = std::string(method);
    envelope["params"] = std::move(params);
    p_->broadcast_event(method, envelope.dump());
  }
} // namespace fxe::debug
