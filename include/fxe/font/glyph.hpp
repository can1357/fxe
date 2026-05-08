#pragma once

// Per-glyph atlas record. Mirrors Ghostty's `Glyph.zig` shape — a small POD
// describing where a rasterized glyph lives in the atlas and what offsets to
// apply when stamping it onto the page. The atlas itself is `font::Atlas`.

#include <cstdint>

#include <fxe/math.hpp>
#include <fxe/types.hpp>

namespace fxe::font {

  // Pixel format of an atlas page / a glyph bitmap.
  enum class Format : u8 {
    grayscale, // 1-channel alpha mask (FT_PIXEL_MODE_GRAY) — most non-color glyphs
    bgra,      // 4-channel premultiplied BGRA — color emoji (CBDT, sbix, COLR)
  };

  // Cached glyph record. `width`/`height` are pixel dimensions of the bitmap
  // in the atlas; `offset_x`/`offset_y` are the bearing from the pen origin
  // to the top-left corner of the bitmap (positive y = down). `advance_x` is
  // the per-glyph advance in pixels.
  struct Glyph {
    u32 atlas_x = 0;
    u32 atlas_y = 0;
    u32 width = 0;
    u32 height = 0;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float advance_x = 0.0f;
    Format format = Format::grayscale;
  };

  // Cache key for glyph_cache. Includes a quantised subpixel bin (0..3) so we
  // can render four versions of every glyph offset by 0/¼/½/¾ px without
  // re-rendering on every pen jump.
  struct GlyphKey {
    u64 face_id = 0;
    u32 glyph_id = 0;
    u32 pixel_size_q = 0; // pixel-height * 64
    u8 subpixel_x = 0;    // 0..3
    u8 hint = 1;          // 0 = no hint, 1 = light, 2 = full

    friend constexpr bool operator==(const GlyphKey&, const GlyphKey&) noexcept = default;
  };

  struct GlyphKeyHash {
    [[nodiscard]] usize operator()(const GlyphKey& k) const noexcept {
      // 64-bit fnv-1a over the packed key. Cheap and good-enough.
      const auto mix = [](u64 h, u64 v) -> u64 {
        h ^= v;
        h *= 1099511628211ull;
        return h;
      };
      u64 h = 14695981039346656037ull;
      h = mix(h, k.face_id);
      h = mix(h, static_cast<u64>(k.glyph_id));
      h = mix(h, static_cast<u64>(k.pixel_size_q));
      h = mix(h, static_cast<u64>((u32{k.subpixel_x} << 8) | k.hint));
      return static_cast<usize>(h);
    }
  };

} // namespace fxe::font
