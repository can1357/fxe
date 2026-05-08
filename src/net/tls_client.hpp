#pragma once

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

  struct tls_options {
    std::string host;
    u16 port = 443;
    std::string ca_pem;
    std::string ca_path; // file path; takes precedence if non-empty
    bool reject_unauthorized = true;
    std::vector<std::string> alpn;
    std::string client_cert_pem;
    std::string client_cert_path; // file path; takes precedence if non-empty
    std::string client_key_pem;
    std::string client_key_path; // file path; takes precedence if non-empty
    bool request_ocsp_stapling = true;
    bool enable_session_resumption = true;
  };

  class tls_client {
  public:
    static std::unique_ptr<tls_client> connect(const tls_options&, std::string& err);
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
    virtual std::string last_error() const = 0;
    virtual void close() = 0;
  };

} // namespace fxe::net
