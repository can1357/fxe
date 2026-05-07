#pragma once

// Shaper — turn UTF-8 + face into a sequence of positioned glyphs. The
// active shaper (HarfBuzz or CoreText CTLine) is selected by the build
// system; the API is identical so callers don't care.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <fxe/font/face.hpp>
#include <fxe/font/feature.hpp>

namespace fxe::font {

  struct ShapedGlyph {
    std::uint32_t glyph_id = 0;
    float x_advance = 0.0f;
    float y_advance = 0.0f;
    float x_offset = 0.0f;
    float y_offset = 0.0f;
    std::uint32_t cluster = 0; // byte offset in input UTF-8
  };

  enum class Direction : std::uint8_t {
    ltr = 0,
    rtl = 1,
  };

  struct ShapeRun {
    std::vector<ShapedGlyph> glyphs;
    Direction direction = Direction::ltr;
    // Total inked advance in pixels. Convenience for measureText().
    float total_advance = 0.0f;
  };

  struct ShapeOptions {
    std::vector<Feature> features;     // empty = default_features()
    std::vector<Variation> variations; // empty = leave face untouched
    Direction direction = Direction::ltr;
    // BCP-47 language tag (e.g. "en", "ar") for shaping hints. Optional.
    std::string_view language;
    // ISO 15924 script tag (e.g. "Latn", "Arab"). Optional; the shaper auto-
    // detects from the input when unset.
    std::string_view script;
  };

  class Shaper {
  public:
    virtual ~Shaper() = default;
    [[nodiscard]] virtual ShapeRun shape(Face& face, std::string_view utf8,
                                         const ShapeOptions& opts) = 0;
  };

  [[nodiscard]] std::unique_ptr<Shaper> default_shaper();

} // namespace fxe::font
