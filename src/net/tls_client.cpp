#include "tls_client.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include <cstring>
#include <string_view>

#include <fxe/types.hpp>
#include <list>
#include <memory>
#include <mutex>
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

    struct tls_session_deleter {
      void operator()(mbedtls_ssl_session* session) const {
        if (!session)
          return;
        mbedtls_ssl_session_free(session);
        delete session;
      }
    };

    using tls_session_ptr = std::unique_ptr<mbedtls_ssl_session, tls_session_deleter>;

    tls_session_ptr make_tls_session() {
      tls_session_ptr session(new mbedtls_ssl_session());
      mbedtls_ssl_session_init(session.get());
      return session;
    }

    struct cached_session {
      std::string key;
      tls_session_ptr session;
    };

    std::mutex& session_cache_mu() {
      static std::mutex mu;
      return mu;
    }

    std::list<cached_session>& session_cache() {
      static std::list<cached_session> cache;
      return cache;
    }

    std::string session_cache_key(const tls_options& opts) {
      return opts.host + ":" + std::to_string(opts.port);
    }

    void erase_cached_session_locked(const std::string& key) {
      auto& cache = session_cache();
      for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->key == key) {
          cache.erase(it);
          return;
        }
      }
    }

    void apply_cached_session(mbedtls_ssl_context& ssl, const std::string& key) {
      std::lock_guard<std::mutex> lk(session_cache_mu());
      auto& cache = session_cache();
      for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->key != key)
          continue;
        int ret = mbedtls_ssl_set_session(&ssl, it->session.get());
        if (ret == 0) {
          cache.splice(cache.begin(), cache, it);
        } else {
          cache.erase(it);
        }
        return;
      }
    }

    void store_cached_session(const mbedtls_ssl_context& ssl, const std::string& key) {
      auto session = make_tls_session();
      if (mbedtls_ssl_get_session(&ssl, session.get()) != 0)
        return;

      std::lock_guard<std::mutex> lk(session_cache_mu());
      auto& cache = session_cache();
      erase_cached_session_locked(key);
      cache.push_front(cached_session{key, std::move(session)});
      while (cache.size() > 128)
        cache.pop_back();
    }

    void configure_ocsp_stapling(mbedtls_ssl_config& conf, const tls_options& opts) {
      (void)conf;
      (void)opts;
      // Mbed TLS 2.x and the 3.x public headers used by vcpkg expose certificate
      // verification through mbedtls_ssl_conf_authmode(), but do not expose a
      // stable client API to request or retrieve stapled OCSP responses. Keep
      // normal certificate verification enabled and skip stapling when unavailable.
    }

    bool load_system_roots(mbedtls_x509_crt& ca) {
#if defined(MBEDTLS_FS_IO)
      static constexpr const char* k_files[] = {
          "/etc/ssl/cert.pem",
          "/etc/ssl/certs/ca-certificates.crt",
          "/etc/pki/tls/certs/ca-bundle.crt",
          "/etc/ssl/ca-bundle.pem",
          "/usr/local/etc/openssl/cert.pem",
          "/opt/homebrew/etc/openssl@3/cert.pem",
          "/opt/homebrew/etc/openssl/cert.pem",
      };
      for (const char* file : k_files) {
        (void)mbedtls_x509_crt_parse_file(&ca, file);
        if (ca.version != 0)
          return true;
      }

      static constexpr const char* k_paths[] = {
          "/etc/ssl/certs",
          "/etc/pki/tls/certs",
          "/usr/local/share/certs",
          "/usr/share/ca-certificates",
      };
      for (const char* path : k_paths) {
        (void)mbedtls_x509_crt_parse_path(&ca, path);
        if (ca.version != 0)
          return true;
      }
#else
      (void)ca;
#endif
      return false;
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

    class tls_client_impl final : public tls_client {
    public:
      tls_client_impl() {
        mbedtls_net_init(&net_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_x509_crt_init(&client_cert_);
        mbedtls_pk_init(&client_key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&ctr_drbg_);
      }

      ~tls_client_impl() override {
        close();
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_x509_crt_free(&client_cert_);
        mbedtls_pk_free(&client_key_);
        mbedtls_ctr_drbg_free(&ctr_drbg_);
        mbedtls_entropy_free(&entropy_);
      }

      bool connect_to(const tls_options& opts, std::string& err) {
        if (opts.host.empty()) {
          err = "TLS host must not be empty";
          return false;
        }
        if (!seed_rng(entropy_, ctr_drbg_, "fxe_tls_client", err))
          return false;

        int ret =
            mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
          err = tls_error("mbedtls_ssl_config_defaults", ret);
          return false;
        }

        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctr_drbg_);
        mbedtls_ssl_conf_authmode(&conf_, opts.reject_unauthorized ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                                   : MBEDTLS_SSL_VERIFY_NONE);
        configure_ocsp_stapling(conf_, opts);

        if (!opts.ca_pem.empty()) {
          ret = mbedtls_x509_crt_parse(&ca_,
                                       reinterpret_cast<const unsigned char*>(opts.ca_pem.data()),
                                       opts.ca_pem.size() + 1);
          if (ret != 0) {
            err = tls_error("mbedtls_x509_crt_parse", ret);
            return false;
          }
        } else {
          (void)load_system_roots(ca_);
        }
        if (ca_.version != 0)
          mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);

        if (opts.client_cert_pem.empty() != opts.client_key_pem.empty()) {
          err = "TLS client_cert_pem and client_key_pem must be provided together";
          return false;
        }
        if (!opts.client_cert_pem.empty()) {
          ret = mbedtls_x509_crt_parse(
              &client_cert_, reinterpret_cast<const unsigned char*>(opts.client_cert_pem.data()),
              opts.client_cert_pem.size() + 1);
          if (ret != 0) {
            err = tls_error("mbedtls_x509_crt_parse client_cert_pem", ret);
            return false;
          }
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
          ret = mbedtls_pk_parse_key(
              &client_key_, reinterpret_cast<const unsigned char*>(opts.client_key_pem.data()),
              opts.client_key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &ctr_drbg_);
#else
          ret = mbedtls_pk_parse_key(
              &client_key_, reinterpret_cast<const unsigned char*>(opts.client_key_pem.data()),
              opts.client_key_pem.size() + 1, nullptr, 0);
#endif
          if (ret != 0) {
            err = tls_error("mbedtls_pk_parse_key client_key_pem", ret);
            return false;
          }
          ret = mbedtls_ssl_conf_own_cert(&conf_, &client_cert_, &client_key_);
          if (ret != 0) {
            err = tls_error("mbedtls_ssl_conf_own_cert client certificate", ret);
            return false;
          }
        }

        if (!configure_alpn(conf_, opts.alpn, alpn_storage_, alpn_ptrs_, err))
          return false;

        ret = mbedtls_ssl_setup(&ssl_, &conf_);
        if (ret != 0) {
          err = tls_error("mbedtls_ssl_setup", ret);
          return false;
        }

        ret = mbedtls_ssl_set_hostname(&ssl_, opts.host.c_str());
        if (ret != 0) {
          err = tls_error("mbedtls_ssl_set_hostname", ret);
          return false;
        }

        const std::string cache_key = session_cache_key(opts);
        if (opts.enable_session_resumption)
          apply_cached_session(ssl_, cache_key);
        const std::string port = std::to_string(opts.port);
        ret = mbedtls_net_connect(&net_, opts.host.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
          err = tls_error("mbedtls_net_connect", ret);
          return false;
        }
        mbedtls_ssl_set_bio(&ssl_, &net_, mbedtls_net_send, mbedtls_net_recv, nullptr);

        ret = handshake(ssl_);
        if (ret != 0) {
          if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char verify_info[512] = {};
            mbedtls_x509_crt_verify_info(verify_info, sizeof(verify_info), "",
                                         mbedtls_ssl_get_verify_result(&ssl_));
            err = std::string("TLS certificate verification failed: ") + verify_info;
          } else {
            err = tls_error("mbedtls_ssl_handshake", ret);
          }
          return false;
        }

        if (opts.enable_session_resumption)
          store_cached_session(ssl_, cache_key);
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
        char subject[512] = {};
        int ret = mbedtls_x509_dn_gets(subject, sizeof(subject), &cert->subject);
        if (ret < 0) {
          last_error_ = tls_error("mbedtls_x509_dn_gets", ret);
          return std::nullopt;
        }
        return std::string(subject, static_cast<usize>(ret));
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
      mbedtls_net_context net_{};
      mbedtls_ssl_context ssl_{};
      mbedtls_ssl_config conf_{};
      mbedtls_x509_crt ca_{};
      mbedtls_x509_crt client_cert_{};
      mbedtls_pk_context client_key_{};
      mbedtls_entropy_context entropy_{};
      mbedtls_ctr_drbg_context ctr_drbg_{};
      std::vector<std::string> alpn_storage_;
      std::vector<const char*> alpn_ptrs_;
      bool closed_ = false;
      mutable std::string last_error_;
    };
  } // namespace

  tls_client::~tls_client() = default;

  std::unique_ptr<tls_client> tls_client::connect(const tls_options& opts, std::string& err) {
    err.clear();
    auto client = std::make_unique<tls_client_impl>();
    if (!client->connect_to(opts, err))
      return nullptr;
    return client;
  }

} // namespace fxe::net
