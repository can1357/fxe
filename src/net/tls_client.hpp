#pragma once

#include <cstddef>
#include <cstdint>
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
    uint16_t port = 443;
    std::string ca_pem;
    bool reject_unauthorized = true;
    std::vector<std::string> alpn;
    std::string client_cert_pem;
    std::string client_key_pem;
    bool request_ocsp_stapling = true;
    bool enable_session_resumption = true;
  };

  class tls_client {
  public:
    static std::unique_ptr<tls_client> connect(const tls_options&, std::string& err);
    virtual ~tls_client();

    virtual ssize_t read(void* buf, size_t cap) = 0;
    virtual ssize_t write(const void* buf, size_t len) = 0;
    virtual std::string negotiated_alpn() const = 0;
    virtual std::optional<std::string> peer_cert_subject() const = 0;
    virtual std::string last_error() const = 0;
    virtual void close() = 0;
  };

} // namespace fxe::net
