#include "base64.hpp"
#include <fxe/types.hpp>

namespace fxe::debug::base64 {
  namespace {
    constexpr char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    constexpr int decode_char(char c) {
      if (c >= 'A' && c <= 'Z')
        return c - 'A';
      if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
      if (c >= '0' && c <= '9')
        return c - '0' + 52;
      if (c == '+')
        return 62;
      if (c == '/')
        return 63;
      return -1;
    }
  } // namespace

  std::string encode(const u8* data, usize n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    usize i = 0;
    while (i + 3 <= n) {
      u32 v = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | u32(data[i + 2]);
      out.push_back(kAlpha[(v >> 18) & 0x3f]);
      out.push_back(kAlpha[(v >> 12) & 0x3f]);
      out.push_back(kAlpha[(v >> 6) & 0x3f]);
      out.push_back(kAlpha[v & 0x3f]);
      i += 3;
    }
    if (i < n) {
      u32 v = u32(data[i]) << 16;
      bool two = (i + 1 < n);
      if (two)
        v |= u32(data[i + 1]) << 8;
      out.push_back(kAlpha[(v >> 18) & 0x3f]);
      out.push_back(kAlpha[(v >> 12) & 0x3f]);
      out.push_back(two ? kAlpha[(v >> 6) & 0x3f] : '=');
      out.push_back('=');
    }
    return out;
  }

  std::optional<std::vector<u8>> decode(std::string_view s) {
    if (s.empty())
      return std::vector<u8>{};
    if (s.size() % 4 != 0)
      return std::nullopt;

    for (usize i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (c != '=' && decode_char(c) < 0)
        return std::nullopt;
      if (c == '=' && i < s.size() - 2u)
        return std::nullopt;
    }

    std::vector<u8> out;
    out.reserve((s.size() / 4) * 3);
    for (usize i = 0; i < s.size(); i += 4) {
      int a = decode_char(s[i]);
      int b = decode_char(s[i + 1]);
      int c = s[i + 2] == '=' ? -2 : decode_char(s[i + 2]);
      int d = s[i + 3] == '=' ? -2 : decode_char(s[i + 3]);
      if (a < 0 || b < 0)
        return std::nullopt;
      out.push_back(u8((a << 2) | (b >> 4)));
      if (c == -2) {
        if (d != -2)
          return std::nullopt;
        if (i + 4u != s.size())
          return std::nullopt;
        break;
      }
      if (c < 0)
        return std::nullopt;
      out.push_back(u8(((b & 0xf) << 4) | (c >> 2)));
      if (d == -2) {
        if (i + 4u != s.size())
          return std::nullopt;
        break;
      }
      if (d < 0)
        return std::nullopt;
      out.push_back(u8(((c & 0x3) << 6) | d));
    }
    return out;
  }
} // namespace fxe::debug::base64
