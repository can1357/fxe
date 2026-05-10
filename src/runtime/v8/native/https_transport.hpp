#pragma once

#include "net/http_client.hpp"

#include <memory>
#include <string>

namespace fxe::runtime {

  struct native_https_request_options {
    bool reject_unauthorized = true;
    std::string ca_pem;
    std::string ca_path;
    std::string session_namespace;
    int max_redirects = 10;
  };

  class native_https_request_handle {
  public:
    virtual ~native_https_request_handle();

    [[nodiscard]] virtual bool poll(fxe::net::http_response& out) = 0;
    virtual void abort() = 0;
  };

  [[nodiscard]] std::unique_ptr<native_https_request_handle>
  start_native_https_request(fxe::net::http_request req, fxe::net::cookie_jar* jar = nullptr,
                             native_https_request_options opts = {});

  [[nodiscard]] fxe::net::http_response
  perform_native_https_request(fxe::net::http_request req, fxe::net::cookie_jar* jar = nullptr,
                               native_https_request_options opts = {});

} // namespace fxe::runtime
