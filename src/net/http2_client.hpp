#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <fxe/types.hpp>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fxe::net {
  struct http2_settings {
    std::optional<u32> header_table_size;
    std::optional<u32> enable_push;
    std::optional<u32> max_concurrent_streams;
    std::optional<u32> initial_window_size;
    std::optional<u32> stream_max_recv_window_size;
    std::optional<u32> max_frame_size;
    std::optional<u32> max_header_list_size;
  };

  struct http2_request {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 0; // 0 = no timeout
  };

  struct http2_response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  struct http2_pushed {
    i32 stream_id = 0;
    i32 associated_stream_id = 0;
    http2_request promised_request;
    http2_response response;
  };

  class http2_client {
  public:
    using push_handler = std::function<void(http2_pushed)>;
    using on_data_consumed = std::function<void(i32 stream_id, std::size_t chunk_size)>;

    // Push streams are surfaced only through push_handler. When no handler is
    // installed, promised streams are refused with REFUSED_STREAM.
    static std::unique_ptr<http2_client> connect(const std::string& host, u16 port,
                                                 std::string& err);
    static std::unique_ptr<http2_client> connect(const std::string& host, u16 port,
                                                 const http2_settings& settings, std::string& err);
    static std::unique_ptr<http2_client> connect(const std::string& host, u16 port,
                                                 const std::string& ca_pem,
                                                 bool reject_unauthorized, std::string& err);
    static std::unique_ptr<http2_client> connect(const std::string& host, u16 port,
                                                 const std::string& ca_pem,
                                                 bool reject_unauthorized,
                                                 const http2_settings& settings, std::string& err);
    static std::unique_ptr<http2_client>
    connect(const std::string& host, u16 port, const std::string& ca_pem, bool reject_unauthorized,
            const http2_settings& settings, std::string session_namespace, std::string& err);
    virtual ~http2_client();
    virtual i32 submit(const http2_request& request) = 0;
    virtual http2_response wait(i32 stream_id, std::string& err) = 0;
    virtual void cancel(i32 stream_id, u32 error_code = NGHTTP2_CANCEL) = 0;
    virtual void close() = 0;
    virtual std::string last_error() const = 0;
    virtual void set_push_handler(push_handler handler) = 0;
    virtual void set_on_data_consumed(on_data_consumed handler) = 0;
    virtual void set_stream_window(i32 stream_id, i32 window_size) = 0;
  };
} // namespace fxe::net
