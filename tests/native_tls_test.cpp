#include "../src/net/tls_client.hpp"
#include "../src/net/tls_server.hpp"

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>

#include <cstdio>
#include <cstring>
#include <fxe/types.hpp>
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

  struct round_trip_result {
    bool ok = false;
    std::string err;
    std::string client_alpn;
    std::string server_alpn;
    std::string client_received;
    std::string server_received;
    std::string peer_subject;
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
} // namespace

int main() {
  auto cert = make_self_signed_certificate();
  if (!cert.ok)
    std::fprintf(stderr, "certificate generation error: %s\n", cert.err.c_str());
  CHECK(cert.ok);
  if (cert.ok) {
    test_verified_round_trip(cert);
    test_reject_unauthorized_false(cert);
  }

  std::fprintf(stderr, "native_tls_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
