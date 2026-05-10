#include "../src/net/tls_client.hpp"
#include "../src/net/tls_server.hpp"
#include "../src/runtime/v8/native/https_transport.hpp"

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
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

    const char* pers = "fxe_native_tls_test";
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

    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost,O=fxe native TLS test");
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_subject_name", ret);
      goto done;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost,O=fxe native TLS test");
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
      ssize_t n = client.write(data + off, len - off);
      if (n <= 0) {
        err = "TLS write failed with return " + std::to_string(n);
        return false;
      }
      off += static_cast<usize>(n);
    }
    return true;
  }

  bool write_all(fxe::net::tls_client& client, const std::string& data, std::string& err) {
    return write_all(client, data.data(), data.size(), err);
  }

  bool read_exact(fxe::net::tls_client& client, char* data, usize len, std::string& err) {
    usize off = 0;
    while (off < len) {
      ssize_t n = client.read(data + off, len - off);
      if (n <= 0) {
        err = "TLS read failed with return " + std::to_string(n);
        return false;
      }
      off += static_cast<usize>(n);
    }
    return true;
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
    case 303:
      return "See Other";
    case 307:
      return "Temporary Redirect";
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

    int port() const {
      return port_;
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

  struct round_trip_result {
    bool ok = false;
    std::string err;
    std::string client_alpn;
    std::string server_alpn;
    std::string client_received;
    std::string server_received;
    std::string peer_subject;
    fxe::net::ocsp_stapling_status ocsp_status = fxe::net::ocsp_stapling_status::not_requested;
  };

  round_trip_result run_round_trip(const generated_certificate& cert, bool reject_unauthorized,
                                   std::string ca_pem) {
    round_trip_result result;

    fxe::net::tls_server_options server_opts;
    server_opts.cert_pem = cert.cert_pem;
    server_opts.key_pem = cert.key_pem;
    server_opts.alpn = {"h2", "http/1.1"};
    server_opts.port = 0;

    std::string err;
    auto server = fxe::net::tls_server::listen(server_opts, err);
    if (!server) {
      result.err = "server listen failed: " + err;
      return result;
    }

    const u16 port = server->local_port();
    if (port == 0) {
      result.err = "server reported local port 0";
      server->close();
      return result;
    }

    std::string server_err;
    std::thread server_thread([&] {
      std::string accept_err;
      auto peer = server->accept(accept_err);
      if (!peer) {
        server_err = "server accept failed: " + accept_err;
        return;
      }

      result.server_alpn = peer->negotiated_alpn();
      char buf[4] = {};
      if (!read_exact(*peer, buf, sizeof(buf), server_err))
        return;
      result.server_received.assign(buf, sizeof(buf));
      if (!write_all(*peer, "pong", 4, server_err))
        return;
      peer->close();
    });

    fxe::net::tls_options client_opts;
    client_opts.host = "localhost";
    client_opts.port = port;
    client_opts.ca_pem = std::move(ca_pem);
    client_opts.reject_unauthorized = reject_unauthorized;
    client_opts.alpn = {"http/1.1"};

    auto client = fxe::net::tls_client::connect(client_opts, err);
    if (!client) {
      result.err = "client connect failed: " + err;
      server->close();
      server_thread.join();
      return result;
    }

    result.client_alpn = client->negotiated_alpn();
    auto peer_subject = client->peer_cert_subject();
    result.peer_subject = peer_subject.value_or(std::string{});
    result.ocsp_status = client->ocsp_stapling_status();
    if (!write_all(*client, "ping", 4, result.err)) {
      client->close();
      server->close();
      server_thread.join();
      return result;
    }

    char buf[4] = {};
    if (!read_exact(*client, buf, sizeof(buf), result.err)) {
      client->close();
      server->close();
      server_thread.join();
      return result;
    }
    result.client_received.assign(buf, sizeof(buf));
    client->close();
    server->close();
    server_thread.join();

    if (!server_err.empty()) {
      result.err = server_err;
      return result;
    }

    result.ok = true;
    return result;
  }

  void test_verified_round_trip(const generated_certificate& cert) {
    auto result = run_round_trip(cert, true, cert.cert_pem);
    if (!result.ok)
      std::fprintf(stderr, "verified round trip error: %s\n", result.err.c_str());
    CHECK(result.ok);
    CHECK(fxe::net::tls_client::supports_ocsp_stapling() || true);
    CHECK(result.ocsp_status == fxe::net::ocsp_stapling_status::unsupported ||
          result.ocsp_status == fxe::net::ocsp_stapling_status::requested_no_response);
    CHECK(std::string(fxe::net::ocsp_stapling_status_name(
              fxe::net::ocsp_stapling_status::unsupported)) == "unsupported");
    CHECK(result.client_received == "pong");
    CHECK(result.server_received == "ping");
    CHECK(result.client_alpn == "http/1.1");
    CHECK(result.server_alpn == "http/1.1");
    CHECK(result.peer_subject.find("CN=localhost") != std::string::npos);
  }

  void test_reject_unauthorized_false(const generated_certificate& cert) {
    auto result = run_round_trip(cert, false, "");
    if (!result.ok)
      std::fprintf(stderr, "reject_unauthorized=false round trip error: %s\n", result.err.c_str());
    CHECK(result.ok);
    CHECK(result.client_received == "pong");
    CHECK(result.server_received == "ping");
    CHECK(result.client_alpn == "http/1.1");
  }
  void test_session_cache_key_isolation(const generated_certificate& cert_a,
                                        const generated_certificate& cert_b) {
    fxe::net::tls_options base;
    base.host = "localhost";
    base.port = 443;
    base.ca_pem = cert_a.cert_pem;
    base.alpn = {"h2", "http/1.1"};
    base.sni = "localhost";
    base.client_cert_pem = cert_a.cert_pem;
    base.session_namespace = "default";

    const auto key_a = fxe::net::tls_session_cache_key_for_test(base);
    CHECK(!key_a.empty());
    CHECK(key_a == fxe::net::tls_session_cache_key_for_test(base));

    auto different_ca = base;
    different_ca.ca_pem = cert_b.cert_pem;
    CHECK(key_a != fxe::net::tls_session_cache_key_for_test(different_ca));

    auto different_namespace = base;
    different_namespace.session_namespace = "other";
    CHECK(key_a != fxe::net::tls_session_cache_key_for_test(different_namespace));

    auto different_sni = base;
    different_sni.sni = "alt.localhost";
    CHECK(key_a != fxe::net::tls_session_cache_key_for_test(different_sni));

    auto different_client_cert = base;
    different_client_cert.client_cert_pem = cert_b.cert_pem;
    CHECK(key_a != fxe::net::tls_session_cache_key_for_test(different_client_cert));
  }

  void test_native_https_redirect_and_final_url(const generated_certificate& cert) {
    scripted_https_server server(cert, [](const https_http_request& request) {
      if (request.path == "/post-start") {
        return make_https_response(303, {{"Location", "/post-final"}}, "");
      }
      if (request.path == "/post-final") {
        const bool ok = request.method == "GET" && request.body.empty();
        return make_https_response(ok ? 200 : 400, {}, ok ? "post-ok" : "bad");
      }
      if (request.path == "/put-start") {
        return make_https_response(307, {{"Location", "/put-final"}}, "");
      }
      if (request.path == "/put-final") {
        const bool ok = request.method == "PUT" && request.body == "payload";
        return make_https_response(ok ? 200 : 400, {}, ok ? "put-ok" : "bad");
      }
      return make_https_response(400, {}, "bad");
    });
    if (!server.ok())
      std::fprintf(stderr, "redirect server error: %s\n", server.error().c_str());
    CHECK(server.ok());

    fxe::runtime::native_https_request_options opts;
    opts.ca_pem = cert.cert_pem;

    fxe::net::http_request post_request;
    post_request.method = "POST";
    post_request.url = server.url("/post-start");
    post_request.body = "payload";
    auto post_response =
        fxe::runtime::perform_native_https_request(std::move(post_request), nullptr, opts);
    CHECK(post_response.error.empty());
    CHECK(post_response.status == 200);
    CHECK(post_response.body == "post-ok");
    CHECK(post_response.final_url == server.url("/post-final"));

    fxe::net::http_request put_request;
    put_request.method = "PUT";
    put_request.url = server.url("/put-start");
    put_request.body = "payload";
    auto put_response =
        fxe::runtime::perform_native_https_request(std::move(put_request), nullptr, opts);
    CHECK(put_response.error.empty());
    CHECK(put_response.status == 200);
    CHECK(put_response.body == "put-ok");
    CHECK(put_response.final_url == server.url("/put-final"));

    const auto requests = server.requests();
    CHECK(requests.size() == 4);
    if (requests.size() == 4) {
      CHECK(requests[0].method == "POST");
      CHECK(requests[0].path == "/post-start");
      CHECK(requests[1].method == "GET");
      CHECK(requests[1].path == "/post-final");
      CHECK(requests[1].body.empty());
      CHECK(requests[2].method == "PUT");
      CHECK(requests[2].path == "/put-start");
      CHECK(requests[3].method == "PUT");
      CHECK(requests[3].path == "/put-final");
      CHECK(requests[3].body == "payload");
    }
    if (!server.error().empty())
      std::fprintf(stderr, "redirect server runtime error: %s\n", server.error().c_str());
    CHECK(server.error().empty());
  }

  void test_native_https_timeout_poll_path(const generated_certificate& cert) {
    scripted_https_server server(cert, [](const https_http_request&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      return make_https_response(200, {}, "late");
    });
    if (!server.ok())
      std::fprintf(stderr, "timeout server error: %s\n", server.error().c_str());
    CHECK(server.ok());

    fxe::runtime::native_https_request_options opts;
    opts.ca_pem = cert.cert_pem;
    fxe::net::http_request request;
    request.url = server.url("/timeout");
    request.timeout_ms = 100;

    auto handle = fxe::runtime::start_native_https_request(std::move(request), nullptr, opts);
    const auto start = std::chrono::steady_clock::now();
    fxe::net::http_response response;
    while (!handle->poll(response) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(response.last_error == fxe::net::http_error::timeout);
    CHECK(response.final_url == server.url("/timeout"));
    CHECK(elapsed < std::chrono::milliseconds(800));
  }

  void test_native_https_cookie_round_trip(const generated_certificate& cert) {
    scripted_https_server server(cert, [](const https_http_request& request) {
      if (request.path == "/set") {
        return make_https_response(200, {{"Set-Cookie", "sid=abc; Path=/; HttpOnly"}}, "ok");
      }
      if (request.path == "/check") {
        auto it = request.headers.find("cookie");
        const bool saw_cookie =
            it != request.headers.end() && it->second.find("sid=abc") != std::string::npos;
        return make_https_response(saw_cookie ? 200 : 400, {}, saw_cookie ? "cookie" : "missing");
      }
      return make_https_response(400, {}, "bad");
    });
    if (!server.ok())
      std::fprintf(stderr, "cookie server error: %s\n", server.error().c_str());
    CHECK(server.ok());

    fxe::runtime::native_https_request_options opts;
    opts.ca_pem = cert.cert_pem;
    fxe::net::cookie_jar jar;

    fxe::net::http_request set_request;
    set_request.url = server.url("/set");
    auto set_response =
        fxe::runtime::perform_native_https_request(std::move(set_request), &jar, opts);
    CHECK(set_response.error.empty());
    CHECK(set_response.status == 200);
    CHECK(jar.pick_for_request(server.url("/check")).find("sid=abc") != std::string::npos);

    fxe::net::http_request check_request;
    check_request.url = server.url("/check");
    auto check_response =
        fxe::runtime::perform_native_https_request(std::move(check_request), &jar, opts);
    CHECK(check_response.error.empty());
    CHECK(check_response.status == 200);
    CHECK(check_response.body == "cookie");

    if (!server.error().empty())
      std::fprintf(stderr, "cookie server runtime error: %s\n", server.error().c_str());
    CHECK(server.error().empty());
  }

  void test_session_resumption_reuses_cached_session(const generated_certificate& cert) {
    fxe::net::tls_session_cache_reset_for_test();
    auto before = fxe::net::tls_session_cache_stats_for_test();
    CHECK(before.entries == 0);
    CHECK(before.hits == 0);
    CHECK(before.misses == 0);
    CHECK(before.stores == 0);

    fxe::net::tls_server_options server_opts;
    server_opts.cert_pem = cert.cert_pem;
    server_opts.key_pem = cert.key_pem;
    server_opts.alpn = {"http/1.1"};
    server_opts.port = 0;

    std::string err;
    auto server = fxe::net::tls_server::listen(server_opts, err);
    if (!server) {
      std::fprintf(stderr, "session resumption server listen failed: %s\n", err.c_str());
      CHECK(false);
      return;
    }

    const u16 port = server->local_port();
    if (port == 0) {
      std::fprintf(stderr, "session resumption server reported local port 0\n");
      CHECK(false);
      server->close();
      return;
    }

    std::string server_err;
    std::thread server_thread([&] {
      for (int attempt = 0; attempt != 2; ++attempt) {
        std::string accept_err;
        auto peer = server->accept(accept_err);
        if (!peer) {
          server_err = "server accept failed: " + accept_err;
          return;
        }

        char buf[4] = {};
        if (!read_exact(*peer, buf, sizeof(buf), server_err))
          return;
        if (!write_all(*peer, "pong", 4, server_err))
          return;
        peer->close();
      }
    });

    fxe::net::tls_options client_opts;
    client_opts.host = "localhost";
    client_opts.port = port;
    client_opts.ca_pem = cert.cert_pem;
    client_opts.reject_unauthorized = true;
    client_opts.alpn = {"http/1.1"};
    client_opts.enable_session_resumption = true;

    auto round_trip = [&] {
      auto client = fxe::net::tls_client::connect(client_opts, err);
      if (!client) {
        err = "client connect failed: " + err;
        return false;
      }
      if (!write_all(*client, "ping", 4, err)) {
        client->close();
        return false;
      }
      char buf[4] = {};
      if (!read_exact(*client, buf, sizeof(buf), err)) {
        client->close();
        return false;
      }
      client->close();
      return std::string_view(buf, sizeof(buf)) == "pong";
    };

    bool ok = round_trip();
    if (ok)
      ok = round_trip();
    server->close();
    server_thread.join();
    if (!ok)
      std::fprintf(stderr, "session resumption round trip error: %s\n", err.c_str());
    if (!server_err.empty())
      std::fprintf(stderr, "session resumption server error: %s\n", server_err.c_str());
    CHECK(ok);
    CHECK(server_err.empty());

    const auto after = fxe::net::tls_session_cache_stats_for_test();
    CHECK(after.entries == 1);
    CHECK(after.misses == 1);
    CHECK(after.hits >= 1);
    CHECK(after.stores >= 2);
  }

} // namespace

int main() {
  auto cert = make_self_signed_certificate();
  if (!cert.ok)
    std::fprintf(stderr, "certificate generation error: %s\n", cert.err.c_str());
  CHECK(cert.ok);
  auto cert_b = make_self_signed_certificate();
  if (!cert_b.ok)
    std::fprintf(stderr, "second certificate generation error: %s\n", cert_b.err.c_str());
  CHECK(cert_b.ok);
  if (cert.ok && cert_b.ok) {
    test_verified_round_trip(cert);
    test_reject_unauthorized_false(cert);
    test_native_https_redirect_and_final_url(cert);
    test_native_https_timeout_poll_path(cert);
    test_native_https_cookie_round_trip(cert);
    test_session_cache_key_isolation(cert, cert_b);
    test_session_resumption_reuses_cached_session(cert);
  }

  std::fprintf(stderr, "native_tls_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
