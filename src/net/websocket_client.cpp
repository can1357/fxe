#include "websocket_client.hpp"
#include "runtime/uv_loop.hpp"
#include <fxe/types.hpp>
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
#include "tls_client.hpp"
#endif
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
#include <zlib.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_handle = SOCKET;
static constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
using socket_addr_len = int;
using socket_opt_len = int;
static int sock_close(socket_handle s) {
  return ::closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
static constexpr socket_handle kInvalidSocket = -1;
using socket_addr_len = socklen_t;
using socket_opt_len = socklen_t;
static int sock_close(socket_handle s) {
  return ::close(s);
}
#endif

namespace fxe::net {

  // ws:// transport still uses its dedicated socket worker so builds without
  // libuv keep the existing truthful fallback. When the shared runtime loop is
  // available this client registers a bounded pump hook; the hook never blocks
  // and keeps WebSocket transport wakeups tied to the same loop that later
  // dispatches events on the V8 thread.

  namespace {

#ifdef _WIN32
    struct wsa_init {
      wsa_init() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
      }
    };
    static wsa_init g_wsa_init;
#endif

    // Tiny base64 encoder for the Sec-WebSocket-Key nonce.
    std::string b64_encode(const u8* data, usize n) {
      static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve(((n + 2) / 3) * 4);
      for (usize i = 0; i < n; i += 3) {
        u32 a = data[i];
        u32 b = i + 1 < n ? data[i + 1] : 0;
        u32 c = i + 2 < n ? data[i + 2] : 0;
        u32 v = (a << 16) | (b << 8) | c;
        out.push_back(tab[(v >> 18) & 0x3F]);
        out.push_back(tab[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? tab[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < n ? tab[v & 0x3F] : '=');
      }
      return out;
    }

    bool parse_ws_url(const std::string& url, std::string& host, std::string& port,
                      std::string& path, bool& secure) {
      const std::string ws = "ws://";
      const std::string wss = "wss://";
      usize off = 0;
      if (url.rfind(ws, 0) == 0) {
        secure = false;
        off = ws.size();
      } else if (url.rfind(wss, 0) == 0) {
        secure = true;
        off = wss.size();
      } else {
        return false;
      }
      auto slash = url.find('/', off);
      std::string authority =
          slash == std::string::npos ? url.substr(off) : url.substr(off, slash - off);
      path = slash == std::string::npos ? "/" : url.substr(slash);
      auto colon = authority.find(':');
      if (colon == std::string::npos) {
        host = authority;
        port = secure ? "443" : "80";
      } else {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
      }
      return !host.empty();
    }

    bool send_all(socket_handle s, const u8* data, usize n) {
      usize off = 0;
      while (off < n) {
#ifdef _WIN32
        const usize chunk = std::min(n - off, static_cast<usize>(std::numeric_limits<int>::max()));
        int r = ::send(s, reinterpret_cast<const char*>(data + off), static_cast<int>(chunk), 0);
#else
        ssize_t r = ::send(s, data + off, n - off, 0);
#endif
        if (r <= 0)
          return false;
        off += static_cast<usize>(r);
      }
      return true;
    }

    bool recv_n(socket_handle s, u8* buf, usize n) {
      usize off = 0;
      while (off < n) {
#ifdef _WIN32
        const usize chunk = std::min(n - off, static_cast<usize>(std::numeric_limits<int>::max()));
        int r = ::recv(s, reinterpret_cast<char*>(buf + off), static_cast<int>(chunk), 0);
#else
        ssize_t r = ::recv(s, buf + off, n - off, 0);
#endif
        if (r <= 0)
          return false;
        off += static_cast<usize>(r);
      }
      return true;
    }

    std::string ascii_lower(std::string s) {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    std::string trim(std::string_view v) {
      usize begin = 0;
      while (begin < v.size() && (v[begin] == ' ' || v[begin] == '\t'))
        ++begin;
      usize end = v.size();
      while (end > begin && (v[end - 1] == ' ' || v[end - 1] == '\t'))
        --end;
      return std::string(v.substr(begin, end - begin));
    }

    std::vector<std::string> split_http_list(std::string_view v, char sep) {
      std::vector<std::string> out;
      usize start = 0;
      bool quoted = false;
      for (usize i = 0; i < v.size(); ++i) {
        if (v[i] == '"') {
          quoted = !quoted;
        } else if (!quoted && v[i] == sep) {
          out.push_back(trim(v.substr(start, i - start)));
          start = i + 1;
        }
      }
      out.push_back(trim(v.substr(start)));
      return out;
    }

    std::vector<std::string> header_values(const std::string& headers, std::string name_lower) {
      std::vector<std::string> out;
      usize line = 0;
      while (line < headers.size()) {
        usize eol = headers.find("\r\n", line);
        if (eol == std::string::npos)
          break;
        usize colon = headers.find(':', line);
        if (colon != std::string::npos && colon < eol) {
          std::string name = ascii_lower(headers.substr(line, colon - line));
          if (name == name_lower)
            out.push_back(trim(std::string_view(headers).substr(colon + 1, eol - colon - 1)));
        }
        line = eol + 2;
      }
      return out;
    }

    bool parse_window_bits(std::string_view value, int& bits) {
      std::string v = trim(value);
      if (v.empty()) {
        bits = 15;
        return true;
      }
      char* end = nullptr;
      long parsed = std::strtol(v.c_str(), &end, 10);
      if (!end || *end != '\0' || parsed < 8 || parsed > 15)
        return false;
      bits = static_cast<int>(parsed);
      return true;
    }
#endif

  } // namespace

  struct websocket_client::ws_deflate_state {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    z_stream inflater{};
    z_stream deflater{};
    bool inflater_ready = false;
    bool deflater_ready = false;
    bool negotiated = false;
    bool client_no_context_takeover = false;
    bool server_no_context_takeover = false;
    int client_max_window_bits = 15;
    int server_max_window_bits = 15;

    ~ws_deflate_state() {
      if (inflater_ready)
        inflateEnd(&inflater);
      if (deflater_ready)
        deflateEnd(&deflater);
    }
#else
    bool negotiated = false;
#endif
  };

  i64 monotonic_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
        .count();
  }

  bool parse_u16_port(const std::string& port, u16& out) {
    if (port.empty())
      return false;
    unsigned long value = 0;
    for (char c : port) {
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return false;
      value = value * 10 + static_cast<unsigned long>(c - '0');
      if (value > 65535)
        return false;
    }
    if (value == 0)
      return false;
    out = static_cast<u16>(value);
    return true;
  }

  bool websocket_client::negotiate_permessage_deflate(const std::string& headers) {
    negotiated_extensions_.clear();
    deflate_.reset();
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    auto values = header_values(headers, "sec-websocket-extensions");
    for (const auto& value : values) {
      for (const auto& extension : split_http_list(value, ',')) {
        auto parts = split_http_list(extension, ';');
        if (parts.empty() || ascii_lower(parts[0]) != "permessage-deflate")
          continue;

        auto state = std::make_unique<ws_deflate_state>();
        state->negotiated = true;
        for (usize i = 1; i < parts.size(); ++i) {
          if (parts[i].empty())
            continue;
          auto eq = parts[i].find('=');
          std::string name = ascii_lower(trim(std::string_view(parts[i]).substr(0, eq)));
          std::string value_part;
          if (eq != std::string::npos)
            value_part = trim(std::string_view(parts[i]).substr(eq + 1));
          if (value_part.size() >= 2 && value_part.front() == '"' && value_part.back() == '"')
            value_part = value_part.substr(1, value_part.size() - 2);

          if (name == "client_no_context_takeover") {
            if (!value_part.empty()) {
              handshake_error_ = "invalid permessage-deflate client_no_context_takeover";
              return false;
            }
            state->client_no_context_takeover = true;
          } else if (name == "server_no_context_takeover") {
            if (!value_part.empty()) {
              handshake_error_ = "invalid permessage-deflate server_no_context_takeover";
              return false;
            }
            state->server_no_context_takeover = true;
          } else if (name == "client_max_window_bits") {
            if (!parse_window_bits(value_part, state->client_max_window_bits)) {
              handshake_error_ = "invalid permessage-deflate client_max_window_bits";
              return false;
            }
          } else if (name == "server_max_window_bits") {
            if (!parse_window_bits(value_part, state->server_max_window_bits)) {
              handshake_error_ = "invalid permessage-deflate server_max_window_bits";
              return false;
            }
          } else {
            handshake_error_ = "unsupported permessage-deflate parameter: " + name;
            return false;
          }
        }

        int ret = inflateInit2(&state->inflater, -state->server_max_window_bits);
        if (ret != Z_OK) {
          handshake_error_ = "permessage-deflate inflate init failed";
          return false;
        }
        state->inflater_ready = true;
        ret = deflateInit2(&state->deflater, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           -state->client_max_window_bits, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
          handshake_error_ = "permessage-deflate deflate init failed";
          return false;
        }
        state->deflater_ready = true;
        negotiated_extensions_ = extension;
        deflate_ = std::move(state);
        return true;
      }
    }
#else
    (void)headers;
#endif
    return true;
  }

  bool websocket_client::deflate_message(const u8* data, usize n, std::vector<u8>& out) {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    if (!deflate_ || !deflate_->negotiated)
      return false;
    out.clear();
    z_stream& stream = deflate_->deflater;
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(n);
    u8 buf[8192];
    do {
      stream.next_out = reinterpret_cast<Bytef*>(buf);
      stream.avail_out = sizeof(buf);
      int ret = ::deflate(&stream, Z_SYNC_FLUSH);
      if (ret != Z_OK)
        return false;
      const usize produced = sizeof(buf) - stream.avail_out;
      out.insert(out.end(), buf, buf + produced);
    } while (stream.avail_in != 0 || stream.avail_out == 0);
    if (out.size() < 4 || out[out.size() - 4] != 0x00 || out[out.size() - 3] != 0x00 ||
        out[out.size() - 2] != 0xff || out[out.size() - 1] != 0xff)
      return false;
    out.resize(out.size() - 4);
    if (deflate_->client_no_context_takeover && deflateReset(&stream) != Z_OK)
      return false;
    return true;
#else
    (void)data;
    (void)n;
    (void)out;
    return false;
#endif
  }

  bool websocket_client::inflate_message(const std::vector<u8>& data, std::vector<u8>& out,
                                         bool& too_big) {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    too_big = false;
    if (!deflate_ || !deflate_->negotiated)
      return false;
    std::vector<u8> input = data;
    input.insert(input.end(), {0x00, 0x00, 0xff, 0xff});
    z_stream& stream = deflate_->inflater;
    stream.next_in = reinterpret_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    out.clear();
    u8 buf[8192];
    do {
      stream.next_out = reinterpret_cast<Bytef*>(buf);
      stream.avail_out = sizeof(buf);
      int ret = ::inflate(&stream, Z_SYNC_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END)
        return false;
      const usize produced = sizeof(buf) - stream.avail_out;
      if (produced > options_.max_message_bytes ||
          out.size() > options_.max_message_bytes - produced) {
        too_big = true;
        return false;
      }
      out.insert(out.end(), buf, buf + produced);
    } while (stream.avail_in != 0 || stream.avail_out == 0);
    if (deflate_->server_no_context_takeover && inflateReset(&stream) != Z_OK)
      return false;
    return true;
#else
    (void)data;
    (void)out;
    too_big = false;
    return false;
#endif
  }

  websocket_client::websocket_client(ws_client_options options) : options_(options) {}

  websocket_client::~websocket_client() {
    stop_.store(true);
    fxe::runtime::uv_loop_runtime::instance().unregister_pump_callback(uv_pump_callback_id_);
    {
      std::lock_guard<std::mutex> lk(out_mu_);
      out_cv_.notify_all();
    }
    close_socket_now(true);
    if (worker_.joinable())
      worker_.join();
  }

  bool websocket_client::connect(std::string url, std::vector<std::string> protocols) {
    if (url.empty())
      return false;
    url_ = std::move(url);
    protocols_ = std::move(protocols);
    uv_pump_callback_id_ = fxe::runtime::uv_loop_runtime::instance().register_pump_callback(
        [this] { runtime_pump(); });
    worker_ = std::thread([this] { worker_main(); });
    return true;
  }

  void websocket_client::close_socket_now(bool shutdown_first) {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    if (tls_) {
      tls_->close();
      return;
    }
#endif
    int old = sock_.exchange(static_cast<int>(kInvalidSocket));
    if (old == static_cast<int>(kInvalidSocket))
      return;
    if (shutdown_first) {
#ifdef _WIN32
      ::shutdown(static_cast<socket_handle>(old), SD_BOTH);
#else
      ::shutdown(static_cast<socket_handle>(old), SHUT_RDWR);
#endif
    }
    sock_close(static_cast<socket_handle>(old));
  }

  void websocket_client::push_event(ws_event ev) {
    std::lock_guard<std::mutex> lk(in_mu_);
    in_q_.push_back(std::move(ev));
  }

  std::vector<ws_event> websocket_client::drain_events() {
    std::vector<ws_event> out;
    std::lock_guard<std::mutex> lk(in_mu_);
    out.swap(in_q_);
    return out;
  }

  u16 websocket_client::close_code() const {
    std::lock_guard<std::mutex> lk(close_mu_);
    return close_code_;
  }

  std::string websocket_client::close_reason() const {
    std::lock_guard<std::mutex> lk(close_mu_);
    return close_reason_;
  }

  void websocket_client::runtime_pump() noexcept {
    const auto state = state_.load();
    if (state == ws_ready_state::open || state == ws_ready_state::closing) {
      out_cv_.notify_one();
    }
  }

  void websocket_client::send_text(std::string s, usize max_fragment_bytes) {
    if (state_.load() != ws_ready_state::open)
      return;
    out_msg m;
    m.op = out_op::text;
    m.max_fragment_bytes = max_fragment_bytes;
    m.bytes.assign(s.begin(), s.end());
    buffered_.fetch_add(m.bytes.size());
    {
      std::lock_guard<std::mutex> lk(out_mu_);
      out_q_.push(std::move(m));
    }
    out_cv_.notify_one();
  }

  void websocket_client::send_binary(std::vector<u8> data, usize max_fragment_bytes) {
    if (state_.load() != ws_ready_state::open)
      return;
    out_msg m;
    m.op = out_op::binary;
    m.max_fragment_bytes = max_fragment_bytes;
    buffered_.fetch_add(data.size());
    m.bytes = std::move(data);
    {
      std::lock_guard<std::mutex> lk(out_mu_);
      out_q_.push(std::move(m));
    }
    out_cv_.notify_one();
  }

  void websocket_client::close(u16 code, std::string reason) {
    auto cur = state_.load();
    if (cur == ws_ready_state::closed || cur == ws_ready_state::closing)
      return;
    state_.store(ws_ready_state::closing);
    out_msg m;
    m.op = out_op::close;
    m.code = code;
    m.bytes.assign(reason.begin(), reason.end());
    {
      std::lock_guard<std::mutex> lk(out_mu_);
      out_q_.push(std::move(m));
    }
    out_cv_.notify_one();
    if (cur == ws_ready_state::connecting) {
      stop_.store(true);
      close_socket_now(true);
    }
  }

  bool websocket_client::do_handshake() {
    std::string host, port, path;
    bool secure = false;
    if (!parse_ws_url(url_, host, port, path, secure))
      return false;
    if (stop_.load())
      return false;

    if (secure) {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
      u16 tls_port = 0;
      if (!parse_u16_port(port, tls_port)) {
        handshake_error_ = "invalid wss port";
        return false;
      }
      tls_options opts;
      opts.host = host;
      opts.port = tls_port;
      std::string err;
      tls_ = tls_client::connect(opts, err);
      if (!tls_) {
        handshake_error_ = err.empty() ? "TLS connection failed" : err;
        return false;
      }
#else
      handshake_error_ =
          "wss requires FXE_ENABLE_NATIVE_TLS_HTTP2; rebuild with -DFXE_ENABLE_NATIVE_TLS_HTTP2=ON";
      return false;
#endif
    } else {
      addrinfo hints{};
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* res = nullptr;
      if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        return false;
      socket_handle s = kInvalidSocket;
      for (auto* it = res; it; it = it->ai_next) {
        s = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == kInvalidSocket)
          continue;
        if (::connect(s, it->ai_addr, static_cast<socket_addr_len>(it->ai_addrlen)) == 0)
          break;
        sock_close(s);
        s = kInvalidSocket;
      }
      ::freeaddrinfo(res);
      if (s == kInvalidSocket)
        return false;
      int one = 1;
#ifdef _WIN32
      ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                   static_cast<socket_opt_len>(sizeof(one)));
#else
      ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, static_cast<socket_opt_len>(sizeof(one)));
#endif
      sock_.store(static_cast<int>(s));
    }
    if (stop_.load()) {
      close_socket_now(true);
      return false;
    }

    // Random 16-byte nonce -> base64
    u8 nonce[16];
    std::random_device rd;
    for (auto& b : nonce)
      b = static_cast<u8>(rd());
    std::string key = b64_encode(nonce, sizeof(nonce));

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host;
    if (!((secure && port == "443") || (!secure && port == "80")))
      req << ":" << port;
    req << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n";
    if (!protocols_.empty()) {
      req << "Sec-WebSocket-Protocol: ";
      for (usize i = 0; i < protocols_.size(); ++i) {
        if (i)
          req << ", ";
        req << protocols_[i];
      }
      req << "\r\n";
    }
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    if (options_.compress) {
      req << "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits; "
             "server_max_window_bits=15\r\n";
    }
#endif
    req << "User-Agent: fxe/0\r\n\r\n";
    std::string r = req.str();
    if (!transport_send_all(reinterpret_cast<const u8*>(r.data()), r.size()))
      return false;
    if (stop_.load())
      return false;

    std::string headers;
    if (!transport_recv_http_headers(headers))
      return false;
    // Parse status line
    if (headers.rfind("HTTP/1.1 101", 0) != 0 && headers.rfind("HTTP/1.0 101", 0) != 0)
      return false;
    // Note: Sec-WebSocket-Accept verification is intentionally skipped in v0.
    // Look for Sec-WebSocket-Protocol header to record selected_protocol_.
    std::string lower = ascii_lower(headers);
    auto p = lower.find("sec-websocket-protocol:");
    if (p != std::string::npos) {
      auto eol = headers.find("\r\n", p);
      auto colon = headers.find(':', p);
      if (eol != std::string::npos && colon != std::string::npos && colon < eol) {
        std::string v = headers.substr(colon + 1, eol - colon - 1);
        usize i = 0;
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t'))
          ++i;
        selected_protocol_ = v.substr(i);
      }
    }
    if (options_.compress && !negotiate_permessage_deflate(headers))
      return false;
    return true;
  }

  bool websocket_client::transport_send_all(const u8* data, usize n) {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    if (tls_) {
      usize off = 0;
      while (off < n) {
        ssize_t written = tls_->write(data + off, n - off);
        if (written <= 0)
          return false;
        off += static_cast<usize>(written);
      }
      return true;
    }
#endif
    int sock = sock_.load();
    return sock != static_cast<int>(kInvalidSocket) &&
           send_all(static_cast<socket_handle>(sock), data, n);
  }

  bool websocket_client::transport_recv_n(u8* buf, usize n) {
#if defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS) && FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    if (tls_) {
      usize off = 0;
      while (off < n) {
        ssize_t got = tls_->read(buf + off, n - off);
        if (got <= 0)
          return false;
        off += static_cast<usize>(got);
      }
      return true;
    }
