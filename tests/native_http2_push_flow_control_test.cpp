#include "../src/net/http2_client.hpp"
#include "../src/net/http2_server.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

    const char* pers = "fxe_native_http2_push_flow_control_test";
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

    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost,O=fxe native HTTP/2 test");
    if (ret != 0) {
      out.err = mbedtls_error("mbedtls_x509write_crt_set_subject_name", ret);
      goto done;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost,O=fxe native HTTP/2 test");
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

  std::string make_body(std::size_t bytes) {
    std::string out(bytes, '\0');
    for (std::size_t i = 0; i < bytes; ++i)
      out[i] = static_cast<char>('a' + (i % 26));
    return out;
  }

  class http2_test_server {
  public:
    using handler_type =
        std::function<void(fxe::net::http2_server&, const fxe::net::http2_incoming_request&)>;

    http2_test_server(const generated_certificate& cert, fxe::net::http2_settings settings,
                      handler_type handler)
        : handler_(std::move(handler)) {
      fxe::net::http2_server_options options;
      options.cert_pem = cert.cert_pem;
      options.key_pem = cert.key_pem;
      options.port = 0;
      options.settings = std::move(settings);
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
      return server_ != nullptr && error().empty();
    }

    u16 port() const {
      return server_ ? server_->local_port() : 0;
    }

    std::string error() const {
      std::lock_guard<std::mutex> lock(error_mutex_);
      return error_;
    }

  private:
    void loop() {
      while (!stop_.load()) {
        std::string err;
        auto request = server_->poll(err);
        if (!err.empty()) {
          set_error(err);
          break;
        }
        if (!request) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }
        handler_(*server_, *request);
      }
    }

    void set_error(std::string err) {
      std::lock_guard<std::mutex> lock(error_mutex_);
      if (error_.empty())
        error_ = std::move(err);
    }

    std::unique_ptr<fxe::net::http2_server> server_;
    handler_type handler_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
  };

  std::unique_ptr<fxe::net::http2_client> connect_client(const generated_certificate& cert,
                                                         u16 port,
                                                         const fxe::net::http2_settings& settings) {
    std::string err;
    auto client =
        fxe::net::http2_client::connect("localhost", port, cert.cert_pem, true, settings, err);
    if (!client)
      std::fprintf(stderr, "client connect error: %s\n", err.c_str());
    CHECK(client != nullptr);
    return client;
  }

  void test_push_promise_round_trip(const generated_certificate& cert) {
    std::string push_err;
    http2_test_server server(
        cert, {},
        [&](fxe::net::http2_server& http2, const fxe::net::http2_incoming_request& request) {
          fxe::net::http2_request promised_request;
          promised_request.method = "GET";
          promised_request.path = "/push.css";
          promised_request.headers.emplace_back(":scheme", "https");
          promised_request.headers.emplace_back(":authority",
                                                "localhost:" + std::to_string(http2.local_port()));

          fxe::net::http2_response promised_response;
          promised_response.status = 200;
          promised_response.body = "body bytes";

          push_err.clear();
          const auto pushed_stream_id = http2.submit_push_promise(
              request.stream_id, promised_request, promised_response, push_err);
          CHECK(pushed_stream_id > 0);
          CHECK(push_err.empty());

          std::string respond_err;
          fxe::net::http2_response response;
          response.status = 200;
          response.body = "parent body";
          CHECK(http2.respond(request.id, response, respond_err));
          CHECK(respond_err.empty());
        });
    CHECK(server.ok());
    auto client = connect_client(cert, server.port(), {});
    if (!client)
      return;

    std::mutex push_mutex;
    std::condition_variable push_cv;
    std::optional<fxe::net::http2_pushed> pushed;
    client->set_push_handler([&](fxe::net::http2_pushed value) {
      std::lock_guard<std::mutex> lock(push_mutex);
      pushed = std::move(value);
      push_cv.notify_all();
    });

    fxe::net::http2_request request;
    request.method = "GET";
    request.path = "/index.html";
    std::string submit_err;
    const auto stream_id = client->submit(request);
    CHECK(stream_id > 0);
    CHECK(client->last_error().empty());

    std::string wait_err;
    const auto response = client->wait(stream_id, wait_err);
    CHECK(wait_err.empty());
    CHECK(response.status == 200);
    CHECK(response.body == "parent body");

    std::unique_lock<std::mutex> lock(push_mutex);
    CHECK(push_cv.wait_for(lock, std::chrono::seconds(2), [&] { return pushed.has_value(); }));
    CHECK(pushed.has_value());
    if (pushed) {
      CHECK(pushed->associated_stream_id == stream_id);
      CHECK(pushed->promised_request.path == "/push.css");
      CHECK(pushed->response.body == "body bytes");
    }
    CHECK(server.error().empty());
  }

  void test_push_refused_when_settings_disable(const generated_certificate& cert) {
    std::mutex result_mutex;
    std::condition_variable result_cv;
    int pushed_stream_id = 0;
    std::string pushed_err;
    bool recorded = false;

    http2_test_server server(
        cert, {},
        [&](fxe::net::http2_server& http2, const fxe::net::http2_incoming_request& request) {
          fxe::net::http2_request promised_request;
          promised_request.method = "GET";
          promised_request.path = "/push.css";
          promised_request.headers.emplace_back(":scheme", "https");
          promised_request.headers.emplace_back(":authority",
                                                "localhost:" + std::to_string(http2.local_port()));

          fxe::net::http2_response promised_response;
          promised_response.status = 200;
          promised_response.body = "body bytes";

          std::string err;
          const auto stream = http2.submit_push_promise(request.stream_id, promised_request,
                                                        promised_response, err);
          {
            std::lock_guard<std::mutex> lock(result_mutex);
            pushed_stream_id = stream;
            pushed_err = std::move(err);
            recorded = true;
          }
          result_cv.notify_all();

          std::string respond_err;
          fxe::net::http2_response response;
          response.status = 200;
          response.body = "parent body";
          CHECK(http2.respond(request.id, response, respond_err));
          CHECK(respond_err.empty());
        });
    CHECK(server.ok());

    fxe::net::http2_settings settings;
    settings.enable_push = 0;
    auto client = connect_client(cert, server.port(), settings);
    if (!client)
      return;

    fxe::net::http2_request request;
    request.method = "GET";
    request.path = "/index.html";
    std::string wait_err;
    const auto stream_id = client->submit(request);
    CHECK(stream_id > 0);
    const auto response = client->wait(stream_id, wait_err);
    CHECK(wait_err.empty());
    CHECK(response.status == 200);

    std::unique_lock<std::mutex> lock(result_mutex);
    CHECK(result_cv.wait_for(lock, std::chrono::seconds(2), [&] { return recorded; }));
    CHECK(pushed_stream_id == -1);
    CHECK(pushed_err == "peer disabled push");
    CHECK(server.error().empty());
  }

  void test_flow_control_consume_advances_window(const generated_certificate& cert) {
    constexpr std::size_t kBodySize = 1024 * 1024;
    const std::string body = make_body(kBodySize);
    http2_test_server server(
        cert, {},
        [&](fxe::net::http2_server& http2, const fxe::net::http2_incoming_request& request) {
          std::string respond_err;
          fxe::net::http2_response response;
          response.status = 200;
          response.body = body;
          CHECK(http2.respond(request.id, response, respond_err));
          CHECK(respond_err.empty());
        });
    CHECK(server.ok());
    auto client = connect_client(cert, server.port(), {});
    if (!client)
      return;

    std::mutex data_mutex;
    std::size_t total = 0;
    std::size_t count = 0;
    client->set_on_data_consumed([&](i32, std::size_t chunk_size) {
      std::lock_guard<std::mutex> lock(data_mutex);
      total += chunk_size;
      ++count;
    });

    fxe::net::http2_request request;
    request.method = "GET";
    request.path = "/flow";
    std::string wait_err;
    const auto stream_id = client->submit(request);
    CHECK(stream_id > 0);
    const auto response = client->wait(stream_id, wait_err);
    CHECK(wait_err.empty());
    CHECK(response.status == 200);
    CHECK(response.body.size() == body.size());
    CHECK(response.body == body);
    {
      std::lock_guard<std::mutex> lock(data_mutex);
      CHECK(total == body.size());
      // nghttp2 frames the body in chunks bounded by MAX_FRAME_SIZE; the
      // exact split is implementation-defined, so just assert we received
      // at least one consume callback.
      CHECK(count > 0);
    }
    CHECK(server.error().empty());
  }

  void test_stream_window_resize(const generated_certificate& cert) {
    constexpr std::size_t kBodySize = 2 * 1024 * 1024;
    const std::string body = make_body(kBodySize);
    http2_test_server server(
        cert, {},
        [&](fxe::net::http2_server& http2, const fxe::net::http2_incoming_request& request) {
          std::string respond_err;
          fxe::net::http2_response response;
          response.status = 200;
          response.body = body;
          CHECK(http2.respond(request.id, response, respond_err));
          CHECK(respond_err.empty());
        });
    CHECK(server.ok());

    fxe::net::http2_settings settings;
    settings.stream_max_recv_window_size = 1024 * 1024;
    auto client = connect_client(cert, server.port(), settings);
    if (!client)
      return;

    fxe::net::http2_request request;
    request.method = "GET";
    request.path = "/window";
    request.timeout_ms = 5000;
    const auto start = std::chrono::steady_clock::now();
    std::string wait_err;
    const auto stream_id = client->submit(request);
    CHECK(stream_id > 0);
    const auto response = client->wait(stream_id, wait_err);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(wait_err.empty());
    CHECK(response.status == 200);
    CHECK(response.body.size() == body.size());
    CHECK(elapsed < std::chrono::seconds(5));
    CHECK(server.error().empty());
  }
} // namespace

int main() {
  const auto cert = make_self_signed_certificate();
  CHECK(cert.ok);
  if (!cert.ok) {
    std::fprintf(stderr, "%s\n", cert.err.c_str());
    return 1;
  }

  test_push_promise_round_trip(cert);
  test_push_refused_when_settings_disable(cert);
  test_flow_control_consume_advances_window(cert);
  test_stream_window_resize(cert);

  if (g_fail != 0)
    std::fprintf(stderr, "FAILED: %d checks failed, %d passed\n", g_fail, g_pass);
  else
    std::printf("ok: %d checks passed\n", g_pass);
  return g_fail == 0 ? 0 : 1;
}
