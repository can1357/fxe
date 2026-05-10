#include "net/http_client.hpp"
#include "net/tls_server.hpp"
#include "runtime/v8/native/https_transport.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fxe/types.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

  std::string ascii_lower_copy(std::string value) {
    for (char& c : value)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
  }

  std::string trim_copy(std::string_view value) {
    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && is_ws(static_cast<unsigned char>(value.front())))
      value.remove_prefix(1);
    while (!value.empty() && is_ws(static_cast<unsigned char>(value.back())))
      value.remove_suffix(1);
    return std::string(value);
  }

  std::string mbedtls_error(const char* operation, int ret) {
    char detail[256] = {};
    mbedtls_strerror(ret, detail, sizeof(detail));
    std::string out(operation);
    out += " failed: ";
    out += detail[0] != '\0' ? detail : "unknown mbedTLS error";
    out += " (";
    out += std::to_string(ret);
    out += ")";
    return out;
  }

  struct generated_certificate {
    bool ok = false;
    std::string cert_pem;
    std::string key_pem;
    std::string err;
  };

  generated_certificate make_self_signed_certificate() {
    generated_certificate out;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);

    const char* pers = "fxe_native_https_test";
    int ret =
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              reinterpret_cast<const unsigned char*>(pers), std::strlen(pers));
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_ctr_drbg_seed", ret);
      goto done;
    }

    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_pk_setup", ret);
      goto done;
    }

    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &ctr_drbg, 2048, 65537);
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_rsa_gen_key", ret);
      goto done;
    }

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    ret = mbedtls_mpi_lset(&serial, 1);
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_mpi_lset", ret);
      goto done;
    }
    ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_serial", ret);
      goto done;
    }

    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost,O=fxe native HTTPS test");
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_subject_name", ret);
      goto done;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost,O=fxe native HTTPS test");
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_issuer_name", ret);
      goto done;
    }

    ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000");
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_validity", ret);
      goto done;
    }

    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1);
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_basic_constraints", ret);
      goto done;
    }

    ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                        MBEDTLS_X509_KU_KEY_ENCIPHERMENT |
                                                        MBEDTLS_X509_KU_KEY_CERT_SIGN);
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_key_usage", ret);
      goto done;
    }

    {
      std::vector<unsigned char> cert_buf(8192);
      ret = mbedtls_x509write_crt_pem(&crt, cert_buf.data(), cert_buf.size(),
                                      mbedtls_ctr_drbg_random, &ctr_drbg);
      if (ret != 0) {
        out.err = mbedtls_error("mbedtls_x509write_crt_pem", ret);
        goto done;
      }
      out.cert_pem.assign(reinterpret_cast<const char*>(cert_buf.data()));
    }

    {
      std::vector<unsigned char> key_buf(8192);
      ret = mbedtls_pk_write_key_pem(&key, key_buf.data(), key_buf.size());
      if (ret != 0) {
        out.err = mbedtls_error("mbedtls_pk_write_key_pem", ret);
        goto done;
      }
      out.key_pem.assign(reinterpret_cast<const char*>(key_buf.data()));
    }

    out.ok = true;

  done:
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return out;
  }

  bool write_all(fxe::net::tls_client& client, const char* data, usize len, std::string& err) {
    usize off = 0;
    while (off < len) {
      const ssize_t n = client.write(data + off, len - off);
      if (n <= 0) {
        err = client.last_error().empty() ? "TLS write failed" : client.last_error();
        return false;
      }
      off += static_cast<usize>(n);
    }
    return true;
  }

  bool write_all(fxe::net::tls_client& client, const std::string& data, std::string& err) {
    return write_all(client, data.data(), data.size(), err);
  }

  std::optional<usize>
  parse_content_length_header(const std::map<std::string, std::string>& headers) {
    auto it = headers.find("content-length");
    if (it == headers.end())
      return 0;
    usize value = 0;
    auto [ptr, ec] =
        std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    if (ec != std::errc{} || ptr != it->second.data() + it->second.size())
      return std::nullopt;
    return value;
  }

  struct https_http_request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
  };

  std::optional<https_http_request> read_https_http_request(fxe::net::tls_client& client,
                                                            std::string& error) {
    std::string buffer;
    std::array<char, 4096> chunk{};
    std::optional<usize> expected_total;
    for (;;) {
      const auto header_end = buffer.find("\r\n\r\n");
      if (expected_total && buffer.size() >= *expected_total)
        break;
      if (header_end != std::string::npos && !expected_total) {
        std::map<std::string, std::string> headers;
        std::string_view header_block(buffer.data(), header_end);
        usize line_start = header_block.find("\r\n");
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
          if (colon != std::string_view::npos) {
            headers[ascii_lower_copy(trim_copy(line.substr(0, colon)))] =
                trim_copy(line.substr(colon + 1));
          }
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
      buffer.append(chunk.data(), static_cast<usize>(n));
      if (buffer.size() > 8 * 1024 * 1024) {
        error = "HTTP request too large";
        return std::nullopt;
      }
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

    usize line_start = line_end == std::string_view::npos ? header_block.size() : line_end + 2;
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

  std::string reason_phrase(int status) {
    switch (status) {
    case 200:
      return "OK";
    case 302:
      return "Found";
    case 400:
      return "Bad Request";
    default:
      return "OK";
    }
  }

  std::string make_https_response(int status,
                                  const std::vector<std::pair<std::string, std::string>>& headers,
                                  std::string body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n";
    for (const auto& [key, value] : headers)
      out << key << ": " << value << "\r\n";
    out << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
    return out.str();
  }

  class scripted_https_server {
  public:
    using handler_fn = std::function<std::string(const https_http_request&)>;

    scripted_https_server(const generated_certificate& cert, handler_fn handler)
        : handler_(std::move(handler)) {
      fxe::net::tls_server_options server_opts;
      server_opts.cert_pem = cert.cert_pem;
      server_opts.key_pem = cert.key_pem;
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

    std::string url(std::string path) const {
      return "https://localhost:" + std::to_string(port_) + std::move(path);
    }

    std::vector<https_http_request> requests() const {
      std::lock_guard<std::mutex> lock(mu_);
      return requests_;
    }

    const std::string& error() const {
      return error_;
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

  fxe::runtime::native_https_request_options
  make_request_options(const generated_certificate& cert) {
    fxe::runtime::native_https_request_options opts;
    opts.ca_pem = cert.cert_pem;
    return opts;
  }

  void test_redirect_following_and_final_url(const generated_certificate& cert) {
    scripted_https_server server(cert, [](const https_http_request& request) {
      if (request.path == "/start") {
        return make_https_response(302, {{"Location", "finish?step=2"}}, "");
      }
      if (request.path == "/finish?step=2") {
        const bool ok = request.method == "GET" && request.body.empty();
        return make_https_response(ok ? 200 : 400, {}, ok ? "redirect-ok" : "bad");
      }
      return make_https_response(400, {}, "bad");
    });
    CHECK(server.ok());

    fxe::runtime::native_https_request_options opts = make_request_options(cert);
    fxe::net::cookie_jar jar;

    fxe::net::http_request request;
    request.method = "POST";
    request.url = server.url("/start");
    request.body = "payload";
    auto response =
        fxe::runtime::perform_native_https_request(std::move(request), &jar, std::move(opts));

    CHECK(response.error.empty());
    CHECK(response.status == 200);
    CHECK(response.body == "redirect-ok");
    CHECK(response.final_url == server.url("/finish?step=2"));

    const auto requests = server.requests();
    CHECK(requests.size() == 2);
    if (requests.size() == 2) {
      CHECK(requests[0].method == "POST");
      CHECK(requests[0].path == "/start");
      CHECK(requests[0].body == "payload");
      CHECK(requests[1].method == "GET");
      CHECK(requests[1].path == "/finish?step=2");
      CHECK(requests[1].body.empty());
    }
    CHECK(server.error().empty());
  }

  void test_timeout_and_poll_abort(const generated_certificate& cert) {
    scripted_https_server server(cert, [](const https_http_request&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      return make_https_response(200, {}, "late");
    });
    CHECK(server.ok());

    fxe::runtime::native_https_request_options opts = make_request_options(cert);
    fxe::net::http_request request;
    request.url = server.url("/timeout");
    request.timeout_ms = 100;
    const auto start = std::chrono::steady_clock::now();
    auto handle =
        fxe::runtime::start_native_https_request(std::move(request), nullptr, std::move(opts));
    fxe::net::http_response response;
    while (!handle->poll(response) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(response.last_error == fxe::net::http_error::timeout);
    CHECK(response.final_url == server.url("/timeout"));
    CHECK(elapsed < std::chrono::milliseconds(900));
  }

  void test_cookie_round_trip(const generated_certificate& cert) {
    scripted_https_server server(cert, [](const https_http_request& request) {
      if (request.path == "/set")
        return make_https_response(200, {{"Set-Cookie", "sid=abc; Path=/; HttpOnly"}}, "ok");
      if (request.path == "/check") {
        auto it = request.headers.find("cookie");
        const bool saw_cookie =
            it != request.headers.end() && it->second.find("sid=abc") != std::string::npos;
        return make_https_response(saw_cookie ? 200 : 400, {}, saw_cookie ? "cookie" : "missing");
      }
      return make_https_response(400, {}, "bad");
    });
    CHECK(server.ok());

    fxe::runtime::native_https_request_options opts = make_request_options(cert);
    fxe::net::cookie_jar jar;

    fxe::net::http_request set_request;
    set_request.url = server.url("/set");
    auto set_response = fxe::runtime::perform_native_https_request(set_request, &jar, opts);
    CHECK(set_response.error.empty());
    CHECK(set_response.status == 200);
    CHECK(jar.pick_for_request(server.url("/check")).find("sid=abc") != std::string::npos);

    fxe::net::http_request check_request;
    check_request.url = server.url("/check");
    auto check_response = fxe::runtime::perform_native_https_request(check_request, &jar, opts);
    CHECK(check_response.error.empty());
    CHECK(check_response.status == 200);
    CHECK(check_response.body == "cookie");
    CHECK(server.error().empty());
  }
} // namespace

int main() {
  auto cert = make_self_signed_certificate();
  if (!cert.ok)
    std::fprintf(stderr, "certificate generation error: %s\n", cert.err.c_str());
  CHECK(cert.ok);
  CHECK(fxe::net::http_client::available());
  if (cert.ok && fxe::net::http_client::available()) {
    test_redirect_following_and_final_url(cert);
    test_timeout_and_poll_abort(cert);
    test_cookie_round_trip(cert);
  }

  std::fprintf(stderr, "native_https_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
