#pragma once

#include "net/http2_client.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fxe::net {
  struct http2_server_options {
    std::string cert_pem;
    std::string key_pem;
    uint16_t port = 0;
    std::vector<std::string> alpn = {"h2"};
    http2_settings settings;
  };

  struct http2_incoming_request {
    uint64_t id = 0;
    int32_t stream_id = 0;
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  class http2_server {
  public:
    static std::unique_ptr<http2_server> listen(const http2_server_options& options,
                                                std::string& err);
    virtual ~http2_server();
    virtual uint16_t local_port() const = 0;
    virtual std::optional<http2_incoming_request> poll(std::string& err) = 0;
    virtual bool respond(uint64_t request_id, const http2_response& response, std::string& err) = 0;
    virtual void close() = 0;
    virtual std::string last_error() const = 0;
  };
} // namespace fxe::net
