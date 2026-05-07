// fxe debug server — NDJSON or CDP WebSocket over TCP. Single-connection v1.
//
// Layout:
//   * server::impl owns a listening socket and an accept thread.
//   * For each accepted protocol connection a "session thread" reads NDJSON
//     lines or WebSocket text frames, parses JSON-RPC/CDP payloads, validates
//     the method, and pushes a `pending_call` onto the MPSC queue consumed by
//     pump_tasks() on the render thread.
//   * Replies are produced on the render thread and posted back to the
//     session thread's outbox; the session thread serializes & flushes.
//   * Events (Console.messageAdded, Debugger.paused, ...) are pushed by the
//     render thread into the same outbox via emit_event/emit_console.
//
// Robustness notes:
//   * If the client disconnects while a task is queued, the task still runs
//     (cheap, render thread doesn't block on the wire) but its reply is
//     dropped before serialize.
//   * Bind errors leave the server in a not-running state; last_error()
//     reports the cause.
//
// Threading:
//   * Render thread calls pump_tasks(), is_paused(), emit_event/console,
//     attach_*, set_wake_callback, start, stop, dtor.
//   * Accept thread runs accept() loop. Once a client is in, it owns the
//     fd until the connection ends.
//   * Session thread reads/writes the connection.
// All shared state is guarded by `mu_` except the atomics noted below.

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

  // -------------------------------------------------------------------------
  // Pending request: parsed off the wire on the session thread, executed on
  // the render thread. The session thread waits on the future to serialize the
  // reply back to the client.
  // -------------------------------------------------------------------------
  struct pending_call {
    std::string method;
    json params;
    json id;                         // null when a notification (no reply expected)
    std::promise<std::string> reply; // serialized JSON envelope (or "" to drop)
    // If set, pump_tasks() must not run this call until steady_clock has
    // reached this point. Used to honour Page.screenshot delayMs without
    // blocking the render thread.
    std::optional<std::chrono::steady_clock::time_point> not_before{};
  };

  struct outgoing_message {
    std::string line; // serialized JSON envelope, no transport delimiter
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

    // Session state. mu_ protects.
    std::mutex mu;
    std::condition_variable session_cv;
    std::thread session_thread;
    socket_t conn_sock = kInvalidSocket;
    std::atomic<bool> session_alive{false};

    // Render-thread inbound queue.
    std::mutex inbox_mu;
    std::deque<std::shared_ptr<pending_call>> inbox;

    // Session outbox — single-writer (render thread / session thread for
    // immediate replies) → consumer (session thread).
    std::mutex outbox_mu;
    std::condition_variable outbox_cv;
    std::deque<outgoing_message> outbox;

    // Debugger flags.
    std::atomic<bool> paused{false};
    std::atomic<bool> step_once{false};
    std::atomic<bool> console_enabled{false};

    impl(server_options o) : opts(std::move(o)) {
      paused.store(opts.start_paused);
    }

    ~impl() {
      shutdown();
    }

    void shutdown() {
      bool was_running = running.exchange(false);
      if (!was_running)
        return;
      // Close listening socket so accept() returns.
      if (listen_sock != kInvalidSocket) {
        close_socket(listen_sock);
        listen_sock = kInvalidSocket;
      }
      // Close session socket so session thread exits.
      {
        std::lock_guard<std::mutex> g(mu);
        if (conn_sock != kInvalidSocket) {
          ::shutdown(conn_sock, 2);
          close_socket(conn_sock);
          conn_sock = kInvalidSocket;
        }
      }
      abort_pending_calls();
      outbox_cv.notify_all();
      if (accept_thread.joinable())
        accept_thread.join();
      if (session_thread.joinable())
        session_thread.join();
    }

    void abort_pending_calls() {
      std::deque<std::shared_ptr<pending_call>> pending;
      {
        std::lock_guard<std::mutex> g(inbox_mu);
        pending = std::move(inbox);
        inbox.clear();
      }
      for (auto& call : pending) {
        try {
          call->reply.set_value({});
        } catch (...) {
          // The render thread may already have completed the call.
        }
      }
    }

    void enqueue_outgoing(std::string line) {
      {
        std::lock_guard<std::mutex> g(outbox_mu);
        outbox.push_back({std::move(line)});
      }
      outbox_cv.notify_all();
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
          // EINTR / EAGAIN: retry. Otherwise log & break.
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

        // TODO(multiclient): replace this single-client gate with session routing.
        bool busy = false;
        socket_t session_sock = kInvalidSocket;
        {
          std::lock_guard<std::mutex> g(mu);
          if (session_alive.load() || conn_sock != kInvalidSocket) {
            busy = true;
          } else {
            conn_sock = s;
            // Snapshot while `mu` protects conn_sock; session_loop uses this local
            // descriptor and does not read conn_sock after the lock is released.
            session_sock = conn_sock;
          }
        }
        if (busy) {
          if (handle_stateless_http_probe(s, peer_addr)) {
            ::shutdown(s, 2);
            close_socket(s);
            continue;
          }
        }
        if (busy) {
          if (opts.log_level > 0)
            std::fprintf(stderr, "fxe.debug: rejecting client %s: server busy\n",
                         peer_addr.c_str());
          static const char kBusy[] = "{\"error\":{\"code\":-32003,\"message\":\"server busy\"}}\n";
          ::send(s, kBusy, sizeof(kBusy) - 1, send_flags());
          ::shutdown(s, 2);
          close_socket(s);
          continue;
        }
        // Reset paused flag honoring opts.start_paused on every fresh
        // connection (fresh debugging session).
        paused.store(opts.start_paused);
        step_once.store(false);
        console_enabled.store(false);
        {
          std::lock_guard<std::mutex> g(outbox_mu);
          outbox.clear();
        }

        if (session_thread.joinable())
          session_thread.join();
        session_alive.store(true);
        session_thread = std::thread(&impl::session_loop, this, session_sock);
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

    std::string request_host_authority(const cdp_ws::http_request& req) const {
      auto authority = req.header("host");
      if (!authority.empty())
        return authority;
      return ws_authority(opts.host, port.load());
    }

    bool send_http_response(socket_t s, const std::string& response) {
      return cdp_ws::send_all(s, response);
    }

    bool path_is_ws_endpoint(std::string_view path) const {
      return path == "/devtools/browser" || path == "/devtools/page/fxe-main";
    }

    bool handle_stateless_http_probe(socket_t s, const std::string& peer_addr) {
      if (peek_first_byte(s) != 'G')
        return false;
      cdp_ws::http_request req;
      std::string error;
      if (!cdp_ws::read_http_request(s, req, error))
        return false;
      auto authority = request_host_authority(req);
      if (auto response = make_cdp_discovery_http_response(req.path, authority, port.load())) {
        send_http_response(s, *response);
        return true;
      }
      const bool busy_ws_endpoint = path_is_ws_endpoint(req.path);
      if (busy_ws_endpoint && opts.log_level > 0)
        std::fprintf(stderr, "fxe.debug: rejecting client %s: server busy\n", peer_addr.c_str());
      auto body = busy_ws_endpoint
                      ? "fxe debug server is busy; one WebSocket client is already attached."
                      : cdp_not_found_body(req.path);
      send_http_response(
          s, cdp_ws::http_response(busy_ws_endpoint ? 503 : 404,
                                   busy_ws_endpoint ? "Service Unavailable" : "Not Found",
                                   "text/plain; charset=utf-8", body));
      return true;
    }

    bool handle_http_connection(socket_t s) {
      cdp_ws::http_request req;
      std::string error;
      if (!cdp_ws::read_http_request(s, req, error)) {
        send_http_response(s, cdp_ws::http_response(400, "Bad Request", "text/plain; charset=utf-8",
                                                    error.empty() ? "bad HTTP request" : error));
        return false;
      }
      auto authority = request_host_authority(req);
      if (auto response = make_cdp_discovery_http_response(req.path, authority, port.load())) {
        send_http_response(s, *response);
        return false;
      }
      if (path_is_ws_endpoint(req.path) && !req.header("upgrade").empty()) {
        if (!cdp_ws::handshake(s, req, error)) {
          send_http_response(
              s, cdp_ws::http_response(400, "Bad Request", "text/plain; charset=utf-8",
                                       error.empty() ? "bad WebSocket handshake" : error));
          return false;
        }
        return true;
      }
      send_http_response(s, cdp_ws::http_response(404, "Not Found", "text/plain; charset=utf-8",
                                                  cdp_not_found_body(req.path)));
      return false;
    }

    void cleanup_session(socket_t s, std::thread* writer) {
      {
        std::lock_guard<std::mutex> g(mu);
        if (conn_sock == s) {
          ::shutdown(s, 2);
          close_socket(s);
          conn_sock = kInvalidSocket;
        }
      }
      session_alive.store(false);
      outbox_cv.notify_all();
      if (writer && writer->joinable())
        writer->join();
    }

    void session_loop(socket_t s) {
      bool websocket = false;
      int first = peek_first_byte(s);
      if (first == 'G') {
        websocket = handle_http_connection(s);
        if (!websocket) {
          cleanup_session(s, nullptr);
          return;
        }
      }

      std::thread writer(&impl::writer_loop, this, s, websocket);
      if (websocket)
        websocket_read_loop(s);
      else
        ndjson_read_loop(s);
      cleanup_session(s, &writer);

      // Drain any inflight tasks: their reply futures will go nowhere, but
      // the render thread still runs them. Drop unfulfilled pending replies.
    }

    void ndjson_read_loop(socket_t s) {
      std::string buf;
      buf.reserve(4096);
      char chunk[4096];
      bool close_requested = false;
      while (running.load()) {
#if defined(_WIN32)
        int n = ::recv(s, chunk, sizeof(chunk), 0);
#else
        ssize_t n = ::recv(s, chunk, sizeof(chunk), 0);
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
          close_requested = !handle_line(std::move(line), s);
        }
        if (close_requested)
          break;
      }
    }

    void websocket_read_loop(socket_t s) {
      cdp_ws::reader r(true);
      while (running.load()) {
        auto rr = r.read_text(s);
        if (rr.state == cdp_ws::read_result::status::message) {
          if (!handle_line(std::move(rr.message), s))
            break;
          continue;
        }
        if (rr.state == cdp_ws::read_result::status::error)
          cdp_ws::write_close(s, 1002, rr.error);
        break;
      }
    }

    bool handle_line(std::string line, socket_t s) {
      (void)s;
      json parsed;
      try {
        parsed = json::parse(line);
      } catch (const nlohmann::json::parse_error& e) {
        json reply{json::object()};
        json err{json::object()};
        err["code"] = static_cast<double>(static_cast<int>(err_code::parse_error));
        err["message"] = std::string(e.what());
        reply["error"] = std::move(err);
        enqueue_outgoing(reply.dump());
        return true;
      }
      if (!parsed.is_object()) {
        send_error(json{nullptr}, err_code::invalid_request, "expected object");
        return true;
      }
      auto mp = parsed.find("method");
      json id_val{nullptr};
      if (auto ip = parsed.find("id"); ip != parsed.end())
        id_val = *ip;
      if (mp == parsed.end() || !mp->is_string()) {
        send_error(id_val, err_code::invalid_request, "missing method");
        return true;
      }
      std::string method = mp->get<std::string>();
      json params{json::object()};
      if (auto pp = parsed.find("params"); pp != parsed.end())
        params = *pp;
      if (!method_exists(method)) {
        send_error(id_val, err_code::method_not_found, method);
        return true;
      }

      auto call = std::make_shared<pending_call>();
      call->method = std::move(method);
      call->params = std::move(params);
      call->id = id_val;

      // Defer screenshot dispatch by `delayMs` to allow the script to render
      // additional frames before capture. Larger schedulers (e.g. CDP-style
      // animations) live client-side; this is the simple "wait then snap".
      if (call->method == "Page.screenshot") {
        double delay_ms = 0.0;
        if (auto it = params.find("delayMs"); it != params.end() && it->is_number())
          delay_ms = it->get<double>();
        if (delay_ms > 0.0) {
          if (delay_ms > 60000.0)
            delay_ms = 60000.0; // hard cap: 60s
          call->not_before = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(static_cast<long long>(delay_ms));
        }
      }

      auto fut = call->reply.get_future();
      {
        std::lock_guard<std::mutex> g(inbox_mu);
        inbox.push_back(call);
      }
      if (wake)
        wake();

      // Block until render thread completes the call. This is fine on the
      // session thread; the render thread won't deadlock waiting on us.
      std::string reply_line = fut.get();
      if (!reply_line.empty())
        enqueue_outgoing(std::move(reply_line));
      return true;
    }

    void send_error(const json& id, err_code code, std::string_view msg) {
      json reply{json::object()};
      if (!id.is_null())
        reply["id"] = id;
      json err{json::object()};
      err["code"] = static_cast<double>(static_cast<int>(code));
      err["message"] = std::string(msg);
      reply["error"] = std::move(err);
      enqueue_outgoing(reply.dump());
    }

    void writer_loop(socket_t s, bool websocket) {
      while (running.load()) {
        outgoing_message msg;
        {
          std::unique_lock<std::mutex> g(outbox_mu);
          outbox_cv.wait(
              g, [&] { return !running.load() || !session_alive.load() || !outbox.empty(); });
          if ((!running.load() || !session_alive.load()) && outbox.empty())
            return;
          if (outbox.empty())
            continue;
          msg = std::move(outbox.front());
          outbox.pop_front();
        }
        if (websocket) {
          if (!cdp_ws::write_text(s, msg.line))
            return;
          continue;
        }
        std::string wire = std::move(msg.line);
        wire.push_back('\n');
        const char* p = wire.data();
        usize left = wire.size();
        while (left > 0) {
#if defined(_WIN32)
          int n = ::send(s, p, static_cast<int>(left), send_flags());
#else
          ssize_t n = ::send(s, p, left, send_flags());
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
    void server_set_console_enabled(server* s, bool on) {
      if (s)
        s->_internal_set_console_enabled(on);
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
      return uint32_t(1) << static_cast<int>(c);
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
    if (::listen(s, 1) != 0) {
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
    p_->console_enabled.store(on);
  }

  void server::pump_tasks() {
    std::deque<std::shared_ptr<pending_call>> drained;
    {
      std::lock_guard<std::mutex> g(p_->inbox_mu);
      auto now = std::chrono::steady_clock::now();
      std::deque<std::shared_ptr<pending_call>> retain;
      for (auto& c : p_->inbox) {
        if (c->not_before && *c->not_before > now)
          retain.push_back(std::move(c));
        else
          drained.push_back(std::move(c));
      }
      p_->inbox = std::move(retain);
    }
    for (auto& call : drained) {
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
      dispatch_context cx{this, p_->host, win, rdr};
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
      // Hand back to the session thread (notifications: empty string).
      try {
        call->reply.set_value(std::move(serialized));
      } catch (...) {
        // promise already satisfied (shouldn't happen); ignore
      }
    }
  }

  bool server::is_paused() const noexcept {
    return p_->paused.load();
  }

  void server::emit_console(std::string_view level, std::string_view text) {
    if (!p_->console_enabled.load())
      return;
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
    if (!p_->session_alive.load())
      return;
    json envelope{json::object()};
    envelope["method"] = std::string(method);
    envelope["params"] = std::move(params);
    p_->enqueue_outgoing(envelope.dump());
  }
} // namespace fxe::debug
