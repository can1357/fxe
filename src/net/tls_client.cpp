#include "tls_client.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

#include <fxe/log.hpp>
#include <fxe/types.hpp>
#include <list>
#include <memory>
#include <mutex>
// <mbedtls/ssl.h> in the vcpkg-shipped 2.x/3.x builds exposes the RFC 6066
// status_request extension identifier, but not a public client API to request
// or retrieve stapled OCSP responses. Keep the gate at 0 until mbedTLS ships a
// real hook we can probe here.
#ifdef __has_include
#if defined(MBEDTLS_SSL_PROTO_TLS1_2) && __has_include(<mbedtls/ssl.h>) && \
    defined(MBEDTLS_TLS_EXT_STATUS_REQUEST) && 0
#define FXE_TLS_OCSP_STAPLING_SUPPORTED 1
#else
#define FXE_TLS_OCSP_STAPLING_SUPPORTED 0
#endif
#else
#define FXE_TLS_OCSP_STAPLING_SUPPORTED 0
#endif

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

    std::string read_file_to_string(const std::string& path, std::string& err) {
      std::ifstream f(path, std::ios::binary);
      if (!f) {
        err = "failed to open " + path;
        return {};
      }
      std::stringstream ss;
      ss << f.rdbuf();
      return ss.str();
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

    struct tls_session_identity {
      std::string host;
      u16 port = 443;
      std::vector<std::string> alpn;
      std::string sni;
      std::string ca_fingerprint;
      std::string client_cert_fingerprint;
      bool reject_unauthorized = true;
      std::string session_namespace;
    };

    struct cached_session {
      std::string key;
      std::vector<unsigned char> serialized;
      std::chrono::steady_clock::time_point expires_at;
    };

    constexpr auto k_default_session_ttl = std::chrono::seconds(300);
    constexpr usize k_max_cached_sessions = 64;

    std::mutex& session_cache_mu() {
      static std::mutex mu;
      return mu;
    }

    std::list<cached_session>& session_cache() {
      static std::list<cached_session> cache;
      return cache;
    }
    tls_session_cache_stats& session_cache_stats() {
      static tls_session_cache_stats stats;
      return stats;
    }

    std::vector<unsigned char> save_session_blob(const mbedtls_ssl_session& session,
                                                 std::string& err) {
      usize cap = 2048;
      for (int attempt = 0; attempt != 6; ++attempt) {
        std::vector<unsigned char> blob(cap);
        size_t used = 0;
        const int ret = mbedtls_ssl_session_save(&session, blob.data(), blob.size(), &used);
        if (ret == 0) {
          blob.resize(used);
          return blob;
        }
        if (ret != MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL) {
          err = tls_error("mbedtls_ssl_session_save", ret);
          return {};
        }
        cap = std::max<usize>(cap * 2, used);
      }
      err = "mbedtls_ssl_session_save failed: serialized session exceeded 65536 bytes";
      return {};
    }

    std::string sha256_hex(std::string_view data) {
      std::array<unsigned char, 32> digest{};
      const auto* bytes =
          data.empty() ? nullptr : reinterpret_cast<const unsigned char*>(data.data());
      (void)mbedtls_sha256(bytes, data.size(), digest.data(), 0);
      static constexpr char k_hex[] = "0123456789abcdef";
      std::string out;
      out.resize(digest.size() * 2);
      for (usize i = 0; i < digest.size(); ++i) {
        out[i * 2] = k_hex[digest[i] >> 4];
        out[i * 2 + 1] = k_hex[digest[i] & 0x0f];
      }
      return out;
    }

    std::string encode_session_key_field(char tag, std::string_view value) {
      std::string out;
      out.reserve(1 + 20 + 1 + value.size());
      out.push_back(tag);
      out += std::to_string(value.size());
      out.push_back(':');
      out.append(value.data(), value.size());
      return out;
    }

    std::string encode_session_key_field(std::string_view tag, std::string_view value) {
      std::string out;
      out.reserve(tag.size() + 20 + 1 + value.size());
      out.append(tag.data(), tag.size());
      out += std::to_string(value.size());
      out.push_back(':');
      out.append(value.data(), value.size());
      return out;
    }

    tls_session_identity make_session_identity(const tls_options& opts,
                                               std::string_view effective_ca,
                                               std::string_view effective_client_cert) {
      tls_session_identity identity;
      identity.host = opts.host;
      identity.port = opts.port;
      identity.alpn = opts.alpn;
      identity.sni = opts.sni.empty() ? opts.host : opts.sni;
      identity.ca_fingerprint = effective_ca.empty() ? std::string{} : sha256_hex(effective_ca);
      identity.client_cert_fingerprint =
          effective_client_cert.empty() ? std::string{} : sha256_hex(effective_client_cert);
      identity.reject_unauthorized = opts.reject_unauthorized;
      identity.session_namespace = opts.session_namespace;
      return identity;
    }

    std::string session_cache_key(const tls_session_identity& identity) {
      std::string alpn_joined;
      for (usize i = 0; i < identity.alpn.size(); ++i) {
        if (i != 0)
          alpn_joined.push_back(',');
        alpn_joined += identity.alpn[i];
      }
      std::string key;
      key += encode_session_key_field('H', identity.host);
      key += "|";
      key += encode_session_key_field('P', std::to_string(identity.port));
      key += "|";
      key += encode_session_key_field('R', identity.reject_unauthorized ? "1" : "0");
      key += "|";
      key += encode_session_key_field('N', identity.session_namespace);
      key += "|";
      key += encode_session_key_field('S', identity.sni);
      key += "|";
      key += encode_session_key_field('A', alpn_joined);
      key += "|";
      key += encode_session_key_field("CA", identity.ca_fingerprint);
      key += "|";
      key += encode_session_key_field("CC", identity.client_cert_fingerprint);
      return key;
    }

    void erase_cached_session_locked(const std::string& key) {
      auto& cache = session_cache();
      for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->key == key) {
          cache.erase(it);
          ++session_cache_stats().evictions;
          return;
        }
      }
    }

    void apply_cached_session(mbedtls_ssl_context& ssl, const std::string& key) {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lk(session_cache_mu());
      auto& cache = session_cache();
      auto& stats = session_cache_stats();
      for (auto it = cache.begin(); it != cache.end();) {
        if (it->expires_at <= now) {
          it = cache.erase(it);
          ++stats.evictions;
          continue;
        }
        if (it->key != key) {
          ++it;
          continue;
        }
        auto session = make_tls_session();
        int ret =
            mbedtls_ssl_session_load(session.get(), it->serialized.data(), it->serialized.size());
        if (ret != 0) {
          FXE_WARN("net.tls", "session_cache_drop key={} op=load err={}", key,
                   tls_error("mbedtls_ssl_session_load", ret));
          it = cache.erase(it);
          ++stats.evictions;
          return;
        }
        ret = mbedtls_ssl_set_session(&ssl, session.get());
        if (ret == 0) {
          ++stats.hits;
          FXE_DEBUG("net.tls", "session_cache_hit key={} bytes={}", key, it->serialized.size());
          cache.splice(cache.begin(), cache, it);
        } else {
          FXE_WARN("net.tls", "session_cache_drop key={} op=set err={}", key,
                   tls_error("mbedtls_ssl_set_session", ret));
          cache.erase(it);
          ++stats.evictions;
        }
        return;
      }
      ++stats.misses;
      FXE_DEBUG("net.tls", "session_cache_miss key={}", key);
    }

    void store_cached_session(const mbedtls_ssl_context& ssl, const std::string& key,
                              std::chrono::seconds ttl) {
      auto session = make_tls_session();
      const int get_ret = mbedtls_ssl_get_session(&ssl, session.get());
      if (get_ret != 0) {
        FXE_WARN("net.tls", "session_cache_skip_store key={} err={}", key,
                 tls_error("mbedtls_ssl_get_session", get_ret));
        return;
      }

      std::string err;
      auto blob = save_session_blob(*session, err);
      if (!err.empty()) {
        FXE_WARN("net.tls", "session_cache_skip_store key={} err={}", key, err);
        return;
      }

      const auto effective_ttl = ttl.count() == 0 ? k_default_session_ttl : ttl;
      std::lock_guard<std::mutex> lk(session_cache_mu());
      auto& cache = session_cache();
      auto& stats = session_cache_stats();
      erase_cached_session_locked(key);
      cache.push_front(cached_session{
          key,
          std::move(blob),
          std::chrono::steady_clock::now() + effective_ttl,
      });
      ++stats.stores;
      while (cache.size() > k_max_cached_sessions) {
        cache.pop_back();
        ++stats.evictions;
      }
      FXE_DEBUG("net.tls", "session_cache_store key={} entries={} ttl_s={}", key, cache.size(),
                effective_ttl.count());
    }

    void configure_ocsp_stapling(mbedtls_ssl_config& conf, const tls_options& opts,
                                 ocsp_stapling_status& status) {
      (void)conf;
      if (!opts.request_ocsp_stapling)
        return;

#if FXE_TLS_OCSP_STAPLING_SUPPORTED
      status = ocsp_stapling_status::requested_no_response;
#else
      FXE_DEBUG("net.tls", "ocsp_stapling_unavailable host={} reason=no_public_client_api",
                opts.host);
      status = ocsp_stapling_status::unsupported;
#endif
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
        status_ = ocsp_stapling_status::not_requested;
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
        configure_ocsp_stapling(conf_, opts, status_);

        std::string effective_ca = opts.ca_pem;
        if (!opts.ca_path.empty()) {
          effective_ca = read_file_to_string(opts.ca_path, err);
          if (!err.empty())
            return false;
        }
        if (!effective_ca.empty()) {
          ret = mbedtls_x509_crt_parse(&ca_,
                                       reinterpret_cast<const unsigned char*>(effective_ca.data()),
                                       effective_ca.size() + 1);
          if (ret != 0) {
            err = tls_error("mbedtls_x509_crt_parse", ret);
            return false;
          }
        } else {
          (void)load_system_roots(ca_);
        }
        if (ca_.version != 0)
          mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);

        std::string effective_client_cert = opts.client_cert_pem;
        if (!opts.client_cert_path.empty()) {
          effective_client_cert = read_file_to_string(opts.client_cert_path, err);
          if (!err.empty())
            return false;
        }
        std::string effective_client_key = opts.client_key_pem;
        if (!opts.client_key_path.empty()) {
          effective_client_key = read_file_to_string(opts.client_key_path, err);
          if (!err.empty())
            return false;
        }
        if (effective_client_cert.empty() != effective_client_key.empty()) {
          err = "TLS client_cert_pem/client_key_pem (or *_path) must be provided together";
          return false;
        }
        if (!effective_client_cert.empty()) {
          ret = mbedtls_x509_crt_parse(
              &client_cert_, reinterpret_cast<const unsigned char*>(effective_client_cert.data()),
              effective_client_cert.size() + 1);
          if (ret != 0) {
            err = tls_error("mbedtls_x509_crt_parse client certificate", ret);
            return false;
          }
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
          ret = mbedtls_pk_parse_key(
              &client_key_, reinterpret_cast<const unsigned char*>(effective_client_key.data()),
              effective_client_key.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &ctr_drbg_);
#else
          ret = mbedtls_pk_parse_key(
              &client_key_, reinterpret_cast<const unsigned char*>(effective_client_key.data()),
              effective_client_key.size() + 1, nullptr, 0);
#endif
          if (ret != 0) {
            err = tls_error("mbedtls_pk_parse_key client key", ret);
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

        const std::string effective_sni = opts.sni.empty() ? opts.host : opts.sni;
        ret = mbedtls_ssl_set_hostname(&ssl_, effective_sni.c_str());
        if (ret != 0) {
          err = tls_error("mbedtls_ssl_set_hostname", ret);
          return false;
        }

        const std::string cache_key =
            session_cache_key(make_session_identity(opts, effective_ca, effective_client_cert));
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
          store_cached_session(ssl_, cache_key, opts.session_ttl);
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
        char subject[512] = {};
        int ret = mbedtls_x509_dn_gets(subject, sizeof(subject), &cert->subject);
        if (ret < 0) {
          last_error_ = tls_error("mbedtls_x509_dn_gets", ret);
          return std::nullopt;
        }
        return std::string(subject, static_cast<usize>(ret));
      }

      ::fxe::net::ocsp_stapling_status ocsp_stapling_status() const noexcept override {
        return status_;
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
      ::fxe::net::ocsp_stapling_status status_ = ::fxe::net::ocsp_stapling_status::not_requested;
    };
  } // namespace

  const char* ocsp_stapling_status_name(ocsp_stapling_status status) noexcept {
    switch (status) {
    case ocsp_stapling_status::unsupported:
      return "unsupported";
    case ocsp_stapling_status::not_requested:
      return "not_requested";
    case ocsp_stapling_status::requested_no_response:
      return "requested_no_response";
    case ocsp_stapling_status::stapled_valid:
      return "stapled_valid";
    case ocsp_stapling_status::stapled_invalid:
      return "stapled_invalid";
    }
    return "unknown";
  }

  tls_client::~tls_client() = default;

  bool tls_client::supports_ocsp_stapling() noexcept {
    return FXE_TLS_OCSP_STAPLING_SUPPORTED != 0;
  }
  void tls_session_cache_reset_for_test() noexcept {
    std::lock_guard<std::mutex> lk(session_cache_mu());
    session_cache().clear();
    session_cache_stats() = {};
  }

  tls_session_cache_stats tls_session_cache_stats_for_test() noexcept {
    std::lock_guard<std::mutex> lk(session_cache_mu());
    auto stats = session_cache_stats();
    stats.entries = session_cache().size();
    return stats;
  }

  std::string tls_session_cache_key_for_test(const tls_options& opts) {
    std::string err;
    std::string effective_ca = opts.ca_pem;
    if (!opts.ca_path.empty()) {
      effective_ca = read_file_to_string(opts.ca_path, err);
      if (!err.empty())
        return {};
    }
    std::string effective_client_cert = opts.client_cert_pem;
    if (!opts.client_cert_path.empty()) {
      effective_client_cert = read_file_to_string(opts.client_cert_path, err);
      if (!err.empty())
        return {};
    }
    return session_cache_key(make_session_identity(opts, effective_ca, effective_client_cert));
  }

  std::unique_ptr<tls_client> tls_client::connect(const tls_options& opts, std::string& err) {
    err.clear();
    auto client = std::make_unique<tls_client_impl>();
    if (!client->connect_to(opts, err))
      return nullptr;
    return client;
  }

} // namespace fxe::net
