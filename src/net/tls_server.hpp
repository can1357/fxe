#pragma once

#include "tls_client.hpp"

#include <cstdint>
#include <fxe/types.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fxe::net {

  struct tls_server_options {
    std::string cert_pem;
    std::string key_pem;
    std::vector<std::string> alpn;
    u16 port = 0;
    bool request_client_cert = false;
  };

  class tls_server {
  public:
    static std::unique_ptr<tls_server> listen(const tls_server_options&, std::string& err);
    virtual ~tls_server();

    virtual std::unique_ptr<tls_client> accept(std::string& err) = 0;
    virtual bool verify_client_cert(const std::string& expected_subject_or_fingerprint) = 0;
    virtual u16 local_port() const = 0;
    virtual std::string last_error() const = 0;
    virtual void close() = 0;
  };

} // namespace fxe::net
