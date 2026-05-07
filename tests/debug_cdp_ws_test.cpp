#include "../src/debug/cdp_ws.hpp"
#include "../src/debug/server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef FXE_HAS_V8
#include <fxe/v8_host.hpp>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
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

  using socket_t = fxe::debug::cdp_ws::socket_t;

#if defined(_WIN32)
  constexpr socket_t kInvalidSocket = INVALID_SOCKET;
  void close_socket(socket_t s) {
    closesocket(s);
  }
#else
  constexpr socket_t kInvalidSocket = -1;
  void close_socket(socket_t s) {
    ::close(s);
  }
#endif

  void init_sockets() {
#if defined(_WIN32)
    static bool once = [] {
      WSADATA wsa{};
      return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)once;
#endif
  }

  socket_t connect_loopback(unsigned port) {
    init_sockets();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket)
      return kInvalidSocket;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close_socket(s);
      return kInvalidSocket;
    }
    return s;
  }

  void set_recv_timeout(socket_t s, int milliseconds) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  }

  std::string recv_until(socket_t s, std::string_view marker) {
    std::string out;
    char buf[512];
    while (out.find(marker) == std::string::npos) {
#if defined(_WIN32)
      int n = ::recv(s, buf, sizeof(buf), 0);
#else
      ssize_t n = ::recv(s, buf, sizeof(buf), 0);
#endif
      if (n <= 0)
        break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  [[maybe_unused]] std::string recv_to_close(socket_t s) {
    std::string out;
    char buf[512];
    for (;;) {
#if defined(_WIN32)
      int n = ::recv(s, buf, sizeof(buf), 0);
#else
      ssize_t n = ::recv(s, buf, sizeof(buf), 0);
#endif
      if (n <= 0)
        break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  [[maybe_unused]] bool send_json_line(socket_t s, std::string_view line) {
    std::string wire(line);
    wire.push_back('\n');
    return fxe::debug::cdp_ws::send_all(s, wire);
  }

  [[maybe_unused]] bool recv_json_line(socket_t s, std::string& buf, nlohmann::ordered_json& out) {
    for (;;) {
      auto nl = buf.find('\n');
      if (nl != std::string::npos) {
        auto line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
          continue;
        out = nlohmann::ordered_json::parse(line);
        return true;
      }
      char chunk[4096];
#if defined(_WIN32)
      int n = ::recv(s, chunk, sizeof(chunk), 0);
#else
      ssize_t n = ::recv(s, chunk, sizeof(chunk), 0);
#endif
      if (n <= 0)
        return false;
      buf.append(chunk, static_cast<std::size_t>(n));
    }
  }

  bool websocket_handshake(socket_t s, unsigned port, std::string* header_out = nullptr) {
    std::string hs = "GET /devtools/page/fxe-main HTTP/1.1\r\n"
                     "Host: 127.0.0.1:" +
                     std::to_string(port) +
                     "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!fxe::debug::cdp_ws::send_all(s, hs))
      return false;
    auto header = recv_until(s, "\r\n\r\n");
    if (header_out)
      *header_out = header;
    return header.find("HTTP/1.1 101 Switching Protocols") == 0;
  }

  [[maybe_unused]] socket_t connect_websocket(unsigned port) {
    socket_t s = connect_loopback(port);
    if (s == kInvalidSocket)
      return kInvalidSocket;
    set_recv_timeout(s, 2000);
    if (!websocket_handshake(s, port)) {
      close_socket(s);
      return kInvalidSocket;
    }
    set_recv_timeout(s, 20);
    return s;
  }

  [[maybe_unused]] bool send_ws_request(socket_t s, int id, std::string_view method,
                                        std::string_view params = "{}") {
    std::string request = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + std::string(method) +
                          "\",\"params\":" + std::string(params) + "}";
    return fxe::debug::cdp_ws::send_all(
        s, fxe::debug::cdp_ws::encode_frame(request, 0x1, true, 0x01020304u));
  }

  [[maybe_unused]] bool pump_until_ws_id(fxe::debug::server& srv, socket_t s,
                                         fxe::debug::cdp_ws::reader& reader, int id,
                                         nlohmann::ordered_json& out) {
    for (int i = 0; i < 250; ++i) {
      srv.pump_tasks();
      auto rr = reader.read_text(s);
      if (rr.state == fxe::debug::cdp_ws::read_result::status::message) {
        out = nlohmann::ordered_json::parse(rr.message);
        if (out.contains("id") && out.at("id").get<int>() == id)
          return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  [[maybe_unused]] bool recv_ws_method(socket_t s, fxe::debug::cdp_ws::reader& reader,
                                       std::string_view method, nlohmann::ordered_json& out) {
    for (int i = 0; i < 250; ++i) {
      auto rr = reader.read_text(s);
      if (rr.state == fxe::debug::cdp_ws::read_result::status::message) {
        out = nlohmann::ordered_json::parse(rr.message);
        if (out.contains("method") && out.at("method").is_string() &&
            out.at("method").get<std::string>() == method)
          return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  void test_sha1_and_handshake_accept() {
    using namespace fxe::debug::cdp_ws;
    CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    std::string raw = "GET /devtools/page/fxe-main HTTP/1.1\r\n"
                      "Host: 127.0.0.1:9229\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    http_request req;
    CHECK(parse_http_request(raw, req));
    auto accept = websocket_accept(req.header("Sec-WebSocket-Key"));
    CHECK(accept.has_value());
    CHECK(*accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    auto response = handshake_response(req);
    CHECK(response.find("HTTP/1.1 101 Switching Protocols") == 0);
    CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
  }

  void test_frame_round_trip() {
    using namespace fxe::debug::cdp_ws;
    std::string buf = encode_frame("hello", 0x1, true, 0x01020304u);
    CHECK((static_cast<unsigned char>(buf[1]) & 0x80u) != 0);
    frame fr;
    std::string error;
    CHECK(decode_frame_from_buffer(buf, fr, error, true));
    CHECK(error.empty());
    CHECK(fr.opcode == 0x1);
    CHECK(fr.payload == "hello");
    CHECK(buf.empty());
  }

  void test_frame_size_limit() {
    using namespace fxe::debug::cdp_ws;
    std::string buf;
    buf.push_back(static_cast<char>(0x81u));
    buf.push_back(static_cast<char>(0x80u | 127u));
    const std::uint64_t len = static_cast<std::uint64_t>(max_ws_frame_bytes) + 1u;
    for (int i = 7; i >= 0; --i)
      buf.push_back(static_cast<char>((len >> (i * 8)) & 0xffu));
    buf.append("\0\0\0\0", 4);

    frame fr;
    std::string error;
    CHECK(!decode_frame_from_buffer(buf, fr, error, true));
    CHECK(error == "WebSocket frame exceeds 16777216-byte limit");
  }

  void test_json_version_descriptor() {
    using namespace fxe::debug;
    auto response = make_cdp_discovery_http_response("/json/version", "127.0.0.1:9229", 9229);
    CHECK(response.has_value());
    auto split = response->find("\r\n\r\n");
    CHECK(split != std::string::npos);
    auto body = nlohmann::ordered_json::parse(response->substr(split + 4));
    CHECK(body.is_object());
    CHECK(body.at("Protocol-Version").get<std::string>() == "1.3");
    CHECK(body.at("webSocketDebuggerUrl").get<std::string>() ==
          "ws://127.0.0.1:9229/devtools/page/fxe-main");
  }

#ifdef FXE_HAS_V8
  void test_complete_cdp_exchange() {
    using namespace fxe::debug;
    fxe::js::host host;
    server_options opts;
    opts.port = 0;
    opts.host = "127.0.0.1";
    server srv(opts);
    srv.attach_host(&host);
    if (!srv.start()) {
      CHECK(false);
      return;
    }
    unsigned port = srv.bound_port();

    socket_t s = connect_loopback(port);
    if (s == kInvalidSocket) {
      CHECK(false);
      srv.stop();
      return;
    }
    set_recv_timeout(s, 2000);
    std::string hs = "GET /devtools/page/fxe-main HTTP/1.1\r\n"
                     "Host: 127.0.0.1:" +
                     std::to_string(port) +
                     "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n";
    CHECK(fxe::debug::cdp_ws::send_all(s, hs));
    auto header = recv_until(s, "\r\n\r\n");
    CHECK(header.find("HTTP/1.1 101 Switching Protocols") == 0);

    std::string request = R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"1+1"}})";
    CHECK(fxe::debug::cdp_ws::send_all(
        s, fxe::debug::cdp_ws::encode_frame(request, 0x1, true, 0x01020304u)));
    for (int i = 0; i < 100; ++i) {
      srv.pump_tasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fxe::debug::cdp_ws::reader reader(false);
    auto rr = reader.read_text(s);
    CHECK(rr.state == fxe::debug::cdp_ws::read_result::status::message);
    auto reply = nlohmann::ordered_json::parse(rr.message);
    CHECK(reply.at("id").get<int>() == 1);
    CHECK(reply.at("result").at("result").at("value").get<int>() == 2);

    fxe::debug::cdp_ws::send_all(s, fxe::debug::cdp_ws::encode_frame("", 0x8, true, 0x01020304u));
    close_socket(s);
    srv.stop();
  }

  void test_multiclient_cdp_websocket_exchange_events_and_cap() {
    using namespace fxe::debug;
    fxe::js::host host;
    server_options opts;
    opts.port = 0;
    opts.host = "127.0.0.1";
    server srv(opts);
    srv.attach_host(&host);
    if (!srv.start()) {
      CHECK(false);
      return;
    }
    unsigned port = srv.bound_port();

    socket_t a = connect_websocket(port);
    socket_t b = connect_websocket(port);
    if (a == kInvalidSocket || b == kInvalidSocket) {
      CHECK(false);
      if (a != kInvalidSocket)
        close_socket(a);
      if (b != kInvalidSocket)
        close_socket(b);
      srv.stop();
      return;
    }
    fxe::debug::cdp_ws::reader ra(false);
    fxe::debug::cdp_ws::reader rb(false);
    nlohmann::ordered_json ja, jb;

    CHECK(send_ws_request(a, 11, "Runtime.evaluate", "{\"expression\":\"10+5\"}"));
    CHECK(send_ws_request(b, 22, "Runtime.evaluate", "{\"expression\":\"7+8\"}"));
    CHECK(pump_until_ws_id(srv, a, ra, 11, ja));
    CHECK(pump_until_ws_id(srv, b, rb, 22, jb));
    CHECK(ja.at("id").get<int>() == 11);
    CHECK(jb.at("id").get<int>() == 22);
    CHECK(ja.at("result").at("result").at("value").get<int>() == 15);
    CHECK(jb.at("result").at("result").at("value").get<int>() == 15);

    CHECK(send_ws_request(a, 12, "Console.enable"));
    CHECK(send_ws_request(b, 23, "Console.enable"));
    CHECK(pump_until_ws_id(srv, a, ra, 12, ja));
    CHECK(pump_until_ws_id(srv, b, rb, 23, jb));
    srv.emit_console("log", "ws-both");
    CHECK(recv_ws_method(a, ra, "Console.messageAdded", ja));
    CHECK(recv_ws_method(b, rb, "Console.messageAdded", jb));
    CHECK(ja.at("params").at("text").get<std::string>() == "ws-both");
    CHECK(jb.at("params").at("text").get<std::string>() == "ws-both");

    close_socket(a);
    for (int i = 0; i < 20; ++i) {
      srv.pump_tasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    srv.emit_console("log", "ws-survivor");
    CHECK(recv_ws_method(b, rb, "Console.messageAdded", jb));
    CHECK(jb.at("params").at("text").get<std::string>() == "ws-survivor");
    close_socket(b);
    srv.stop();

    server srv_cap(opts);
    srv_cap.attach_host(&host);
    if (!srv_cap.start()) {
      CHECK(false);
      return;
    }
    std::vector<socket_t> clients;
    for (int i = 0; i < 8; ++i) {
      socket_t s = connect_websocket(srv_cap.bound_port());
      CHECK(s != kInvalidSocket);
      if (s != kInvalidSocket)
        clients.push_back(s);
    }
    socket_t overflow = connect_loopback(srv_cap.bound_port());
    CHECK(overflow != kInvalidSocket);
    if (overflow != kInvalidSocket) {
      set_recv_timeout(overflow, 2000);
      std::string header;
      (void)websocket_handshake(overflow, srv_cap.bound_port(), &header);
      CHECK(header.find("HTTP/1.1 503 Service Unavailable") == 0);
      CHECK(header.find("server is full") != std::string::npos);
      close_socket(overflow);
    }
    for (auto s : clients)
      close_socket(s);
    srv_cap.stop();
  }

  void test_mixed_ndjson_and_websocket_clients() {
    using namespace fxe::debug;
    fxe::js::host host;
    server_options opts;
    opts.port = 0;
    opts.host = "127.0.0.1";
    server srv(opts);
    srv.attach_host(&host);
    if (!srv.start()) {
      CHECK(false);
      return;
    }
    unsigned port = srv.bound_port();

    socket_t nd = connect_loopback(port);
    socket_t ws = connect_websocket(port);
    if (nd == kInvalidSocket || ws == kInvalidSocket) {
      CHECK(false);
      if (nd != kInvalidSocket)
        close_socket(nd);
      if (ws != kInvalidSocket)
        close_socket(ws);
      srv.stop();
      return;
    }
    set_recv_timeout(nd, 20);
    fxe::debug::cdp_ws::reader rw(false);
    std::string nbuf;
    nlohmann::ordered_json nreply, wreply;

    CHECK(send_json_line(nd,
                         R"({"id":31,"method":"Runtime.evaluate","params":{"expression":"3*7"}})"));
    CHECK(send_ws_request(ws, 41, "Runtime.evaluate", "{\"expression\":\"6*7\"}"));
    for (int i = 0; i < 250; ++i) {
      srv.pump_tasks();
      if (nreply.is_null())
        (void)recv_json_line(nd, nbuf, nreply);
      if (wreply.is_null()) {
        auto rr = rw.read_text(ws);
        if (rr.state == fxe::debug::cdp_ws::read_result::status::message)
          wreply = nlohmann::ordered_json::parse(rr.message);
      }
      if (!nreply.is_null() && !wreply.is_null())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(nreply.at("id").get<int>() == 31);
    CHECK(wreply.at("id").get<int>() == 41);
    CHECK(nreply.at("result").at("result").at("value").get<int>() == 21);
    CHECK(wreply.at("result").at("result").at("value").get<int>() == 42);

    close_socket(nd);
    close_socket(ws);
    srv.stop();
  }
#endif
} // namespace

int main(int argc, char** argv) {
#ifdef FXE_HAS_V8
  fxe::js::initialize(argc > 0 ? argv[0] : "");
#else
  (void)argc;
  (void)argv;
#endif

  test_sha1_and_handshake_accept();
  test_frame_round_trip();
  test_frame_size_limit();
  test_json_version_descriptor();
#ifdef FXE_HAS_V8
  test_complete_cdp_exchange();
  test_multiclient_cdp_websocket_exchange_events_and_cap();
  test_mixed_ndjson_and_websocket_clients();
  fxe::js::shutdown();
#endif

  std::fprintf(stderr, "debug_cdp_ws_test: %d passed, %d failed\n", g_pass, g_fail);
  std::fflush(stderr);
#ifdef FXE_HAS_V8
  std::_Exit(g_fail == 0 ? 0 : 1);
#else
  return g_fail == 0 ? 0 : 1;
#endif
}
