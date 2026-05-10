#include "../src/debug/server.hpp"
#include "../src/net/http2_server.hpp"
#include "../src/net/tls_server.hpp"
#include <fxe/v8_host.hpp>

#ifndef FXE_V8_ICUDTL_PATH
#define FXE_V8_ICUDTL_PATH ""
#endif

#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
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

  constexpr std::string_view kCertPem = R"PEM(-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIULWllllk5MThgFlNnEn673iafmOcwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDUwNzE1MDc1OVoXDTM2MDUw
NDE1MDc1OVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAsptDxbszD+poOpDTx4PMJNNWpL3vHNTKdhqzvhC4i50m
+uPXsVlQV0gBFLX5DPF1pYJfVzdgwiomosi7LuPzyVku9RpLSCWIOksSunScdyIH
rCSG4+G+R3C1PC7GXwrgdngZg5icKFkulW6xc+3jeK+2Mthn6URz2sdq5GDIq28s
/K+FsXc35INInCbYcGApuGKzDc++laENQbuAKaZm2zQRKqQI0pSXo2H6V2TAl19x
f1pYF8chwDXqJ8Nl7MtIWtuYMOo53WKUC5U8mDYoP0VU2He8d4ad5HB8wDVEpZK5
28P/AySm+vhKWw6kU680AyJsFeZ1H8yLwbGZYNj4rwIDAQABo28wbTAdBgNVHQ4E
FgQUu7jsaydOCb+RE3NlBHTP+sOx3skwHwYDVR0jBBgwFoAUu7jsaydOCb+RE3Nl
BHTP+sOx3skwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBAABEPEI0idJ9uCXPam2Gj7kVNW5jijjm
YJTj/UHu3bC+RFew2CZijn6xklBJFfgx60ov6XQng6LqcusjX5qKP/jhd4tLhBmx
l7cz46OSdbh+ET4EajZSr+NXSP1o6E2P9bH3A5VkfEPJHtJEQMgk3hvhxSxXSk2Z
YQzX2PnBVZ7Mdq923WVOzQ4noamQtlSi3MBFiLgtR8frpMYB94H24yBRUD3guThc
f+8+7SIAJUaC50tgqhdcWov0u1pJgvdrcaj348o7ehLzqdIBpe9L+uDJFdNdmT5z
Pojw9zXzus17rnwk3nOeJ0Hf/8oVMlh/ESDhdzMftgIwHyBiLycWy/k=
-----END CERTIFICATE-----)PEM";

  constexpr std::string_view kKeyPem = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCym0PFuzMP6mg6
