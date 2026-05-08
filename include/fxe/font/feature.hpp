#pragma once

// OpenType feature/variation primitives shared between the shaper and face
// modules. Kept dependency-free so JS bindings and tests can speak the same
// types as the C++ core.

#include <array>
#include <cstdint>
#include <fxe/types.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::font {

  // A 4-character OpenType feature/axis tag, e.g. "liga", "wght". Stored as
  // a packed big-endian uint32 the way HarfBuzz/CoreText expect.
  struct Tag {
    std::array<char, 4> chars{' ', ' ', ' ', ' '};

    constexpr Tag() = default;
    constexpr Tag(std::string_view sv) noexcept {
      for (usize i = 0; i < 4; ++i)
        chars[i] = i < sv.size() ? sv[i] : ' ';
    }
    constexpr Tag(const char* s) noexcept : Tag(std::string_view{s}) {}

    [[nodiscard]] constexpr u32 packed() const noexcept {
      return (static_cast<u32>(static_cast<unsigned char>(chars[0])) << 24) |
             (static_cast<u32>(static_cast<unsigned char>(chars[1])) << 16) |
             (static_cast<u32>(static_cast<unsigned char>(chars[2])) << 8) |
             (static_cast<u32>(static_cast<unsigned char>(chars[3])));
    }

    [[nodiscard]] std::string str() const {
      return std::string{chars.data(), 4};
    }

    friend constexpr bool operator==(const Tag&, const Tag&) noexcept = default;
  };

  // A single OpenType feature setting: tag + on/off (or numeric value for
  // ranged features such as `cv01=2`).
  struct Feature {
    Tag tag;
    u32 value = 1;
    u32 start = 0;                  // run-relative byte offset, 0 = whole run
    u32 end = static_cast<u32>(-1); // -1 = end-of-run
  };

  // A variation axis setting: tag + value (e.g. `wght=600`). Used by
  // OpenType variable fonts.
  struct Variation {
    Tag tag;
    float value = 0.0f;
  };

  // Default feature list applied to every shape() call unless the caller
  // overrides it. Mirrors Ghostty's default set: contextual alternates +
  // ligatures + kerning. Discretionary ligatures are intentionally off.
  inline std::vector<Feature> default_features() {
    return {
        Feature{Tag{"calt"}, 1},
        Feature{Tag{"liga"}, 1},
        Feature{Tag{"kern"}, 1},
    };
  }

} // namespace fxe::font
