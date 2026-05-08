#include "net/http_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fxe/types.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    usize left = bytes.size();
    while (left > 0) {
      ssize_t n = ::send(fd, p, left, 0);
      if (n <= 0)
        return;
      p += n;
      left -= static_cast<usize>(n);
    }
  }

  std::string recv_headers(int fd) {
    std::string out;
    char buf[512];
    while (out.find("\r\n\r\n") == std::string::npos && out.size() < 16384) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      out.append(buf, static_cast<usize>(n));
    }
    return out;
  }

  int listen_loopback(int backlog = 64) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    int yes = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(::listen(fd, backlog) == 0);
    return fd;
  }

  int socket_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    return ntohs(addr.sin_port);
  }

  class one_shot_http_server {
  public:
    using handler_fn = std::function<std::string(const std::string&)>;

    explicit one_shot_http_server(handler_fn handler) : handler_(std::move(handler)) {
      fd_ = listen_loopback(1);
      port_ = socket_port(fd_);
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

  class threaded_http_server {
  public:
    using handler_fn = std::function<void(int, const std::string&)>;

    explicit threaded_http_server(handler_fn handler) : handler_(std::move(handler)) {
      fd_ = listen_loopback(128);
      port_ = socket_port(fd_);
      accept_thread_ = std::thread([this] { run(); });
    }

    ~threaded_http_server() {
      stop_.store(true);
      if (fd_ >= 0)
        ::shutdown(fd_, SHUT_RDWR);
      if (fd_ >= 0)
        ::close(fd_);
      if (accept_thread_.joinable())
        accept_thread_.join();
      std::vector<std::thread> workers;
      {
        std::lock_guard<std::mutex> lock(workers_mu_);
        workers.swap(workers_);
      }
      for (auto& worker : workers) {
        if (worker.joinable())
          worker.join();
      }
    }

    int port() const {
      return port_;
    }

  private:
    void run() {
      while (!stop_.load()) {
        int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0)
          break;
        std::lock_guard<std::mutex> lock(workers_mu_);
        workers_.emplace_back([this, client] {
          std::string req = recv_headers(client);
          handler_(client, req);
          ::shutdown(client, SHUT_RDWR);
          ::close(client);
        });
      }
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic_bool stop_{false};
    handler_fn handler_;
    std::thread accept_thread_;
    std::mutex workers_mu_;
    std::vector<std::thread> workers_;
  };

  fxe::net::http_response
  request_sync(fxe::net::http_request req,
               std::chrono::milliseconds max_wait = std::chrono::seconds(5)) {
    fxe::net::http_response out;
    bool done = false;
    fxe::net::http_client::instance().submit(std::move(req), [&](fxe::net::http_response resp) {
      out = std::move(resp);
      done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + max_wait;
    while (!done && std::chrono::steady_clock::now() < deadline) {
      fxe::net::http_client::instance().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done);
    return out;
  }

  void pump_until(const std::function<bool()>& done, std::chrono::milliseconds max_wait) {
    const auto deadline = std::chrono::steady_clock::now() + max_wait;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      fxe::net::http_client::instance().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done());
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

  void test_abort_error_paths() {
    if (!fxe::net::http_client::available()) {
      std::puts("SKIP abort paths: libcurl unavailable");
      return;
    }
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    threaded_http_server server([](int client, const std::string&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      send_all(client, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nlate");
    });

    fxe::net::http_response resp;
    int calls = 0;
    fxe::net::http_request req;
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/abort";
    auto id =
        fxe::net::http_client::instance().submit(std::move(req), [&](fxe::net::http_response r) {
          resp = std::move(r);
          ++calls;
        });
    fxe::net::http_client::instance().poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    fxe::net::http_client::instance().abort(id);
    pump_until([&] { return calls == 1; }, std::chrono::seconds(1));
    CHECK(calls == 1);
    CHECK(resp.last_error == fxe::net::http_error::abort);
  }

  void test_timeout() {
    if (!fxe::net::http_client::available()) {
      std::puts("SKIP timeout: libcurl unavailable");
      return;
    }
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    threaded_http_server server([](int, const std::string&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });
    fxe::net::http_request req;
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/timeout";
    req.timeout_ms = 100;
    const auto start = std::chrono::steady_clock::now();
    auto resp = request_sync(std::move(req), std::chrono::seconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(resp.last_error == fxe::net::http_error::timeout);
    CHECK(elapsed < std::chrono::milliseconds(900));
  }

  void test_per_origin_concurrency_cap() {
    if (!fxe::net::http_client::available()) {
      std::puts("SKIP concurrency cap: libcurl unavailable");
      return;
    }
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    threaded_http_server server([&](int client, const std::string&) {
      const int now = active.fetch_add(1) + 1;
      int observed = max_active.load();
      while (now > observed && !max_active.compare_exchange_weak(observed, now)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      send_all(client, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
      active.fetch_sub(1);
    });

    constexpr int kRequests = 12;
    std::atomic<int> done{0};
    std::atomic<int> success{0};
    for (int i = 0; i < kRequests; ++i) {
      fxe::net::http_request req;
      req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/cap/" + std::to_string(i);
      fxe::net::http_client::instance().submit(std::move(req), [&](fxe::net::http_response resp) {
        if (resp.error.empty() && resp.status == 200)
          ++success;
        ++done;
      });
    }
    pump_until([&] { return done.load() == kRequests; }, std::chrono::seconds(5));
    CHECK(success.load() == kRequests);
    CHECK(max_active.load() <= 6);
  }

  void test_queue_overflow_and_abort_cleanup() {
    if (!fxe::net::http_client::available()) {
      std::puts("SKIP queue overflow: libcurl unavailable");
      return;
    }
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    threaded_http_server server([](int client, const std::string&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      send_all(client, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    });

    constexpr int kTotal = 270;
    std::vector<fxe::net::http_request_id> ids;
    ids.reserve(kTotal);
    std::atomic<int> callbacks{0};
    std::atomic<int> overflow{0};
    std::atomic<int> aborts{0};
    for (int i = 0; i < kTotal; ++i) {
      fxe::net::http_request req;
      req.url =
          "http://127.0.0.1:" + std::to_string(server.port()) + "/overflow/" + std::to_string(i);
      ids.push_back(fxe::net::http_client::instance().submit(
          std::move(req), [&](fxe::net::http_response resp) {
            if (resp.error.find("request queue full") != std::string::npos)
              ++overflow;
            if (resp.last_error == fxe::net::http_error::abort)
              ++aborts;
            ++callbacks;
          }));
    }
    CHECK(overflow.load() > 0);
    for (auto id : ids)
      fxe::net::http_client::instance().abort(id);
    pump_until([&] { return callbacks.load() == kTotal; }, std::chrono::seconds(2));
    CHECK(callbacks.load() == kTotal);
    CHECK(aborts.load() + overflow.load() == kTotal);
  }
} // namespace

int main() {
  test_cookie_round_trip();
  test_local_proxy();
  test_abort_error_paths();
  test_timeout();
  test_per_origin_concurrency_cap();
  test_queue_overflow_and_abort_cleanup();
  std::printf("fxe_net_http_advanced_tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
