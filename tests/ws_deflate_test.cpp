#include "../src/net/websocket_client.hpp"

#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_handle = SOCKET;
static constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
using socket_addr_len = int;
static int sock_close(socket_handle s) {
  return ::closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
static constexpr socket_handle kInvalidSocket = -1;
using socket_addr_len = socklen_t;
static int sock_close(socket_handle s) {
  return ::close(s);
}
#endif

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

#ifdef _WIN32
  struct wsa_init {
    wsa_init() {
      WSADATA d;
      WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~wsa_init() {
      WSACleanup();
    }
  };
#endif

  bool send_all(socket_handle s, const std::uint8_t* data, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
#ifdef _WIN32
      int sent = ::send(s, reinterpret_cast<const char*>(data + off), static_cast<int>(n - off), 0);
#else
      ssize_t sent = ::send(s, data + off, n - off, 0);
#endif
      if (sent <= 0)
        return false;
      off += static_cast<std::size_t>(sent);
    }
    return true;
  }

  bool recv_n(socket_handle s, std::uint8_t* data, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
#ifdef _WIN32
      int got = ::recv(s, reinterpret_cast<char*>(data + off), static_cast<int>(n - off), 0);
#else
      ssize_t got = ::recv(s, data + off, n - off, 0);
#endif
      if (got <= 0)
        return false;
      off += static_cast<std::size_t>(got);
    }
    return true;
  }

  bool read_http_headers(socket_handle s, std::string& out) {
    out.clear();
    while (out.size() < 16384) {
      std::uint8_t c = 0;
      if (!recv_n(s, &c, 1))
        return false;
      out.push_back(static_cast<char>(c));
      if (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0)
        return true;
    }
    return false;
  }

  bool raw_deflate(const std::string& in, std::vector<std::uint8_t>& out) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK)
      return false;
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    stream.avail_in = static_cast<uInt>(in.size());
    std::uint8_t buf[256];
    out.clear();
    bool ok = true;
    do {
      stream.next_out = reinterpret_cast<Bytef*>(buf);
      stream.avail_out = sizeof(buf);
      int ret = deflate(&stream, Z_SYNC_FLUSH);
      if (ret != Z_OK) {
        ok = false;
        break;
      }
      out.insert(out.end(), buf, buf + (sizeof(buf) - stream.avail_out));
    } while (stream.avail_in != 0 || stream.avail_out == 0);
    deflateEnd(&stream);
    if (!ok || out.size() < 4)
      return false;
    out.resize(out.size() - 4);
    return true;
  }

  bool raw_inflate(std::vector<std::uint8_t> in, std::string& out) {
    in.insert(in.end(), {0x00, 0x00, 0xff, 0xff});
    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK)
      return false;
    stream.next_in = reinterpret_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());
    std::uint8_t buf[256];
    std::vector<std::uint8_t> decoded;
    bool ok = true;
    do {
      stream.next_out = reinterpret_cast<Bytef*>(buf);
      stream.avail_out = sizeof(buf);
      int ret = inflate(&stream, Z_SYNC_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END) {
        ok = false;
        break;
      }
      decoded.insert(decoded.end(), buf, buf + (sizeof(buf) - stream.avail_out));
    } while (stream.avail_in != 0 || stream.avail_out == 0);
    inflateEnd(&stream);
    if (!ok)
      return false;
    out.assign(decoded.begin(), decoded.end());
    return true;
  }

  bool send_ws_frame(socket_handle s, std::uint8_t first,
                     const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    frame.push_back(first);
    if (payload.size() < 126) {
      frame.push_back(static_cast<std::uint8_t>(payload.size()));
    } else {
      return false;
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return send_all(s, frame.data(), frame.size());
  }

  bool read_client_frame(socket_handle s, std::uint8_t& first, std::vector<std::uint8_t>& payload) {
    std::uint8_t hdr[2] = {};
    if (!recv_n(s, hdr, 2))
      return false;
    first = hdr[0];
    bool masked = (hdr[1] & 0x80) != 0;
    std::uint64_t len = hdr[1] & 0x7f;
    if (len == 126) {
      std::uint8_t ext[2];
      if (!recv_n(s, ext, 2))
        return false;
      len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      return false;
    }
    std::uint8_t mask[4] = {};
    if (masked && !recv_n(s, mask, 4))
      return false;
    payload.resize(static_cast<std::size_t>(len));
    if (len != 0 && !recv_n(s, payload.data(), payload.size()))
      return false;
    if (masked) {
      for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(payload[i] ^ mask[i & 3]);
    }
    return masked;
  }

  struct server_result {
    bool ok = false;
    std::string err;
    std::string received;
    bool saw_offer = false;
    bool saw_client_rsv1 = false;
  };

  socket_handle listen_loopback(std::uint16_t& port) {
    socket_handle s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket)
      return kInvalidSocket;
    int one = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      sock_close(s);
      return kInvalidSocket;
    }
    if (::listen(s, 1) != 0) {
      sock_close(s);
      return kInvalidSocket;
    }
    sockaddr_in bound{};
    socket_addr_len len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
      sock_close(s);
      return kInvalidSocket;
    }
    port = ntohs(bound.sin_port);
    return s;
  }

  void server_main(socket_handle listener, server_result& result) {
    sockaddr_in peer{};
    socket_addr_len peer_len = sizeof(peer);
    socket_handle client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    sock_close(listener);
    if (client == kInvalidSocket) {
      result.err = "accept failed";
      return;
    }

    std::string headers;
    if (!read_http_headers(client, headers)) {
      result.err = "request headers read failed";
      sock_close(client);
      return;
    }
    result.saw_offer = headers.find("permessage-deflate") != std::string::npos;

    const char* response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: unused-by-test-client\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover; server_max_window_bits=15; client_max_window_bits=15\r\n"
        "\r\n";
    if (!send_all(client, reinterpret_cast<const std::uint8_t*>(response), std::strlen(response))) {
      result.err = "handshake response send failed";
      sock_close(client);
      return;
    }

    std::vector<std::uint8_t> compressed;
    if (!raw_deflate("server compressed payload", compressed) ||
        !send_ws_frame(client, 0xC1, compressed)) {
      result.err = "compressed server message send failed";
      sock_close(client);
      return;
    }

    std::uint8_t first = 0;
    std::vector<std::uint8_t> client_payload;
    if (!read_client_frame(client, first, client_payload)) {
      result.err = "client frame read failed";
      sock_close(client);
      return;
    }
    result.saw_client_rsv1 = (first & 0x40) != 0;
    if ((first & 0x0f) != 0x1 || !result.saw_client_rsv1) {
      result.err = "client did not send compressed text frame";
      sock_close(client);
      return;
    }
    if (!raw_inflate(client_payload, result.received)) {
      result.err = "client payload inflate failed";
      sock_close(client);
      return;
    }

    std::vector<std::uint8_t> close_body = {0x03, 0xe8};
    (void)send_ws_frame(client, 0x88, close_body);
    sock_close(client);
    result.ok = true;
  }

  void test_permessage_deflate_round_trip() {
#ifdef _WIN32
    wsa_init wsa;
#endif
    std::uint16_t port = 0;
    socket_handle listener = listen_loopback(port);
    CHECK(listener != kInvalidSocket);
    CHECK(port != 0);
    if (listener == kInvalidSocket || port == 0)
      return;

    server_result server;
    std::thread thread([&] { server_main(listener, server); });

    fxe::net::ws_client_options options;
    options.compress = true;
    fxe::net::websocket_client client(options);
    std::ostringstream url;
    url << "ws://127.0.0.1:" << port << "/deflate";
    CHECK(client.connect(url.str(), {}));

    bool opened = false;
    bool got_message = false;
    std::string message;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !got_message) {
      for (auto& ev : client.drain_events()) {
        if (ev.kind == fxe::net::ws_event_kind::open) {
          opened = true;
          client.send_text("client compressed payload");
        } else if (ev.kind == fxe::net::ws_event_kind::message_text) {
          got_message = true;
          message = ev.text;
        } else if (ev.kind == fxe::net::ws_event_kind::error_) {
          std::fprintf(stderr, "websocket error: %s\n", ev.text.c_str());
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.close(1000, "done");
    thread.join();

    CHECK(opened);
    CHECK(got_message);
    CHECK(message == "server compressed payload");
    CHECK(client.negotiated_extensions().find("permessage-deflate") != std::string::npos);
    CHECK(server.ok);
    if (!server.err.empty())
      std::fprintf(stderr, "server error: %s\n", server.err.c_str());
    CHECK(server.saw_offer);
    CHECK(server.saw_client_rsv1);
    CHECK(server.received == "client compressed payload");
  }
} // namespace

int main() {
  test_permessage_deflate_round_trip();
  std::fprintf(stderr, "ws_deflate_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