#endif
    int sock = sock_.load();
    return sock != static_cast<int>(kInvalidSocket) &&
           recv_n(static_cast<socket_handle>(sock), buf, n);
  }

  bool websocket_client::transport_recv_http_headers(std::string& out) {
    out.clear();
    const usize cap = 16 * 1024;
    while (out.size() < cap) {
      u8 c = 0;
      if (!transport_recv_n(&c, 1))
        return false;
      out.push_back(static_cast<char>(c));
      if (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0)
        return true;
    }
    return false;
  }

  bool websocket_client::send_message(u8 opcode, const u8* data, usize n, usize max_fragment_bytes,
                                      bool compressed) {
    usize limit = max_fragment_bytes == 0 ? options_.max_fragment_bytes : max_fragment_bytes;
    if (limit == 0 || n <= limit)
      return send_frame(opcode, data, n, true, compressed);
    usize off = 0;
    bool first = true;
    while (off < n) {
      const usize chunk = std::min(limit, n - off);
      const bool fin = off + chunk == n;
      const u8 frame_opcode = first ? opcode : 0x0;
      if (!send_frame(frame_opcode, data + off, chunk, fin, first && compressed))
        return false;
      first = false;
      off += chunk;
    }
    return true;
  }

  bool websocket_client::send_frame(u8 opcode, const u8* data, usize n, bool fin, bool rsv1) {
    std::lock_guard<std::mutex> lk(send_mu_);
    std::vector<u8> hdr;
    hdr.reserve(14);
    hdr.push_back(static_cast<u8>((fin ? 0x80 : 0x00) | (rsv1 ? 0x40 : 0x00) | (opcode & 0x0F)));
    u8 mask_bit = 0x80;
    if (n < 126) {
      hdr.push_back(static_cast<u8>(mask_bit | n));
    } else if (n <= 0xFFFF) {
      hdr.push_back(static_cast<u8>(mask_bit | 126));
      hdr.push_back(static_cast<u8>((n >> 8) & 0xFF));
      hdr.push_back(static_cast<u8>(n & 0xFF));
    } else {
      hdr.push_back(static_cast<u8>(mask_bit | 127));
      for (int i = 7; i >= 0; --i)
        hdr.push_back(static_cast<u8>((n >> (i * 8)) & 0xFF));
    }
    u8 mask[4];
    std::random_device rd;
    for (auto& b : mask)
      b = static_cast<u8>(rd());
    hdr.insert(hdr.end(), mask, mask + 4);
    if (!transport_send_all(hdr.data(), hdr.size()))
      return false;
    u8 buf[4096];
    usize off = 0;
    while (off < n) {
      usize chunk = n - off;
      if (chunk > sizeof(buf))
        chunk = sizeof(buf);
      for (usize i = 0; i < chunk; ++i)
        buf[i] = static_cast<u8>(data[off + i] ^ mask[(off + i) & 3]);
      if (!transport_send_all(buf, chunk))
        return false;
      off += chunk;
    }
    return true;
  }

  bool websocket_client::send_close_frame(u16 code, const std::string& reason) {
    std::vector<u8> body;
    body.push_back(static_cast<u8>((code >> 8) & 0xFF));
    body.push_back(static_cast<u8>(code & 0xFF));
    body.insert(body.end(), reason.begin(), reason.end());
    return send_frame(0x8, body.data(), body.size());
  }

  void websocket_client::record_close(u16 code, std::string reason) {
    std::lock_guard<std::mutex> lk(close_mu_);
    close_code_ = code;
    close_reason_ = std::move(reason);
  }

  void websocket_client::emit_local_close(u16 code, std::string reason) {
    ws_event ev;
    ev.kind = ws_event_kind::close;
    ev.code = code;
    ev.reason = std::move(reason);
    ev.was_clean = false;
    push_event(std::move(ev));
    state_.store(ws_ready_state::closed);
    stop_.store(true);
    close_socket_now(true);
    out_cv_.notify_all();
  }

  void websocket_client::worker_main() {
    if (!do_handshake()) {
      ws_event err;
      err.kind = ws_event_kind::error_;
      err.text = handshake_error_.empty() ? "websocket handshake failed" : handshake_error_;
      err.reason = err.text;
      push_event(std::move(err));
      ws_event ev;
      ev.kind = ws_event_kind::close;
      ev.code = 1006;
      ev.was_clean = false;
      push_event(std::move(ev));
      state_.store(ws_ready_state::closed);
      return;
    }
    state_.store(ws_ready_state::open);
    last_frame_ms_.store(monotonic_ms());
    {
      ws_event ev;
      ev.kind = ws_event_kind::open;
      push_event(std::move(ev));
    }

    // Reader thread for inbound; this thread becomes the writer.
    std::thread reader([this] {
      std::vector<u8> assembled;
      u8 cont_opcode = 0;
      bool compressed_message = false;
      while (!stop_.load()) {
        u8 hdr[2];
        if (!transport_recv_n(hdr, 2))
          break;
        last_frame_ms_.store(monotonic_ms());
        bool fin = (hdr[0] & 0x80) != 0;
        u8 op = static_cast<u8>(hdr[0] & 0x0F);
        bool masked = (hdr[1] & 0x80) != 0;
        bool rsv1 = (hdr[0] & 0x40) != 0;
        bool rsv_other = (hdr[0] & 0x30) != 0;
        if (rsv_other || (rsv1 && !(op == 0x1 || op == 0x2)) ||
            (rsv1 && (!deflate_ || !deflate_->negotiated))) {
          send_close_frame(1002, "Protocol Error");
          emit_local_close(1002, "Protocol Error");
          return;
        }
        u64 plen = static_cast<u64>(hdr[1] & 0x7F);
        if (plen == 126) {
          u8 ext[2];
          if (!transport_recv_n(ext, 2))
            break;
          plen = (static_cast<u64>(ext[0]) << 8) | static_cast<u64>(ext[1]);
        } else if (plen == 127) {
          u8 ext[8];
          if (!transport_recv_n(ext, 8))
            break;
          plen = 0;
          for (int i = 0; i < 8; ++i)
            plen = (plen << 8) | static_cast<u64>(ext[i]);
        }
        u8 mask[4] = {0, 0, 0, 0};
        if (masked && !transport_recv_n(mask, 4))
          break;
        const bool is_data = op == 0x1 || op == 0x2 || op == 0x0;
        if (!(op == 0x0 || op == 0x1 || op == 0x2 || op == 0x8 || op == 0x9 || op == 0xA)) {
          send_close_frame(1002, "Protocol Error");
          emit_local_close(1002, "Protocol Error");
          return;
        }
        if (is_data) {
          const u64 base = op == 0x0 ? assembled.size() : 0;
          if (plen > static_cast<u64>(options_.max_message_bytes) ||
              base > static_cast<u64>(options_.max_message_bytes) - plen) {
            send_close_frame(1009, "Message Too Big");
            emit_local_close(1009, "Message Too Big");
            return;
          }
        }
        if (plen > static_cast<u64>(std::numeric_limits<usize>::max())) {
          send_close_frame(1009, "Message Too Big");
          emit_local_close(1009, "Message Too Big");
          return;
        }
        std::vector<u8> payload;
        if (plen > 0) {
          payload.resize(static_cast<usize>(plen));
          if (!transport_recv_n(payload.data(), payload.size()))
            break;
          if (masked)
            for (usize i = 0; i < payload.size(); ++i)
              payload[i] = static_cast<u8>(payload[i] ^ mask[i & 3]);
        }
        // Control frames must not be fragmented.
        if (op >= 0x8 && (!fin || payload.size() > 125)) {
          send_close_frame(1002, "Protocol Error");
          emit_local_close(1002, "Protocol Error");
          return;
        }
        if (op >= 0x8) {
          if (op == 0x9) {
            send_frame(0xA, payload.data(), payload.size());
          } else if (op == 0xA) {
            awaiting_pong_.store(false);
          } else if (op == 0x8) {
            u16 code = 1005;
            std::string reason;
            if (payload.size() >= 2) {
              code = static_cast<u16>((static_cast<u16>(payload[0]) << 8) |
                                      static_cast<u16>(payload[1]));
              reason.assign(payload.begin() + 2, payload.end());
            }
            record_close(code, reason);
            // Echo close back if we haven't already.
            if (state_.load() != ws_ready_state::closing) {
              send_frame(0x8, payload.data(), payload.size());
            }
            ws_event ev;
            ev.kind = ws_event_kind::close;
            ev.code = code;
            ev.reason = reason;
            ev.was_clean = true;
            push_event(std::move(ev));
            state_.store(ws_ready_state::closed);
            stop_.store(true);
            out_cv_.notify_all();
            return;
          }
          continue;
        }
        // Data frame: 0x1 text, 0x2 binary, 0x0 continuation
        if (op == 0x1 || op == 0x2) {
          if (cont_opcode != 0) {
            send_close_frame(1002, "Protocol Error");
            emit_local_close(1002, "Protocol Error");
            return;
          }
          assembled = std::move(payload);
          cont_opcode = op;
          compressed_message = rsv1;
        } else if (op == 0x0) {
          if (cont_opcode == 0) {
            send_close_frame(1002, "Protocol Error");
            emit_local_close(1002, "Protocol Error");
            return;
          }
          assembled.insert(assembled.end(), payload.begin(), payload.end());
        }
        if (fin) {
          std::vector<u8> message;
          if (compressed_message) {
            bool too_big = false;
            if (!inflate_message(assembled, message, too_big)) {
              const u16 code = too_big ? 1009 : 1002;
              const char* reason = too_big ? "Message Too Big" : "Bad Compressed Data";
              send_close_frame(code, reason);
              emit_local_close(code, reason);
              return;
            }
          } else {
            message = std::move(assembled);
          }
          ws_event ev;
          if (cont_opcode == 0x1) {
            ev.kind = ws_event_kind::message_text;
            ev.text.assign(message.begin(), message.end());
          } else {
            ev.kind = ws_event_kind::message_binary;
            ev.binary = std::move(message);
          }
          push_event(std::move(ev));
          assembled.clear();
          cont_opcode = 0;
          compressed_message = false;
        }
      }
    });

    // Writer loop on this thread.
    auto check_idle = [this]() {
      if (options_.idle_timeout_ms == 0)
        return true;
      const i64 now = monotonic_ms();
      if (awaiting_pong_.load()) {
        if (now - ping_sent_ms_.load() >= static_cast<i64>(options_.pong_timeout_ms)) {
          send_close_frame(1011, "pong timeout");
          emit_local_close(1011, "pong timeout");
          return false;
        }
        return true;
      }
      if (now - last_frame_ms_.load() >= static_cast<i64>(options_.idle_timeout_ms)) {
        if (!send_frame(0x9, nullptr, 0)) {
          stop_.store(true);
          close_socket_now(true);
          out_cv_.notify_all();
          return false;
        }
        awaiting_pong_.store(true);
        ping_sent_ms_.store(now);
      }
      return true;
    };

    while (!stop_.load()) {
      std::unique_lock<std::mutex> lk(out_mu_);
      if (options_.idle_timeout_ms == 0) {
        out_cv_.wait(lk, [&] { return stop_.load() || !out_q_.empty(); });
      } else {
        out_cv_.wait_for(lk, std::chrono::milliseconds(250),
                         [&] { return stop_.load() || !out_q_.empty(); });
      }
      while (!out_q_.empty()) {
        out_msg m = std::move(out_q_.front());
        out_q_.pop();
        lk.unlock();
        switch (m.op) {
        case out_op::text:
        case out_op::binary: {
          const u8 opcode = m.op == out_op::text ? 0x1 : 0x2;
          bool compressed = false;
          std::vector<u8> wire;
          const u8* data = m.bytes.data();
          usize size = m.bytes.size();
          if (deflate_ && deflate_->negotiated) {
            if (!deflate_message(m.bytes.data(), m.bytes.size(), wire)) {
              send_close_frame(1011, "compression failed");
              emit_local_close(1011, "compression failed");
              break;
            }
            data = wire.data();
            size = wire.size();
            compressed = true;
          }
          send_message(opcode, data, size, m.max_fragment_bytes, compressed);
          if (buffered_.load() >= m.bytes.size())
            buffered_.fetch_sub(m.bytes.size());
        } break;
        case out_op::close: {
          std::string reason(m.bytes.begin(), m.bytes.end());
          send_close_frame(m.code, reason);
          stop_.store(true);
        } break;
        }
        lk.lock();
      }
      lk.unlock();
      if (!check_idle())
        break;
    }

    close_socket_now(true);
    if (reader.joinable())
      reader.join();
    if (state_.load() != ws_ready_state::closed) {
      ws_event ev;
      ev.kind = ws_event_kind::close;
      ev.code = 1000;
      ev.was_clean = true;
      push_event(std::move(ev));
      state_.store(ws_ready_state::closed);
    }
  }

} // namespace fxe::net
