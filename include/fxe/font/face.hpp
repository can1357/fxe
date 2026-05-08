#pragma once

// Face — a loaded font at a specific size. Backend-agnostic interface; each
// backend (FreeType, CoreText) provides a concrete implementation.
//
// Faces are cheap to construct and own a backend handle plus optional
// per-size cache state. They DO NOT own an atlas — rasterized glyphs are
// stamped into a caller-provided `font::Atlas` via `render_glyph`.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fxe/font/feature.hpp>
#include <fxe/font/glyph.hpp>
#include <fxe/math.hpp>
#include <fxe/types.hpp>

namespace fxe::font {

  class Atlas;

  // Per-face metrics, all in pixels at the active size.
  struct FaceMetrics {
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_gap = 0.0f;
    float line_height = 0.0f;
    float underline_position = 0.0f;
    float underline_thickness = 0.0f;
    // Mean advance for ASCII 'M', useful for "fixed-pitch-ish" measurements.
    float em_advance = 0.0f;
  };

  // Style flags. Used by `Collection` to key face slots.
  enum class Style : u8 {
    regular = 0,
    bold = 1,
    italic = 2,
    bold_italic = 3,
  };

  // Hinting strategy. Maps to FT load flags / CT options.
  enum class Hint : u8 {
    none = 0,
    light = 1,
    full = 2,
  };

  class Face {
  public:
    virtual ~Face() = default;

    // Stable id derived from the underlying font data + size. Used as the
    // face component of the glyph cache key.
    [[nodiscard]] virtual u64 id() const noexcept = 0;

    [[nodiscard]] virtual std::string family_name() const = 0;
    [[nodiscard]] virtual Style style() const noexcept = 0;
    [[nodiscard]] virtual float pixel_size() const noexcept = 0;
    [[nodiscard]] virtual FaceMetrics metrics() const noexcept = 0;

    // Whether this face contains color (CBDT/sbix/COLR) glyphs. The result
    // is whole-face; per-glyph color is reported by `render_glyph` via the
    // returned `Glyph::format`.
    [[nodiscard]] virtual bool has_color() const noexcept = 0;

    // Maps a Unicode codepoint to a glyph id, or 0 if the face does not
    // cover the codepoint.
    [[nodiscard]] virtual u32 glyph_index(char32_t cp) const noexcept = 0;

    // Sets active OpenType variation axes (variable fonts). No-op on faces
    // without variations or on backends that don't support them.
    virtual void set_variations(std::span<const Variation> v) = 0;

    // Renders glyph `glyph_id` into `atlas` and returns the resulting
    // record. Returns a zero-sized Glyph if rendering failed or the glyph
    // is empty (e.g. space).
    //
    // `subpixel_x` is a horizontal sub-pixel offset in pixel units, in
    // the half-open interval [0, 1). Backends that support sub-pixel
    // positioning (e.g. CoreText, FreeType with hinting) should bake the
    // shift into the rasterised bitmap so that drawing the result at an
    // integer pen position reproduces the requested sub-pixel placement.
    // Backends that ignore it produce identical output regardless and
    // simply waste one cache slot per bin — still correct, just less
    // crisp during shaped text with fractional advances (kerning, etc.).
    [[nodiscard]] virtual Glyph render_glyph(u32 glyph_id, Atlas& mask, Atlas& color,
                                             Hint hint = Hint::full, float subpixel_x = 0.0f) = 0;

    // Backend handle. For FT-backed faces this is `FT_Face`; for CT it's
    // `CTFontRef`. Returned as void* so headers stay free of FT/CT.
    [[nodiscard]] virtual void* native_handle() const noexcept = 0;
  };

  // Factory: construct a Face from raw font bytes. The bytes are copied
  // into the face. `face_index` selects a face inside .ttc collections.
  [[nodiscard]] std::unique_ptr<Face> load_face_from_bytes(std::span<const u8> bytes,
                                                           float pixel_size, u32 face_index = 0);

  // Factory: construct a Face from a file path.
  [[nodiscard]] std::unique_ptr<Face> load_face_from_file(std::string_view path, float pixel_size,
                                                          u32 face_index = 0);

  // Backend-specific factories. The CoreText one is only available when
  // `FXE_FONT_HAS_CORETEXT`; calling it from a non-CT build returns nullptr.
  // The FreeType one is similarly only available with `FXE_FONT_HAS_FREETYPE`.
  [[nodiscard]] std::unique_ptr<Face> load_face_freetype(std::span<const u8> bytes,
                                                         float pixel_size, u32 face_index = 0);
  [[nodiscard]] std::unique_ptr<Face> load_face_coretext(std::span<const u8> bytes,
                                                         float pixel_size);
  [[nodiscard]] std::unique_ptr<Face> load_face_coretext_name(std::string_view family,
                                                              float pixel_size, Style style);
  // CoreText only: wraps an existing CTFontRef (typically reported by the
  // shaper's font-cascade fallback) in a Face. The Face retains a strong
  // reference to the CTFont so the caller may release theirs after the
  // call. Used internally by the shaper to honour multi-font runs (text
  // mixed with emoji, CJK fallback, etc.).
  [[nodiscard]] std::unique_ptr<Face> make_face_from_ctfont(void* ct_font_ref, float pixel_size);

} // namespace fxe::font
