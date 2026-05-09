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
#include <fxe/types.hpp>

namespace fxe::font {

  struct ShapedGlyph {
    u32 glyph_id = 0;
    float x_advance = 0.0f;
    float y_advance = 0.0f;
    float x_offset = 0.0f;
    float y_offset = 0.0f;
    u32 cluster = 0; // byte offset in input UTF-8
  };

  enum class Direction : u8 {
    ltr = 0,
    rtl = 1,
  };

  struct ShapeRun {
    std::vector<ShapedGlyph> glyphs;
    Direction direction = Direction::ltr;
    // Total inked advance in pixels. Convenience for measureText().
    float total_advance = 0.0f;
    // Resolved face for THIS run. Non-owning. When CoreText / HarfBuzz cascade
    // substitutes a different font for unsupported codepoints (e.g. emoji
    // falling back to Apple Color Emoji), the shaper emits a separate run
    // pointing at that substitute Face. `nullptr` means "use the caller-
    // supplied face" — that is the common single-script path.
    Face* face = nullptr;
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
    // Returns one ShapeRun per resolved face. Single-script Latin text
    // typically returns a vector of size 1; mixed text (text + emoji,
    // CJK + Latin, etc.) yields one run per font segment, each with its
    // own `face` pointer the renderer should use to look up glyphs.
    [[nodiscard]] virtual std::vector<ShapeRun> shape(Face& face, std::string_view utf8,
                                                      const ShapeOptions& opts) = 0;
  };

  // Process-wide shaper singleton. The shaper holds backend-specific caches
  // (most importantly the CoreText cascade fallback `substitute_faces_` map);
  // returning a fresh instance per call would mint new face IDs for every
  // shape() invocation, blow up the glyph cache, and corrupt cached text
  // vertex UVs. Callers must NOT assume ownership; the returned pointer
  // outlives every Face/CommandBuffer interaction.
  [[nodiscard]] Shaper* default_shaper();

} // namespace fxe::font
