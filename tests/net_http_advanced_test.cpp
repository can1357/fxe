#include "net/http_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

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

  void send_all(int fd, const std::string& bytes) {
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
      ssize_t n = ::send(fd, p, left, 0);
      if (n <= 0)
        return;
      p += n;
      left -= static_cast<std::size_t>(n);
    }
  }

  std::string recv_headers(int fd) {
    std::string out;
    char buf[512];
    while (out.find("\r\n\r\n") == std::string::npos && out.size() < 16384) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  class one_shot_http_server {
  public:
    using handler_fn = std::function<std::string(const std::string&)>;

    explicit one_shot_http_server(handler_fn handler) : handler_(std::move(handler)) {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd_ >= 0);
      int yes = 1;
      (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      CHECK(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
      CHECK(::listen(fd_, 1) == 0);
      socklen_t len = sizeof(addr);
      CHECK(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
      port_ = ntohs(addr.sin_port);
      thread_ = std::thread([this] { run(); });
    }

    ~one_shot_http_server() {
      if (thread_.joinable())
        thread_.join();
      if (fd_ >= 0)
        ::close(fd_);
    }

    int port() const {
      return port_;
    }

  private:
    void run() {
      int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0)
        return;
      std::string req = recv_headers(client);
      std::string resp = handler_(req);
      send_all(client, resp);
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }

    int fd_ = -1;
    int port_ = 0;
    handler_fn handler_;
    std::thread thread_;
  };

  fxe::net::http_response request_sync(fxe::net::http_request req) {
    fxe::net::http_response out;
    bool done = false;
    fxe::net::http_client::instance().submit(std::move(req), [&](fxe::net::http_response resp) {
      out = std::move(resp);
      done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      fxe::net::http_client::instance().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done);
    return out;
  }

  bool contains_header_value(const std::string& request, const std::string& needle) {
    return request.find(needle) != std::string::npos;
  }

  void test_cookie_round_trip() {
    if (!fxe::net::http_client::available()) {
      std::puts("SKIP cookie round-trip: libcurl unavailable");
      return;
    }
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    auto& client = fxe::net::http_client::instance();
    client.set_cookie_file_path(
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/fxe_net_cookie_test.txt");
    client.cookies().clear();

    one_shot_http_server setter([](const std::string&) {
      return "HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc; Path=/; HttpOnly\r\nContent-Length: "
             "2\r\n\r\nok";
    });
    fxe::net::http_request first;
    first.url = "http://127.0.0.1:" + std::to_string(setter.port()) + "/set";
    auto r1 = request_sync(std::move(first));
    CHECK(r1.error.empty());
    CHECK(r1.status == 200);
    CHECK(client.cookies().pick_for_request("http://127.0.0.1/next").find("sid=abc") !=
          std::string::npos);
    fxe::net::cookie_filter filter;
    filter.url = "http://127.0.0.1/next";
    const auto active = client.cookies().get_all(filter);
    CHECK(active.size() == 1);
    if (!active.empty()) {
      CHECK(active.front().name == "sid");
      CHECK(active.front().http_only);
    }
    CHECK(!client.cookies().set_from_header("third=1; SameSite=None", "http://127.0.0.1/"));
    CHECK(client.cookies().set_from_header("third=1; SameSite=None; Secure", "https://127.0.0.1/"));
    CHECK(client.cookies().pick_for_request("http://127.0.0.1/").find("third=1") ==
          std::string::npos);
    CHECK(client.cookies().pick_for_request("https://127.0.0.1/").find("third=1") !=
          std::string::npos);

    one_shot_http_server checker([](const std::string& req) {
      const bool saw_cookie = contains_header_value(req, "Cookie: sid=abc") ||
                              contains_header_value(req, "cookie: sid=abc");
      if (!saw_cookie)
        return std::string("HTTP/1.1 400 Bad Request\r\nContent-Length: 14\r\n\r\nmissing cookie");
      return std::string("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\ncookie");
    });
    fxe::net::http_request second;
    second.url = "http://127.0.0.1:" + std::to_string(checker.port()) + "/next";
    auto r2 = request_sync(std::move(second));
    CHECK(r2.error.empty());
    CHECK(r2.status == 200);
    CHECK(r2.body == "cookie");
    client.cookies().clear();
  }

  void test_local_proxy() {
    if (!fxe::net::http_client::available()) {
      std::puts("SKIP proxy request: libcurl unavailable");
      return;
    }
    if (!std::getenv("HTTPS_PROXY")) {
      std::puts("SKIP proxy request: HTTPS_PROXY not set");
      return;
    }

    one_shot_http_server proxy([](const std::string& req) {
      const bool absolute_form = req.rfind("GET http://example.test/proxied HTTP/", 0) == 0;
      if (!absolute_form)
        return std::string("HTTP/1.1 400 Bad Request\r\nContent-Length: 9\r\n\nnot proxy");
      return std::string("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nvia proxy");
    });

    std::string old_http_proxy = std::getenv("HTTP_PROXY") ? std::getenv("HTTP_PROXY") : "";
    std::string old_no_proxy = std::getenv("NO_PROXY") ? std::getenv("NO_PROXY") : "";
    const bool had_http_proxy = std::getenv("HTTP_PROXY") != nullptr;
    const bool had_no_proxy = std::getenv("NO_PROXY") != nullptr;
    ::setenv("HTTP_PROXY", ("http://127.0.0.1:" + std::to_string(proxy.port())).c_str(), 1);
    ::unsetenv("NO_PROXY");

    fxe::net::http_request req;
    req.url = "http://example.test/proxied";
    auto resp = request_sync(std::move(req));

    if (had_http_proxy)
      ::setenv("HTTP_PROXY", old_http_proxy.c_str(), 1);
    else
      ::unsetenv("HTTP_PROXY");
    if (had_no_proxy)
      ::setenv("NO_PROXY", old_no_proxy.c_str(), 1);
    else
      ::unsetenv("NO_PROXY");

    CHECK(resp.error.empty());
    CHECK(resp.status == 200);
    CHECK(resp.body == "via proxy");
  }
} // namespace

int main() {
  test_cookie_round_trip();
  test_local_proxy();
  std::printf("fxe_net_http_advanced_tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
