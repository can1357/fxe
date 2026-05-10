#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fxe/types.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <BaseTsd.h>
using ssize_t = SSIZE_T;
#else
#include <sys/types.h>
#endif

namespace fxe::net {

  enum class ocsp_stapling_status {
    unsupported,
    not_requested,
    requested_no_response,
    stapled_valid,
    stapled_invalid,
  };

  const char* ocsp_stapling_status_name(ocsp_stapling_status status) noexcept;

  struct tls_options {
    std::string host;
    u16 port = 443;
    std::string ca_pem;
    std::string ca_path; // file path; takes precedence if non-empty
    bool reject_unauthorized = true;
    std::vector<std::string> alpn;
    std::string sni;               // optional override; if empty, use host
    std::string session_namespace; // Follow-up: plumb fetch/cookies/node-https isolation into this.
    std::chrono::seconds session_ttl{0}; // 0 = use default (300s)
    std::string client_cert_pem;
    std::string client_cert_path; // file path; takes precedence if non-empty
    std::string client_key_pem;
    std::string client_key_path; // file path; takes precedence if non-empty
    // Defaults to true, but callers must inspect tls_client::ocsp_stapling_status()
    // after connect because some mbedTLS builds do not expose client-side stapling.
    bool request_ocsp_stapling = true;
    bool enable_session_resumption = true;
  };

  // test-only: mirrors the internal TLS session cache identity derivation.
  std::string tls_session_cache_key_for_test(const tls_options& opts);
  class tls_client {
  public:
    static std::unique_ptr<tls_client> connect(const tls_options&, std::string& err);
    static bool supports_ocsp_stapling() noexcept;
    virtual ~tls_client();

    virtual ssize_t read(void* buf, usize cap) = 0;
    static constexpr ssize_t read_timed_out = -2;
    virtual ssize_t read_with_timeout(void* buf, usize cap, int timeout_ms) {
      (void)timeout_ms;
      return read(buf, cap);
    }
    virtual ssize_t write(const void* buf, usize len) = 0;
    virtual std::string negotiated_alpn() const = 0;
    virtual std::optional<std::string> peer_cert_subject() const = 0;
    virtual ::fxe::net::ocsp_stapling_status ocsp_stapling_status() const noexcept {
      return ::fxe::net::ocsp_stapling_status::not_requested;
    }
    virtual std::string last_error() const = 0;
    virtual void close() = 0;
  };

} // namespace fxe::net
