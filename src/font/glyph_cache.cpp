// (face_id, glyph_id, size, hint, sub_x_bin) → Glyph cache + the two atlas
// pages. Owned by `font::GlyphCache`. The cache stores the rendered glyph
// records by value; the actual pixels live in `mask_atlas_` / `color_atlas_`.

#include <fxe/font.hpp>
#include <fxe/font/atlas.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/glyph.hpp>

#include <cmath>
#include <unordered_map>

namespace fxe::font {

  struct GlyphCache::Impl {
    Atlas mask{Format::grayscale, 256, 8192};
    Atlas color{Format::bgra, 256, 8192};
    std::unordered_map<GlyphKey, Glyph, GlyphKeyHash> cache;
    Glyph empty{};
  };

  GlyphCache::GlyphCache() : impl_(std::make_unique<Impl>()) {}
  GlyphCache::~GlyphCache() = default;

  Atlas& GlyphCache::mask_atlas() noexcept {
    return impl_->mask;
  }
  Atlas& GlyphCache::color_atlas() noexcept {
    return impl_->color;
  }
  const Atlas& GlyphCache::mask_atlas() const noexcept {
    return impl_->mask;
  }
  const Atlas& GlyphCache::color_atlas() const noexcept {
    return impl_->color;
  }

  const Glyph& GlyphCache::lookup(Face& face, std::uint32_t glyph_id, float subpixel_x, Hint hint) {
    // Quantise subpixel x into 4 bins. 0 → integer pen position; 1 → ¼ px,
    // etc. Callers asking for `subpixel_x` outside [0, 1) get clamped.
    if (!std::isfinite(subpixel_x) || subpixel_x < 0.0f)
      subpixel_x = 0.0f;
    if (subpixel_x >= 1.0f)
      subpixel_x -= std::floor(subpixel_x);
    const std::uint8_t sub_bin = static_cast<std::uint8_t>(subpixel_x * 4.0f) & 0x3;
    // Snap the actual sub-pixel offset we hand to the rasteriser to the
    // centre of the chosen bin so each cache entry is rendered at a
    // consistent fractional position regardless of which fractional input
    // landed in it.
    const float bin_subpixel = static_cast<float>(sub_bin) * 0.25f;

    GlyphKey k{};
    k.face_id = face.id();
    k.glyph_id = glyph_id;
    k.pixel_size_q = static_cast<std::uint32_t>(std::lround(face.pixel_size() * 64.0f));
    k.subpixel_x = sub_bin;
    k.hint = static_cast<std::uint8_t>(hint);

    if (auto it = impl_->cache.find(k); it != impl_->cache.end())
      return it->second;

    Glyph g = face.render_glyph(glyph_id, impl_->mask, impl_->color, hint, bin_subpixel);
    auto [it, _] = impl_->cache.emplace(k, g);
    return it->second;
  }

  void GlyphCache::clear() {
    impl_->cache.clear();
    impl_->mask.clear();
    impl_->color.clear();
  }

} // namespace fxe::font
