#include <fxe/webauthn.hpp>

#include <mbedtls/sha256.h>

#include <array>
#include <string>
#include <vector>

namespace fxe::webauthn {
  namespace {

    std::string base64url_no_pad(std::span<const uint8_t> bytes) {
      static constexpr char k_table[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

      std::string out;
      out.reserve(((bytes.size() + 2u) / 3u) * 4u);
      size_t i = 0;
      while (i + 3u <= bytes.size()) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                               (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                               static_cast<uint32_t>(bytes[i + 2]);
        out.push_back(k_table[(chunk >> 18) & 0x3f]);
        out.push_back(k_table[(chunk >> 12) & 0x3f]);
        out.push_back(k_table[(chunk >> 6) & 0x3f]);
        out.push_back(k_table[chunk & 0x3f]);
        i += 3;
      }
      const size_t remain = bytes.size() - i;
      if (remain == 1u) {
        const uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16;
        out.push_back(k_table[(chunk >> 18) & 0x3f]);
        out.push_back(k_table[(chunk >> 12) & 0x3f]);
      } else if (remain == 2u) {
        const uint32_t chunk =
            (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
        out.push_back(k_table[(chunk >> 18) & 0x3f]);
        out.push_back(k_table[(chunk >> 12) & 0x3f]);
        out.push_back(k_table[(chunk >> 6) & 0x3f]);
      }
      return out;
    }

    void append_json_escaped(std::string& out, std::string_view input) {
      static constexpr char k_hex[] = "0123456789abcdef";
      for (char ch_raw : input) {
        const auto ch = static_cast<unsigned char>(ch_raw);
        switch (ch) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\b':
          out += "\\b";
          break;
        case '\f':
          out += "\\f";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (ch < 0x20u) {
            out += "\\u00";
            out.push_back(k_hex[(ch >> 4) & 0x0f]);
            out.push_back(k_hex[ch & 0x0f]);
          } else {
            out.push_back(static_cast<char>(ch));
          }
          break;
        }
      }
    }

    std::string type_string(client_data_type type) {
      return type == client_data_type::create ? "webauthn.create" : "webauthn.get";
    }

  } // namespace

  std::array<uint8_t, 32> sha256(std::span<const uint8_t> bytes) {
    std::array<uint8_t, 32> digest{};
    const auto* data = bytes.empty() ? nullptr : bytes.data();
    (void)mbedtls_sha256(data, bytes.size(), digest.data(), 0);
    return digest;
  }

  client_data build_client_data(client_data_type type, std::span<const uint8_t> challenge,
                                std::string_view origin, bool cross_origin) {
    client_data out;
    const std::string challenge_b64 = base64url_no_pad(challenge);
    out.json.reserve(origin.size() + challenge_b64.size() + 96u);
    out.json += '{';
    out.json += "\"type\":\"";
    out.json += type_string(type);
    out.json += "\",\"challenge\":\"";
    out.json += challenge_b64;
    out.json += "\",\"origin\":\"";
    append_json_escaped(out.json, origin);
    out.json += '"';
    if (cross_origin)
      out.json += ",\"crossOrigin\":true";
    out.json += '}';

    const auto bytes =
        std::span(reinterpret_cast<const uint8_t*>(out.json.data()), out.json.size());
    out.hash = sha256(bytes);
    return out;
  }

} // namespace fxe::webauthn
