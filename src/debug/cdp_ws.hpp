#pragma once

#include <fxe/types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
namespace fxe::debug::cdp_ws {
  using socket_t = SOCKET;
}
#else
namespace fxe::debug::cdp_ws {
  using socket_t = int;
}
#endif

namespace fxe::debug::cdp_ws {
  // Maximum decoded WebSocket frame payload accepted by the debug server.
  // Prevents a peer from advertising a huge payload and forcing allocation.
  constexpr usize max_ws_frame_bytes = 16u * 1024u * 1024u;

  struct http_request {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;

    [[nodiscard]] std::string header(std::string_view name) const;
  };

  struct frame {
    bool fin = true;
    std::uint8_t opcode = 0;
    std::string payload;
  };

  struct read_result {
    enum class status { message, closed, error } state = status::error;
    std::string message;
    std::string error;
  };

  std::array<u8, 20> sha1(std::string_view bytes);
  std::string sha1_hex(std::string_view bytes);
  std::optional<std::string> websocket_accept(std::string_view sec_websocket_key);

  bool parse_http_request(std::string_view raw, http_request& out);
  bool read_http_request(socket_t s, http_request& out, std::string& error);

  std::string http_response(int status, std::string_view reason, std::string_view content_type,
                            std::string_view body);
  std::string handshake_response(const http_request& req);
  bool handshake(socket_t s, const http_request& req, std::string& error);

  std::string encode_frame(std::string_view payload, std::uint8_t opcode = 0x1, bool mask = false,
                           std::uint32_t mask_key = 0x11223344u);
  bool decode_frame_from_buffer(std::string& buffer, frame& out, std::string& error,
                                bool require_mask = true);

  bool send_all(socket_t s, std::string_view bytes);
  bool write_text(socket_t s, std::string_view payload);
  bool write_close(socket_t s, std::uint16_t code = 1000, std::string_view reason = {});
  bool write_pong(socket_t s, std::string_view payload);

  class reader {
  public:
    explicit reader(bool require_mask = true) : require_mask_(require_mask) {}
    read_result read_text(socket_t s);

  private:
    bool require_mask_ = true;
    std::string buffer_;
    std::string fragmented_;
    std::uint8_t fragmented_opcode_ = 0;
  };
} // namespace fxe::debug::cdp_ws