kNPHg8wk01akve8c1Mp2GrO+ELiLnSb649exWVBXSAEUtfkM8XWlgl9XN2DCKiai
yLsu4/PJWS71GktIJYg6SxK6dJx3IgesJIbj4b5HcLU8LsZfCuB2eBmDmJwoWS6V
brFz7eN4r7Yy2GfpRHPax2rkYMirbyz8r4Wxdzfkg0icJthwYCm4YrMNz76VoQ1B
u4AppmbbNBEqpAjSlJejYfpXZMCXX3F/WlgXxyHANeonw2Xsy0ha25gw6jndYpQL
lTyYNig/RVTYd7x3hp3kcHzANUSlkrnbw/8DJKb6+EpbDqRTrzQDImwV5nUfzIvB
sZlg2PivAgMBAAECggEAGXAPqPvOe/fQvHagEwxsaNpIvtHmWl7cLxICg5FyF0Bc
quMEd1fXH3c74C1CuVsyfE4jMhLLDxxdwFWCg10n/YdcLsB99FqUGmlS04eEOVt5
aEUTiSU/qoEc7uNikWrFKVpVl+6GXyDEh7fqQi6hdTDhbEByHEEJlyFL0hcOvYus
5rVqauhGdIuxZ1ZUi33dgOPS59Q/K/fdy9AyopzvqRDY0UqJmRi6E9V0DoF2eiMI
FftyMNEOs0ZazfquxsG0YDdfVXFfpO+d75sVZeImrr8zSVKJ2CmQOHtq2Q8w4mE0
70B6C76WqtQQIiDynVwINuNJVQ8AqKzio1Jnci0iYQKBgQDoj215zHEbLymljVXA
gmDJsNsGdlpdW8qoMJ2htB2+YS1124TA3/qv3OTLgX9KA4l69ROlwEYRXPLtkql4
s5skaWXKS8DclgHzVhscfrd4+sJJJrhR7Vy7qFYMQIf6HwQVzwBJgro9vPr6ecOO
lUUZOaSOgYxN6oRGNUwsvtpoGQKBgQDEm7XBgToTXF/IKk43h69fL5deKToOVjkV
C3eKrQ6aKPBW5z90m7RsEOIBlS7GZmA97mVo4toaotzK/Gdp3YPexoRaTAv88rRP
U/mTphiWVvIfX07hVSMEPhaCJf2nEyS88M40iR6zzb1hs7xIFzRAHeypu9ZZPjxj
KdnpWmUgBwKBgDrSDB6CVxlJFH+K/+VxFInu8Xbw+GokjV187mG37M36RkVJAIrI
G9/fPv86Abf2rQ8sbYu+1foOSGNOdQ7SXqsW/WftQRqJ1nR1kuXiJwWyZvGZmYUf
RBUyvpDawYnBzoa1lJ0DM5fp9JDlu1CU8KUwry5cFeCfMFWRpXKr0xIBAoGAd9F9
X0RmJE5zgQVnTag/VH8ofJYbb4lUmGK4o6b78y9n6U5c+a+6sPFJCzXjn73cgWG8
I8O8r+b5MCvKylXZe/b3yh/2Xl17Ta0buMPM0DKEtGHdLK45/Ofpx79nal7cUNlg
kdvO/j0wYU6sPDMIANs70+VJqHGpU7W5u+D/KBkCgYAsmitwmlWNAJMuEcjlSHKR
QENLXORjrGTuwyCiCmAv0pnteiiPCTjMWKe1kGUQ49whBUPkRItJ7CazX5zooEzu
/GJp5L33/lzKAx7KZy+HtAoUqSrcfixDR5mZ9lEZyzghIW5Ad0rTGXuz8uCHcG/B
76/sAfgC3tu8K6G+hHhJgw==
-----END PRIVATE KEY-----)PEM";

#if defined(_WIN32)
  using socket_t = SOCKET;
  constexpr socket_t k_invalid_socket = INVALID_SOCKET;
  void close_socket(socket_t s) {
    closesocket(s);
  }
