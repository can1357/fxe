#include "tls_server.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fxe/types.hpp>
#include <memory>
#include <string_view>
#include <utility>

namespace fxe::net {

  namespace {
    std::string tls_error(std::string_view operation, int ret) {
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

    bool seed_rng(mbedtls_entropy_context& entropy, mbedtls_ctr_drbg_context& ctr_drbg,
                  const char* personalization, std::string& err) {
      const auto* pers = reinterpret_cast<const unsigned char*>(personalization);
      int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, pers,
                                      std::strlen(personalization));
      if (ret != 0) {
        err = tls_error("mbedtls_ctr_drbg_seed", ret);
        return false;
      }
      return true;
    }

    int parse_private_key(mbedtls_pk_context& key, const std::string& key_pem,
                          mbedtls_ctr_drbg_context& ctr_drbg) {
#if MBEDTLS_VERSION_MAJOR >= 3
      return mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(key_pem.data()),
                                  key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random,
                                  &ctr_drbg);
#else
      (void)ctr_drbg;
      return mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(key_pem.data()),
                                  key_pem.size() + 1, nullptr, 0);
#endif
    }

    bool configure_alpn(mbedtls_ssl_config& conf, const std::vector<std::string>& requested,
                        std::vector<std::string>& storage, std::vector<const char*>& protocol_ptrs,
                        std::string& err) {
      storage = requested;
      protocol_ptrs.clear();
      protocol_ptrs.reserve(storage.size() + 1);
      for (const auto& protocol : storage) {
        if (protocol.empty()) {
          err = "TLS ALPN protocol entries must not be empty";
          return false;
        }
        protocol_ptrs.push_back(protocol.c_str());
      }
      protocol_ptrs.push_back(nullptr);
      if (protocol_ptrs.size() == 1)
        return true;

      int ret = mbedtls_ssl_conf_alpn_protocols(&conf, protocol_ptrs.data());
      if (ret != 0) {
        err = tls_error("mbedtls_ssl_conf_alpn_protocols", ret);
        return false;
      }
      return true;
    }

    int handshake(mbedtls_ssl_context& ssl) {
      int ret = 0;
      do {
        ret = mbedtls_ssl_handshake(&ssl);
      } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
      return ret;
    }

    std::string cert_subject(const mbedtls_x509_crt& cert, std::string& err) {
      char subject[512] = {};
      const int ret = mbedtls_x509_dn_gets(subject, sizeof(subject), &cert.subject);
      if (ret < 0) {
        err = tls_error("mbedtls_x509_dn_gets", ret);
        return {};
      }
      return std::string(subject, static_cast<usize>(ret));
    }

    std::string lowercase_fingerprint(std::string_view value) {
      std::string out;
      out.reserve(value.size());
      for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == ':' || c == '-' || c == ' ' || c == '\t')
          continue;
        out.push_back(static_cast<char>(std::tolower(c)));
      }
      return out;
    }

    std::string sha256_fingerprint(const mbedtls_x509_crt& cert, std::string& err) {
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      if (!md) {
        err = "mbedtls_md_info_from_type failed: SHA-256 unavailable";
        return {};
      }

      unsigned char digest[32] = {};
      const int ret = mbedtls_md(md, cert.raw.p, cert.raw.len, digest);
      if (ret != 0) {
        err = tls_error("mbedtls_md", ret);
        return {};
      }

      static constexpr char k_hex[] = "0123456789abcdef";
      std::string out;
      out.reserve(sizeof(digest) * 2);
      for (unsigned char byte : digest) {
        out.push_back(k_hex[byte >> 4]);
        out.push_back(k_hex[byte & 0x0f]);
      }
      return out;
    }

    bool cert_matches_expected(const mbedtls_x509_crt& cert, std::string_view expected,
                               std::string& err) {
      const std::string subject = cert_subject(cert, err);
      if (!err.empty())
        return false;
      if (subject == expected)
        return true;

      const std::string fingerprint = sha256_fingerprint(cert, err);
      if (!err.empty())
        return false;
      return !fingerprint.empty() && fingerprint == lowercase_fingerprint(expected);
    }

    void warn_once(std::atomic<bool>& warned, const char* message) {
      bool expected = false;
      if (warned.compare_exchange_strong(expected, true))
        std::fprintf(stderr, "%s\n", message);
    }

    struct server_state {
      server_state() {
        mbedtls_x509_crt_init(&cert);
        mbedtls_pk_init(&key);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
      }

      ~server_state() {
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&cert);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
      }

      mbedtls_x509_crt cert{};
      mbedtls_pk_context key{};
      mbedtls_entropy_context entropy{};
      mbedtls_ctr_drbg_context ctr_drbg{};
      std::vector<std::string> alpn;
      bool request_client_cert = false;
      std::string expected_client_cert;
      std::atomic<bool> warned_unverified_client_cert{false};
    };

    class accepted_tls_client final : public tls_client {
    public:
      accepted_tls_client(std::shared_ptr<server_state> state, mbedtls_net_context accepted)
          : state_(std::move(state)) {
        mbedtls_net_init(&net_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&ctr_drbg_);
        net_ = accepted;
      }

      ~accepted_tls_client() override {
        close();
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ctr_drbg_free(&ctr_drbg_);
        mbedtls_entropy_free(&entropy_);
      }

      bool finish_handshake(std::string& err) {
        last_error_.clear();
        if (!seed_rng(entropy_, ctr_drbg_, "fxe_tls_server_connection", err)) {
          last_error_ = err;
          return false;
        }

        int ret =
            mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_ssl_config_defaults", ret));

        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctr_drbg_);
        mbedtls_ssl_conf_authmode(&conf_, state_->request_client_cert ? MBEDTLS_SSL_VERIFY_OPTIONAL
                                                                      : MBEDTLS_SSL_VERIFY_NONE);

        ret = mbedtls_ssl_conf_own_cert(&conf_, &state_->cert, &state_->key);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_ssl_conf_own_cert", ret));

        if (!configure_alpn(conf_, state_->alpn, alpn_storage_, alpn_ptrs_, err)) {
          last_error_ = err;
          return false;
        }

        ret = mbedtls_ssl_setup(&ssl_, &conf_);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_ssl_setup", ret));

        mbedtls_ssl_set_bio(&ssl_, &net_, mbedtls_net_send, mbedtls_net_recv, nullptr);
        ret = handshake(ssl_);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_ssl_handshake", ret));

        if (state_->request_client_cert)
          return verify_requested_client_cert(err);
        return true;
      }

      ssize_t read(void* buf, usize cap) override {
        last_error_.clear();
        if (!buf || cap == 0)
          return 0;
        int ret = 0;
        do {
          ret = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(buf), cap);
        } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
          return 0;
        if (ret < 0)
          last_error_ = tls_error("mbedtls_ssl_read", ret);
        return static_cast<ssize_t>(ret);
      }
      ssize_t read_with_timeout(void* buf, usize cap, int timeout_ms) override {
        last_error_.clear();
        if (!buf || cap == 0)
          return 0;
        if (timeout_ms <= 0)
          return read(buf, cap);
        const int poll =
            mbedtls_net_poll(&net_, MBEDTLS_NET_POLL_READ, static_cast<uint32_t>(timeout_ms));
        if (poll == 0)
          return read_timed_out;
        if (poll < 0) {
          last_error_ = tls_error("mbedtls_net_poll", poll);
          return -1;
        }
        if (mbedtls_net_set_nonblock(&net_) != 0) {
          last_error_ = "mbedtls_net_set_nonblock failed";
          return -1;
        }
        const int ret = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(buf), cap);
        (void)mbedtls_net_set_block(&net_);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
          return read_timed_out;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
          return 0;
        if (ret < 0) {
          last_error_ = tls_error("mbedtls_ssl_read", ret);
          return -1;
        }
        return static_cast<ssize_t>(ret);
      }

      ssize_t write(const void* buf, usize len) override {
        last_error_.clear();
        if (!buf || len == 0)
          return 0;
        int ret = 0;
        do {
          ret = mbedtls_ssl_write(&ssl_, static_cast<const unsigned char*>(buf), len);
        } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (ret < 0)
          last_error_ = tls_error("mbedtls_ssl_write", ret);
        return static_cast<ssize_t>(ret);
      }

      std::string negotiated_alpn() const override {
        last_error_.clear();
        const char* protocol = mbedtls_ssl_get_alpn_protocol(&ssl_);
        return protocol ? std::string(protocol) : std::string();
      }

      std::optional<std::string> peer_cert_subject() const override {
        last_error_.clear();
        const mbedtls_x509_crt* cert = mbedtls_ssl_get_peer_cert(&ssl_);
        if (!cert)
          return std::nullopt;
        std::string err;
        auto subject = cert_subject(*cert, err);
        if (!err.empty()) {
          last_error_ = std::move(err);
          return std::nullopt;
        }
        return subject;
      }

      std::string last_error() const override {
        return last_error_;
      }

      void close() override {
        if (closed_)
          return;
        closed_ = true;
        last_error_.clear();
        if (net_.fd >= 0)
          (void)mbedtls_ssl_close_notify(&ssl_);
        mbedtls_net_free(&net_);
      }

    private:
      bool fail(std::string& err, std::string message) {
        last_error_ = std::move(message);
        err = last_error_;
        return false;
      }

      bool verify_requested_client_cert(std::string& err) {
        const mbedtls_x509_crt* cert = mbedtls_ssl_get_peer_cert(&ssl_);
        if (state_->expected_client_cert.empty()) {
          warn_once(state_->warned_unverified_client_cert,
                    "fxe.net.tls_server: request_client_cert accepts client certificates without "
                    "verification; call verify_client_cert() to enforce subject or fingerprint");
          return true;
        }
        if (!cert)
          return fail(err, "TLS client certificate verification failed: no peer certificate");

        std::string verify_err;
        if (!cert_matches_expected(*cert, state_->expected_client_cert, verify_err)) {
          if (verify_err.empty())
            verify_err = "TLS client certificate verification failed: subject/fingerprint mismatch";
          return fail(err, std::move(verify_err));
        }
        return true;
      }

      std::shared_ptr<server_state> state_;
      mbedtls_net_context net_{};
      mbedtls_ssl_context ssl_{};
      mbedtls_ssl_config conf_{};
      mbedtls_entropy_context entropy_{};
      mbedtls_ctr_drbg_context ctr_drbg_{};
      std::vector<std::string> alpn_storage_;
      std::vector<const char*> alpn_ptrs_;
      bool closed_ = false;
      mutable std::string last_error_;
    };

    class tls_server_impl final : public tls_server {
    public:
      tls_server_impl() {
        mbedtls_net_init(&listen_);
      }

      ~tls_server_impl() override {
        close();
      }

      bool listen_on(const tls_server_options& opts, std::string& err) {
        last_error_.clear();
        if (opts.cert_pem.empty())
          return fail(err, "TLS server certificate PEM must not be empty");
        if (opts.key_pem.empty())
          return fail(err, "TLS server private key PEM must not be empty");

        state_ = std::make_shared<server_state>();
        state_->alpn = opts.alpn;
        state_->request_client_cert = opts.request_client_cert;

        if (!seed_rng(state_->entropy, state_->ctr_drbg, "fxe_tls_server", err)) {
          last_error_ = err;
          return false;
        }

        int ret = mbedtls_x509_crt_parse(
            &state_->cert, reinterpret_cast<const unsigned char*>(opts.cert_pem.data()),
            opts.cert_pem.size() + 1);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_x509_crt_parse", ret));

        ret = parse_private_key(state_->key, opts.key_pem, state_->ctr_drbg);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_pk_parse_key", ret));

        mbedtls_ssl_config probe_conf;
        mbedtls_ssl_config_init(&probe_conf);
        ret = mbedtls_ssl_config_defaults(&probe_conf, MBEDTLS_SSL_IS_SERVER,
                                          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0)
          ret = mbedtls_ssl_conf_own_cert(&probe_conf, &state_->cert, &state_->key);
        mbedtls_ssl_config_free(&probe_conf);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_ssl_conf_own_cert", ret));

        const std::string port = std::to_string(opts.port);
        ret = mbedtls_net_bind(&listen_, nullptr, port.c_str(), MBEDTLS_NET_PROTO_TCP);
        if (ret != 0)
          return fail(err, tls_error("mbedtls_net_bind", ret));
        closed_.store(false);
        return true;
      }

      std::unique_ptr<tls_client> accept(std::string& err) override {
        err.clear();
        last_error_.clear();
        mbedtls_net_context accepted;
        mbedtls_net_init(&accepted);
        int ret = mbedtls_net_accept(&listen_, &accepted, nullptr, 0, nullptr);
        if (ret != 0) {
          mbedtls_net_free(&accepted);
          if (!closed_.load())
            fail(err, tls_error("mbedtls_net_accept", ret));
          return nullptr;
        }

        auto client = std::make_unique<accepted_tls_client>(state_, accepted);
        accepted.fd = -1;
        if (!client->finish_handshake(err)) {
          last_error_ = err.empty() ? client->last_error() : err;
          return nullptr;
        }
        return client;
      }

      u16 local_port() const override {
        last_error_.clear();
        if (listen_.fd < 0)
          return 0;

        sockaddr_storage addr{};
#if defined(_WIN32)
        int len = static_cast<int>(sizeof(addr));
        if (::getsockname(static_cast<SOCKET>(listen_.fd), reinterpret_cast<sockaddr*>(&addr),
                          &len) != 0)
          return 0;
#else
        socklen_t len = static_cast<socklen_t>(sizeof(addr));
        if (::getsockname(listen_.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
          return 0;
#endif

        if (addr.ss_family == AF_INET) {
          const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
          return ntohs(in->sin_port);
        }
        if (addr.ss_family == AF_INET6) {
          const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
          return ntohs(in6->sin6_port);
        }
        return 0;
      }

      bool verify_client_cert(const std::string& expected_subject_or_fingerprint) override {
        last_error_.clear();
        if (!state_)
          return fail(last_error_, "TLS server is not listening");
        state_->expected_client_cert = expected_subject_or_fingerprint;
        state_->warned_unverified_client_cert.store(false);
        return true;
      }

      std::string last_error() const override {
        return last_error_;
      }
      void close() override {
        bool was_closed = closed_.exchange(true);
        if (!was_closed) {
          last_error_.clear();
          mbedtls_net_free(&listen_);
        }
      }

    private:
      bool fail(std::string& err, std::string message) const {
        last_error_ = std::move(message);
        err = last_error_;
        return false;
      }

      std::shared_ptr<server_state> state_;
      mbedtls_net_context listen_{};
      std::atomic<bool> closed_{true};
      mutable std::string last_error_;
    };
  } // namespace

  tls_server::~tls_server() = default;

  std::unique_ptr<tls_server> tls_server::listen(const tls_server_options& opts, std::string& err) {
    err.clear();
    auto server = std::make_unique<tls_server_impl>();
    if (!server->listen_on(opts, err))
      return nullptr;
    return server;
  }

} // namespace fxe::net
