#include "cdp_ws.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fxe/types.hpp>
#include <mbedtls/md.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace fxe::debug::cdp_ws {
  namespace {
    constexpr std::string_view kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    int send_flags() {
#if defined(MSG_NOSIGNAL)
      return MSG_NOSIGNAL;
#else
      return 0;
#endif
    }

    std::string lower(std::string_view s) {
      std::string out;
      out.reserve(s.size());
      for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      return out;
    }

    std::string trim(std::string_view s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
      return std::string(s);
    }

    void append_be16(std::string& out, u16 v) {
      out.push_back(static_cast<char>((v >> 8) & 0xffu));
      out.push_back(static_cast<char>(v & 0xffu));
    }

    void append_be64(std::string& out, u64 v) {
      for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xffu));
    }

    bool read_exact_from_socket(socket_t s, std::string& into) {
      char ch;
#if defined(_WIN32)
      int n = ::recv(s, &ch, 1, 0);
#else
      ssize_t n = ::recv(s, &ch, 1, 0);
#endif
      if (n <= 0)
        return false;
      into.push_back(ch);
      return true;
    }
  } // namespace

  std::array<u8, 20> sha1(std::string_view bytes) {
    std::array<u8, 20> out{};
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    // info is non-null for SHA1 in any mbedTLS build that ships SHA-1 (always-on in our config).
    // mbedtls_md returns 0 on success; on failure we fall through with a zeroed digest, which
    // will produce an incorrect handshake and be caught by the WebSocket peer. mbedTLS SHA-1
    // cannot fail for a non-null info and a valid input pointer, so this is a defensive safety
    // net rather than a runtime path.
    (void)mbedtls_md(info, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                     out.data());
    return out;
  }

  std::string sha1_hex(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    auto digest = sha1(bytes);
    std::string out;
    out.reserve(40);
    for (u8 b : digest) {
      out.push_back(kHex[(b >> 4u) & 0x0fu]);
      out.push_back(kHex[b & 0x0fu]);
    }
    return out;
  }

  std::optional<std::string> websocket_accept(std::string_view sec_websocket_key) {
    if (sec_websocket_key.empty())
      return std::nullopt;
    // Standard WebSocket handshake: the client nonce decodes to exactly 16 bytes.
    if (sec_websocket_key.size() % 4 != 0)
      return std::nullopt;
    std::array<u8, 16> nonce{};
    usize nonce_len = 0;
    const char* b64_end = nullptr;
    if (sodium_base642bin(nonce.data(), nonce.size(), sec_websocket_key.data(),
                          sec_websocket_key.size(), nullptr, &nonce_len, &b64_end,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        nonce_len != nonce.size())
      return std::nullopt;
    std::string seed(sec_websocket_key);
    seed += kMagic;
    auto digest = sha1(seed);
    // Sec-WebSocket-Accept: base64 of a 20-byte SHA-1 digest = 28 chars padded.
    constexpr usize kAcceptLen =
        sodium_base64_ENCODED_LEN(20u, sodium_base64_VARIANT_ORIGINAL) - 1u;
    std::array<char, kAcceptLen + 1u> accept_buf{};
    sodium_bin2base64(accept_buf.data(), accept_buf.size(),
                      reinterpret_cast<const unsigned char*>(digest.data()), digest.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    return std::string(accept_buf.data(), kAcceptLen);
  }

  std::string http_request::header(std::string_view name) const {
    auto it = headers.find(lower(name));
    return it == headers.end() ? std::string{} : it->second;
  }

  bool parse_http_request(std::string_view raw, http_request& out) {
    out = http_request{};
    auto first_end = raw.find("\r\n");
    if (first_end == std::string_view::npos)
      return false;
    std::string_view first = raw.substr(0, first_end);
    auto sp1 = first.find(' ');
    if (sp1 == std::string_view::npos)
      return false;
    auto sp2 = first.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos)
      return false;
    out.method = std::string(first.substr(0, sp1));
    out.path = std::string(first.substr(sp1 + 1, sp2 - sp1 - 1));
    out.version = std::string(first.substr(sp2 + 1));

    usize pos = first_end + 2;
    while (pos < raw.size()) {
      auto end = raw.find("\r\n", pos);
      if (end == std::string_view::npos)
        return false;
      if (end == pos)
        return true;
      std::string_view line = raw.substr(pos, end - pos);
      auto colon = line.find(':');
      if (colon != std::string_view::npos)
        out.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
      pos = end + 2;
    }
    return true;
  }

  bool read_http_request(socket_t s, http_request& out, std::string& error) {
    std::string raw;
    raw.reserve(1024);
    while (raw.find("\r\n\r\n") == std::string::npos) {
      if (raw.size() >= 65536u) {
        error = "HTTP request headers too large";
        return false;
      }
      if (!read_exact_from_socket(s, raw)) {
        error = "socket closed while reading HTTP request";
        return false;
      }
    }
    if (!parse_http_request(raw, out)) {
      error = "malformed HTTP request";
      return false;
    }
    return true;
  }

  std::string http_response(int status, std::string_view reason, std::string_view content_type,
                            std::string_view body) {
    std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
        "\r\nContent-Type: " + std::string(content_type) +
        "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
        std::string(body);
    return response;
  }

  std::string handshake_response(const http_request& req) {
    auto accept = websocket_accept(req.header("sec-websocket-key"));
    if (!accept)
      return {};
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " +
           *accept + "\r\n\r\n";
  }

  bool handshake(socket_t s, const http_request& req, std::string& error) {
    if (req.method != "GET") {
      error = "WebSocket handshake requires GET";
      return false;
    }
    std::string upgrade = lower(req.header("upgrade"));
    std::string connection = lower(req.header("connection"));
    if (upgrade != "websocket" || connection.find("upgrade") == std::string::npos) {
      error = "missing WebSocket upgrade headers";
      return false;
    }
    if (req.header("sec-websocket-version") != "13") {
      error = "unsupported WebSocket version";
      return false;
    }
    std::string sec_key = req.header("sec-websocket-key");
    auto accept = websocket_accept(sec_key);
    if (!accept) {
      error = sec_key.empty() ? "missing Sec-WebSocket-Key" : "invalid Sec-WebSocket-Key";
      return false;
    }
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           *accept + "\r\n\r\n";
    if (!send_all(s, response)) {
      error = "send WebSocket handshake failed";
      return false;
    }
    return true;
  }

  std::string encode_frame(std::string_view payload, u8 opcode, bool mask, u32 mask_key) {
    std::string out;
    out.reserve(payload.size() + 14);
    out.push_back(static_cast<char>(0x80u | (opcode & 0x0fu)));
    u64 len = static_cast<u64>(payload.size());
    u8 mask_bit = mask ? 0x80u : 0u;
    if (len <= 125u) {
      out.push_back(static_cast<char>(mask_bit | static_cast<u8>(len)));
    } else if (len <= 65535u) {
      out.push_back(static_cast<char>(mask_bit | 126u));
      append_be16(out, static_cast<u16>(len));
    } else {
      out.push_back(static_cast<char>(mask_bit | 127u));
      append_be64(out, len);
    }

    std::array<u8, 4> key = {
        static_cast<u8>((mask_key >> 24u) & 0xffu), static_cast<u8>((mask_key >> 16u) & 0xffu),
        static_cast<u8>((mask_key >> 8u) & 0xffu), static_cast<u8>(mask_key & 0xffu)};
    if (mask) {
      for (u8 b : key)
        out.push_back(static_cast<char>(b));
    }
    for (usize i = 0; i < payload.size(); ++i) {
      u8 b = static_cast<u8>(payload[i]);
      if (mask)
        b ^= key[i % 4u];
      out.push_back(static_cast<char>(b));
    }
    return out;
  }

  bool decode_frame_from_buffer(std::string& buffer, frame& out, std::string& error,
                                bool require_mask) {
    out = frame{};
    if (buffer.size() < 2)
      return false;
    const auto* data = reinterpret_cast<const u8*>(buffer.data());
    u8 b0 = data[0];
    u8 b1 = data[1];
    bool rsv = (b0 & 0x70u) != 0;
    if (rsv) {
      error = "reserved WebSocket bits set";
      return false;
    }
    bool fin = (b0 & 0x80u) != 0;
    u8 opcode = b0 & 0x0fu;
    bool masked = (b1 & 0x80u) != 0;
    u64 len = b1 & 0x7fu;
    usize pos = 2;
    if (len == 126u) {
      if (buffer.size() < pos + 2u)
        return false;
      len = (static_cast<u64>(data[pos]) << 8u) | data[pos + 1u];
      pos += 2;
    } else if (len == 127u) {
      if (buffer.size() < pos + 8u)
        return false;
      len = 0;
      for (int i = 0; i < 8; ++i)
        len = (len << 8u) | data[pos + static_cast<usize>(i)];
      pos += 8;
      if ((len & (1ull << 63u)) != 0) {
        error = "invalid WebSocket 64-bit length";
        return false;
      }
    }
    if (require_mask && !masked) {
      error = "client WebSocket frames must be masked";
      return false;
    }
    std::array<u8, 4> key{};
    if (masked) {
      if (buffer.size() < pos + 4u)
        return false;
      for (usize i = 0; i < 4u; ++i)
        key[i] = data[pos + i];
      pos += 4;
    }
    if (len > static_cast<u64>(max_ws_frame_bytes)) {
      error = "WebSocket frame exceeds 16777216-byte limit";
      return false;
    }
    if (len > static_cast<u64>(buffer.size() - pos))
      return false;
    if (opcode >= 0x8u) {
      if (!fin) {
        error = "fragmented WebSocket control frame";
        return false;
      }
      if (len > 125u) {
        error = "oversized WebSocket control frame";
        return false;
      }
    }
    out.fin = fin;
    out.opcode = opcode;
    out.payload.resize(static_cast<usize>(len));
    for (usize i = 0; i < static_cast<usize>(len); ++i) {
      u8 b = data[pos + i];
      if (masked)
        b ^= key[i % 4u];
      out.payload[i] = static_cast<char>(b);
    }
    buffer.erase(0, pos + static_cast<usize>(len));
    return true;
  }

  bool send_all(socket_t s, std::string_view bytes) {
    const char* p = bytes.data();
    usize left = bytes.size();
    while (left > 0) {
#if defined(_WIN32)
      int n = ::send(s, p, static_cast<int>(left), send_flags());
#else
      ssize_t n = ::send(s, p, left, send_flags());
#endif
      if (n <= 0)
        return false;
      p += n;
      left -= static_cast<usize>(n);
    }
    return true;
  }

  bool write_text(socket_t s, std::string_view payload) {
    return send_all(s, encode_frame(payload, 0x1, false));
  }

  bool write_close(socket_t s, u16 code, std::string_view reason) {
    std::string payload;
    append_be16(payload, code);
    payload += reason;
    return send_all(s, encode_frame(payload, 0x8, false));
  }

  bool write_pong(socket_t s, std::string_view payload) {
    return send_all(s, encode_frame(payload, 0xA, false));
  }

  read_result reader::read_text(socket_t s) {
    for (;;) {
      frame fr;
      std::string error;
      if (!decode_frame_from_buffer(buffer_, fr, error, require_mask_)) {
        if (!error.empty())
          return {read_result::status::error, {}, std::move(error)};
        char chunk[4096];
#if defined(_WIN32)
        int n = ::recv(s, chunk, sizeof(chunk), 0);
#else
        ssize_t n = ::recv(s, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0)
          return {read_result::status::closed, {}, {}};
        buffer_.append(chunk, static_cast<usize>(n));
        continue;
      }

      switch (fr.opcode) {
      case 0x0: // continuation
        if (fragmented_opcode_ == 0)
          return {read_result::status::error, {}, "unexpected WebSocket continuation frame"};
        fragmented_ += fr.payload;
        if (fr.fin) {
          std::string msg = std::move(fragmented_);
          fragmented_.clear();
          fragmented_opcode_ = 0;
          return {read_result::status::message, std::move(msg), {}};
        }
        break;
      case 0x1: // text
        if (fr.fin)
          return {read_result::status::message, std::move(fr.payload), {}};
        if (fragmented_opcode_ != 0)
          return {read_result::status::error, {}, "nested WebSocket fragmented message"};
        fragmented_opcode_ = fr.opcode;
        fragmented_ = std::move(fr.payload);
        break;
      case 0x8: // close
        write_close(s);
        return {read_result::status::closed, {}, {}};
      case 0x9: // ping
        write_pong(s, fr.payload);
        break;
      case 0xA: // pong
        break;
      default:
        return {read_result::status::error, {}, "unsupported WebSocket opcode"};
      }
    }
  }
} // namespace fxe::debug::cdp_ws