#else
  using socket_t = int;
  constexpr socket_t k_invalid_socket = -1;
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
    if (s == k_invalid_socket)
      return k_invalid_socket;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close_socket(s);
      return k_invalid_socket;
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

  bool send_all(socket_t s, std::string_view data) {
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
#if defined(_WIN32)
      int n = ::send(s, p, static_cast<int>(left), 0);
#else
      ssize_t n = ::send(s, p, left, 0);
#endif
      if (n <= 0)
        return false;
      p += n;
      left -= static_cast<std::size_t>(n);
    }
    return true;
  }

  bool recv_json_line(socket_t s, std::string& buf, fxe::debug::json& out) {
    for (;;) {
      auto nl = buf.find('\n');
      if (nl != std::string::npos) {
        auto line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
          continue;
        out = fxe::debug::json::parse(line);
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

  bool send_request(socket_t s, int id, std::string_view method, std::string_view params = "{}") {
    std::string line = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + std::string(method) +
                       "\",\"params\":" + std::string(params) + "}\n";
    return send_all(s, line);
  }

  bool pump_until_id(fxe::debug::server& srv, socket_t s, std::string& buf, int id,
                     fxe::debug::json& out) {
    for (int i = 0; i < 250; ++i) {
      srv.pump_tasks();
      while (recv_json_line(s, buf, out)) {
        if (out.contains("id") && out.at("id").get<int>() == id)
          return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  std::string trim_copy(std::string_view value) {
    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && is_ws(static_cast<unsigned char>(value.front())))
      value.remove_prefix(1);
    while (!value.empty() && is_ws(static_cast<unsigned char>(value.back())))
      value.remove_suffix(1);
    return std::string(value);
  }

  std::string ascii_lower_copy(std::string value) {
    for (char& c : value)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
  }

  struct https_http_request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
  };

  bool write_all(fxe::net::tls_client& client, const std::string& data, std::string& err) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = client.write(data.data() + off, data.size() - off);
      if (n <= 0) {
        err = client.last_error().empty() ? "TLS write failed" : client.last_error();
        return false;
      }
      off += static_cast<std::size_t>(n);
    }
    return true;
  }

  std::optional<std::size_t>
  parse_content_length_header(const std::map<std::string, std::string>& headers) {
    auto it = headers.find("content-length");
    if (it == headers.end())
      return 0;
    std::size_t value = 0;
    auto [ptr, ec] =
        std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    if (ec != std::errc{} || ptr != it->second.data() + it->second.size())
      return std::nullopt;
    return value;
  }

  std::optional<https_http_request> read_https_http_request(fxe::net::tls_client& client,
                                                            std::string& error) {
    std::string buffer;
    std::array<char, 4096> chunk{};
    std::optional<std::size_t> expected_total;
    for (;;) {
      const auto header_end = buffer.find("\r\n\r\n");
      if (expected_total && buffer.size() >= *expected_total)
        break;
      if (header_end != std::string::npos && !expected_total) {
        std::map<std::string, std::string> headers;
        std::string_view header_block(buffer.data(), header_end);
        std::size_t line_start = header_block.find("\r\n");
        if (line_start == std::string_view::npos) {
          error = "malformed HTTP request line";
          return std::nullopt;
        }
        line_start += 2;
        while (line_start < header_block.size()) {
          auto line_end = header_block.find("\r\n", line_start);
          if (line_end == std::string_view::npos)
            line_end = header_block.size();
          auto line = header_block.substr(line_start, line_end - line_start);
          const auto colon = line.find(':');
          if (colon != std::string_view::npos)
            headers[ascii_lower_copy(trim_copy(line.substr(0, colon)))] =
                trim_copy(line.substr(colon + 1));
          line_start = line_end + 2;
        }
        auto content_length = parse_content_length_header(headers);
        if (!content_length) {
          error = "invalid HTTP request content-length";
          return std::nullopt;
        }
        expected_total = header_end + 4 + *content_length;
      }
      const auto n = client.read(chunk.data(), chunk.size());
      if (n <= 0) {
        error = n < 0 && !client.last_error().empty()
                    ? client.last_error()
                    : "TLS client closed before full HTTP request";
        return std::nullopt;
      }
      buffer.append(chunk.data(), static_cast<std::size_t>(n));
    }

    const auto header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      error = "incomplete HTTP request headers";
      return std::nullopt;
    }
    std::string_view header_block(buffer.data(), header_end);
    auto line_end = header_block.find("\r\n");
    auto request_line =
        header_block.substr(0, line_end == std::string_view::npos ? header_block.size() : line_end);
    const auto first_space = request_line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : request_line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
      error = "malformed HTTP request line";
      return std::nullopt;
    }

    https_http_request request;
    request.method = std::string(request_line.substr(0, first_space));
    request.path =
        std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
    std::size_t line_start =
        line_end == std::string_view::npos ? header_block.size() : line_end + 2;
    while (line_start < header_block.size()) {
      line_end = header_block.find("\r\n", line_start);
      if (line_end == std::string_view::npos)
        line_end = header_block.size();
      auto line = header_block.substr(line_start, line_end - line_start);
      const auto colon = line.find(':');
      if (colon != std::string_view::npos)
        request.headers[ascii_lower_copy(trim_copy(line.substr(0, colon)))] =
            trim_copy(line.substr(colon + 1));
      line_start = line_end + 2;
    }
    auto content_length = parse_content_length_header(request.headers);
    if (!content_length) {
      error = "invalid HTTP request content-length";
      return std::nullopt;
    }
    request.body.assign(buffer.data() + header_end + 4, *content_length);
    return request;
  }

  std::string make_https_response(int status,
                                  std::vector<std::pair<std::string, std::string>> headers,
                                  std::string body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Created") << "\r\n";
    for (const auto& [key, value] : headers)
      out << key << ": " << value << "\r\n";
    out << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
    return out.str();
  }

  class scripted_https_server {
  public:
    using handler_fn = std::function<std::string(const https_http_request&)>;

    scripted_https_server(handler_fn handler) : handler_(std::move(handler)) {
      fxe::net::tls_server_options server_opts;
      server_opts.cert_pem = std::string(kCertPem);
      server_opts.key_pem = std::string(kKeyPem);
      server_opts.alpn = {"http/1.1"};
      server_opts.port = 0;
      server_ = fxe::net::tls_server::listen(server_opts, error_);
      if (!server_)
        return;
      port_ = server_->local_port();
      thread_ = std::thread([this] { run(); });
    }

    ~scripted_https_server() {
      stopping_.store(true);
      if (server_)
        server_->close();
      if (thread_.joinable())
        thread_.join();
    }

    bool ok() const {
      return server_ != nullptr && error_.empty();
    }
    int port() const {
      return port_;
    }
    std::vector<https_http_request> requests() const {
      std::lock_guard<std::mutex> lock(mu_);
      return requests_;
    }

  private:
    void run() {
      while (!stopping_.load() && server_) {
        std::string accept_err;
        auto client = server_->accept(accept_err);
        if (!client) {
          if (!stopping_.load() && !accept_err.empty() && error_.empty())
            error_ = accept_err;
          break;
        }
        std::string parse_error;
        auto request = read_https_http_request(*client, parse_error);
        if (!request) {
          if (error_.empty())
            error_ = parse_error;
          client->close();
          break;
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          requests_.push_back(*request);
        }
        std::string response = handler_(*request);
        std::string write_error;
        if (!write_all(*client, response, write_error) && error_.empty())
          error_ = write_error;
        client->close();
      }
    }

    handler_fn handler_;
    std::unique_ptr<fxe::net::tls_server> server_;
    std::thread thread_;
    std::atomic_bool stopping_{false};
    int port_ = 0;
    mutable std::mutex mu_;
    std::vector<https_http_request> requests_;
    std::string error_;
  };

  class http2_test_server {
  public:
    using handler_type =
        std::function<void(fxe::net::http2_server&, const fxe::net::http2_incoming_request&)>;

    explicit http2_test_server(handler_type handler) : handler_(std::move(handler)) {
      fxe::net::http2_server_options options;
      options.cert_pem = std::string(kCertPem);
      options.key_pem = std::string(kKeyPem);
      options.port = 0;
      server_ = fxe::net::http2_server::listen(options, error_);
      if (!server_)
        return;
      thread_ = std::thread([this] { loop(); });
    }

    ~http2_test_server() {
      stop_.store(true);
      if (server_)
        server_->close();
      if (thread_.joinable())
        thread_.join();
    }

    bool ok() const {
      return server_ != nullptr && error_.empty();
    }
    std::uint16_t port() const {
      return server_ ? server_->local_port() : 0;
    }

  private:
    void loop() {
      while (!stop_.load()) {
        std::string err;
        auto request = server_->poll(err);
        if (!err.empty()) {
          if (error_.empty())
            error_ = err;
          break;
        }
        if (!request) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }
        handler_(*server_, *request);
      }
    }

    std::unique_ptr<fxe::net::http2_server> server_;
    handler_type handler_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::string error_;
  };

  struct network_capture {
    fxe::debug::json request_will_be_sent;
    fxe::debug::json response_received;
    fxe::debug::json loading_finished;
    fxe::debug::json loading_failed;
  };

  bool wait_for_request_event(fxe::debug::server& srv, socket_t s, std::string& buf,
                              std::string_view url, network_capture& out) {
    std::string request_id;
    for (int i = 0; i < 400; ++i) {
      srv.pump_tasks();
      fxe::debug::json msg;
      while (recv_json_line(s, buf, msg)) {
        if (!msg.contains("method") || !msg.at("method").is_string())
          continue;
        const auto method = msg.at("method").get<std::string>();
        const auto& params = msg.at("params");
        if (method == "Network.requestWillBeSent") {
          const auto seen_url = params.at("request").at("url").get<std::string>();
          if (seen_url != url)
            continue;
          out.request_will_be_sent = params;
          request_id = params.at("requestId").get<std::string>();
          continue;
        }
        if (request_id.empty() || !params.contains("requestId") ||
            params.at("requestId").get<std::string>() != request_id)
          continue;
        if (method == "Network.responseReceived")
          out.response_received = params;
        else if (method == "Network.loadingFinished")
          out.loading_finished = params;
        else if (method == "Network.loadingFailed")
          out.loading_failed = params;
        if (!out.response_received.is_null() || !out.loading_failed.is_null()) {
          if (!out.loading_finished.is_null() || !out.loading_failed.is_null())
            return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  void enable_network(fxe::debug::server& srv, socket_t s, std::string& buf) {
    CHECK(send_request(s, 1, "Network.enable"));
    fxe::debug::json out;
    CHECK(pump_until_id(srv, s, buf, 1, out));
    CHECK(out.contains("result"));
  }

  void expect_ok(const fxe::js::run_result& result) {
    CHECK(result.ok);
    if (!result.ok)
      std::fprintf(stderr, "%s\n", result.message.c_str());
  }

  void test_https_network_events(fxe::js::host& host, fxe::debug::server& srv, socket_t s,
                                 std::string& buf) {
    scripted_https_server server([](const https_http_request& request) {
      CHECK(request.method == "POST");
      CHECK(request.path == "/https-network-events");
      CHECK(request.body == "hello-native-https");
      return make_https_response(200, {{"Content-Type", "text/plain"}}, "https-ok");
    });
    CHECK(server.ok());
    if (!server.ok())
      return;

    const std::string url =
        "https://127.0.0.1:" + std::to_string(server.port()) + "/https-network-events";
    const std::string source =
        "const response = __fxe_native.https.request('" + url +
        "', { method: 'POST', rejectUnauthorized: false, headers: { 'content-type': 'text/plain', "
        "'x-test': 'yes' } }, 'hello-native-https');\n"
        "if (response.statusCode !== 200 || response.body !== 'https-ok') throw new "
        "Error('unexpected https response');\n";
    expect_ok(host.run_module(source, "<https-network-events>"));

    network_capture capture;
    CHECK(wait_for_request_event(srv, s, buf, url, capture));
    CHECK(capture.loading_failed.is_null());
    CHECK(!capture.request_will_be_sent.is_null());
    CHECK(!capture.response_received.is_null());
    CHECK(!capture.loading_finished.is_null());
    CHECK(capture.request_will_be_sent.at("request").at("method").get<std::string>() == "POST");
    CHECK(capture.request_will_be_sent.at("request").at("postData").get<std::string>() ==
          "hello-native-https");
    CHECK(capture.response_received.at("response").at("status").get<int>() == 200);
    CHECK(capture.response_received.at("response").at("mimeType").get<std::string>() ==
          "text/plain");
    CHECK(capture.loading_finished.at("encodedDataLength").get<int>() == 8);
  }

  void test_http2_network_events(fxe::js::host& host, fxe::debug::server& srv, socket_t s,
                                 std::string& buf) {
    http2_test_server server(
        [](fxe::net::http2_server& http2, const fxe::net::http2_incoming_request& request) {
          CHECK(request.method == "POST");
          CHECK(request.path == "/http2-network-events");
          CHECK(request.body == "hello-native-http2");
          fxe::net::http2_response response;
          response.status = 200;
          response.headers.emplace_back("content-type", "text/plain");
          response.body = "http2-ok";
          std::string err;
          CHECK(http2.respond(request.id, response, err));
          CHECK(err.empty());
        });
    CHECK(server.ok());
    if (!server.ok())
      return;

    const std::string authority = "https://127.0.0.1:" + std::to_string(server.port());
    const std::string url = authority + "/http2-network-events";
    const std::string source =
        "const native = __fxe_native.http2;\n"
        "const handle = native.connect('" +
        authority + "', { ca: `" + std::string(kCertPem) +
        "` });\n"
        "try {\n"
        "  const streamId = native.submit(handle, { ':method': 'POST', ':path': "
        "'/http2-network-events', 'content-type': 'text/plain', '__body': 'hello-native-http2' "
        "});\n"
        "  const readHandle = native.read(handle, streamId);\n"
        "  let result = null;\n"
        "  while (result === null) { await new Promise((resolve) => setTimeout(resolve, 5)); "
        "result = native.readResult(readHandle); }\n"
        "  if (!result.ok || result.status !== 200) throw new Error(result.error ?? 'unexpected "
        "http2 result');\n"
        "  const body = new TextDecoder().decode(result.body);\n"
        "  if (body !== 'http2-ok') throw new Error('unexpected http2 body');\n"
        "} finally { native.close(handle); }\n";
    expect_ok(host.run_module(source, "<http2-network-events>"));

    network_capture capture;
    CHECK(wait_for_request_event(srv, s, buf, url, capture));
    CHECK(capture.loading_failed.is_null());
    CHECK(capture.request_will_be_sent.at("request").at("postData").get<std::string>() ==
          "hello-native-http2");
    CHECK(capture.response_received.at("response").at("status").get<int>() == 200);
    CHECK(capture.response_received.at("response").at("mimeType").get<std::string>() ==
          "text/plain");
    CHECK(capture.loading_finished.at("encodedDataLength").get<int>() == 8);
  }

  void test_https_loading_failed(fxe::js::host& host, fxe::debug::server& srv, socket_t s,
                                 std::string& buf) {
    int unused_port = 0;
    {
      scripted_https_server server([](const https_http_request&) {
        return make_https_response(200, {{"Content-Type", "text/plain"}}, "unused");
      });
      CHECK(server.ok());
      if (!server.ok())
        return;
      unused_port = server.port();
    }
    CHECK(unused_port > 0);
    if (unused_port <= 0)
      return;

    const std::string url =
        "https://127.0.0.1:" + std::to_string(unused_port) + "/https-loading-failed";
    const std::string source =
        "let threw = false;\n"
        "try {\n"
        "  __fxe_native.https.request('" +
        url +
        "', { method: 'GET', rejectUnauthorized: false, headers: { accept: 'text/plain' } }, '');\n"
        "} catch (error) {\n"
        "  threw = error instanceof Error;\n"
        "}\n"
        "if (!threw) throw new Error('expected https request failure');\n";
    expect_ok(host.run_module(source, "<https-loading-failed>"));

    network_capture capture;
    CHECK(wait_for_request_event(srv, s, buf, url, capture));
    CHECK(!capture.loading_failed.is_null());
    CHECK(capture.response_received.is_null());
    CHECK(capture.loading_finished.is_null());
    CHECK(capture.loading_failed.contains("errorText"));
    CHECK(!capture.loading_failed.at("errorText").get<std::string>().empty());
    CHECK(!capture.loading_failed.contains("canceled"));
  }
} // namespace

int main(int argc, char** argv) {
  fxe::js::initialize(argv[0], FXE_V8_ICUDTL_PATH);
  {
    fxe::js::host host;
    fxe::debug::server srv({.port = 0, .host = "127.0.0.1"});
    srv.attach_host(&host);
    CHECK(srv.start());
    if (srv.running()) {
      socket_t s = connect_loopback(srv.bound_port());
      CHECK(s != k_invalid_socket);
      if (s != k_invalid_socket) {
        set_recv_timeout(s, 20);
        std::string buf;
        enable_network(srv, s, buf);
        test_https_network_events(host, srv, s, buf);
        test_http2_network_events(host, srv, s, buf);
        test_https_loading_failed(host, srv, s, buf);
        close_socket(s);
      }
      srv.stop();
    }
  }
  fxe::js::shutdown();
  std::printf("native http network events tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
