#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fxe::net {
  struct http2_settings {
    std::optional<uint32_t> header_table_size;
    std::optional<uint32_t> enable_push;
    std::optional<uint32_t> max_concurrent_streams;
    std::optional<uint32_t> initial_window_size;
    std::optional<uint32_t> max_frame_size;
    std::optional<uint32_t> max_header_list_size;
  };

  struct http2_request {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  struct http2_response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  class http2_client {
  public:
    static std::unique_ptr<http2_client> connect(const std::string& host, uint16_t port,
                                                 std::string& err);
    static std::unique_ptr<http2_client> connect(const std::string& host, uint16_t port,
                                                 const http2_settings& settings, std::string& err);
    static std::unique_ptr<http2_client> connect(const std::string& host, uint16_t port,
                                                 const std::string& ca_pem,
                                                 bool reject_unauthorized, std::string& err);
    static std::unique_ptr<http2_client> connect(const std::string& host, uint16_t port,
                                                 const std::string& ca_pem,
                                                 bool reject_unauthorized,
                                                 const http2_settings& settings, std::string& err);
    virtual ~http2_client();
    virtual int32_t submit(const http2_request& request) = 0;
    virtual http2_response wait(int32_t stream_id, std::string& err) = 0;
    virtual void close() = 0;
    virtual std::string last_error() const = 0;
  };
} // namespace fxe::net
