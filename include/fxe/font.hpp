#pragma once

// Public umbrella header for the fxe font stack. Pulls in every public
// header so callers can `#include <fxe/font.hpp>` without listing each
// sub-header manually.

#include <cstddef>
#include <cstdint>
#include <memory>

#include <fxe/font/atlas.hpp>
#include <fxe/font/backend.hpp>
#include <fxe/font/collection.hpp>
#include <fxe/font/descriptor.hpp>
#include <fxe/font/discover.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/feature.hpp>
#include <fxe/font/glyph.hpp>
#include <fxe/font/library.hpp>
#include <fxe/font/shaper.hpp>

namespace fxe::font {

  // Glyph cache: maps (face, glyph_id, size, hint, subpixel) → Glyph and
  // owns the two atlas pages. Renderers read the pages from here each frame.

  struct GlyphCacheBudget {
    std::uint32_t initial_atlas_size = 256;
    std::uint32_t max_atlas_size = 8192;
    std::size_t max_mask_glyph_count = 4096;
    std::size_t max_color_glyph_count = 1024;
    std::size_t max_mask_atlas_bytes = 16ull * 1024ull * 1024ull;
    std::size_t max_color_atlas_bytes = 16ull * 1024ull * 1024ull;
  };
  class GlyphCache {
  public:
    GlyphCache();
    explicit GlyphCache(GlyphCacheBudget budget);
    ~GlyphCache();
    GlyphCache(const GlyphCache&) = delete;
    GlyphCache& operator=(const GlyphCache&) = delete;

    [[nodiscard]] Atlas& mask_atlas() noexcept;
    [[nodiscard]] Atlas& color_atlas() noexcept;
    [[nodiscard]] const Atlas& mask_atlas() const noexcept;
    [[nodiscard]] const Atlas& color_atlas() const noexcept;

    // Looks up or rasterizes a glyph. `subpixel_x` is in [0, 1), quantised
    // to four bins inside.
    [[nodiscard]] const Glyph& lookup(Face& face, std::uint32_t glyph_id, float subpixel_x = 0.0f,
                                      Hint hint = Hint::full);

    void set_budget(GlyphCacheBudget budget);
    [[nodiscard]] std::size_t cache_size(Format format) const noexcept;
    [[nodiscard]] std::size_t eviction_count(Format format) const noexcept;
    [[nodiscard]] std::size_t atlas_bytes(Format format) const noexcept;
    [[nodiscard]] std::uint64_t generation(Format format) const noexcept;
    [[nodiscard]] bool debug_contains(const GlyphKey& key) const noexcept;

    // Drops every cached glyph. Used by tests; not needed in steady state.
    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

  // Process-wide glyph cache singleton. Owns the two atlas pages renderers
  // upload each frame, and is shared between the text path (which feeds
  // glyphs in) and the GPU backend (which copies pixels out).
  [[nodiscard]] GlyphCache& shared_glyph_cache();

  // Process-wide font collection used by the default text path. Lazy-loaded
  // on first call: the regular face is the result of running `default_discover()`
  // for the platform's preferred system font, plus any color-emoji fallback
  // the platform exposes.
  [[nodiscard]] Collection& shared_collection();

} // namespace fxe::font

namespace fxe::font {
  // Process-wide device-pixel-ratio set by the active renderer at the start
  // of each frame. The text path multiplies the requested point size by
  // this value when looking up a face so glyphs are rasterised at the
  // physical framebuffer resolution. Defaults to 1.0 when no renderer has
  // published a value yet.
  void set_device_pixel_ratio(float dpr) noexcept;
  [[nodiscard]] float device_pixel_ratio() noexcept;
} // namespace fxe::font
