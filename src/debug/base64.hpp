// Minimal base64 encode/decode for the fxe debug protocol.
//
// Standard alphabet, '=' padding. encode() always pads. decode() requires
// length % 4 == 0 and rejects characters outside the base64 alphabet/padding.
// Empty input intentionally decodes to empty output.

#pragma once

#include <fxe/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::debug::base64 {
  std::string encode(const u8* data, usize n);
  inline std::string encode(std::string_view bytes) {
    return encode(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  }

  // Returns decoded bytes, or std::nullopt on malformed input.
  [[nodiscard]] std::optional<std::vector<u8>> decode(std::string_view encoded);
} // namespace fxe::debug::base64
