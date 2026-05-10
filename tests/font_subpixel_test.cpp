// Regression for TODO T1: "Subpixel glyph positioning verification".
//
// Pins three invariants that drive crisp text:
//   1. The active shaper produces fractional (non-integer-rounded) advances
//      so cumulative pen positions land on sub-pixel boundaries instead of
//      collapsing into the nearest whole pixel.
//   2. `GlyphCache::lookup` quantises subpixel x into 4 distinct bins
//      (subpixel_x ∈ {0, 0.25, 0.5, 0.75}) and yields up to four physically
//      different cache entries for the same glyph.
//   3. The face backend honours `subpixel_x` in `render_glyph(...)`: the
//      rasterised pixels for two adjacent bins differ.
//
// Soft-skips when no font fixture is available (CI matrix entries without
// system fonts).

#include <fxe/font.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fxe/types.hpp>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }
#define CHECK(e) check((e), #e, __FILE__, __LINE__)

  std::string find_path(std::initializer_list<const char*> candidates) {
    for (const char* p : candidates)
      if (std::filesystem::exists(p))
        return p;
    return {};
  }

  std::string find_latin_font() {
    return find_path({
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Geneva.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
    });
  }

  std::vector<u8> bin_pixels(const fxe::font::Atlas& atlas, const fxe::font::Glyph& glyph) {
    const auto size = atlas.size();
    const auto bpp = static_cast<usize>(atlas.bytes_per_pixel());
    std::vector<u8> out(static_cast<usize>(glyph.width) * glyph.height * bpp);
    for (u32 y = 0; y < glyph.height; ++y) {
      const u8* src = atlas.pixels().data() +
                      (static_cast<usize>(glyph.atlas_y + y) * size.x + glyph.atlas_x) * bpp;
      std::copy_n(src, static_cast<usize>(glyph.width) * bpp,
                  out.data() + static_cast<usize>(y) * glyph.width * bpp);
    }
    return out;
  }
} // namespace

int main() {
  using namespace fxe::font;

  const std::string p = find_latin_font();
  if (p.empty()) {
    std::printf("SKIP font_subpixel_test (no font)\n");
    return 0;
  }

  // 1. Shaper produces fractional advances.
  {
    auto face = load_face_from_file(p, 24.0f);
    auto shaper = default_shaper();
    CHECK(face != nullptr);
    CHECK(shaper != nullptr);
    if (face && shaper) {
      ShapeOptions opts;
      // A long sequence of mixed-width glyphs gives many chances for the
      // accumulated pen to land between integer pixels.
      auto runs = shaper->shape(*face, "The quick brown fox jumps over the lazy dog", opts);
      CHECK(!runs.empty());
      bool any_fractional = false;
      float pen_x = 0.0f;
      for (const auto& run : runs) {
        for (const auto& g : run.glyphs) {
          // Per-glyph fractional advance.
          if (std::abs(g.x_advance - std::nearbyint(g.x_advance)) > 1.0f / 64.0f) {
            any_fractional = true;
          }
          pen_x += g.x_advance;
          // Cumulative pen position fractional.
          if (std::abs(pen_x - std::nearbyint(pen_x)) > 1.0f / 64.0f) {
            any_fractional = true;
          }
        }
      }
      CHECK(any_fractional);
    }
  }

  // 2. GlyphCache quantises into 4 subpixel bins.
  {
    auto face = load_face_from_file(p, 24.0f);
    CHECK(face != nullptr);
    if (face) {
      GlyphCache cache;
      const u32 gA = face->glyph_index(U'A');
      CHECK(gA != 0);
      if (gA != 0) {
        // Look up the same glyph at subpixel offsets that quantise into
        // each of the four bins (0, ¼, ½, ¾ px). The cache should produce
        // four entries (atlas-distinct or pixel-distinct).
        const Glyph b0 = cache.lookup(*face, gA, 0.0f);
        const Glyph b1 = cache.lookup(*face, gA, 0.25f);
        const Glyph b2 = cache.lookup(*face, gA, 0.5f);
        const Glyph b3 = cache.lookup(*face, gA, 0.75f);
        CHECK(b0.width > 0);
        CHECK(b1.width > 0);
        CHECK(b2.width > 0);
        CHECK(b3.width > 0);

        // Sub-pixel cache entries each occupy a distinct atlas slot —
        // otherwise the cache collapsed all four bins back to a single
        // bitmap (= no sub-pixel positioning).
        const std::array<std::pair<u32, u32>, 4> slots{{
            {b0.atlas_x, b0.atlas_y},
            {b1.atlas_x, b1.atlas_y},
            {b2.atlas_x, b2.atlas_y},
            {b3.atlas_x, b3.atlas_y},
        }};
        usize unique_slots = 0;
        for (usize i = 0; i < slots.size(); ++i) {
          bool seen = false;
          for (usize j = 0; j < i; ++j)
            if (slots[i] == slots[j])
              seen = true;
          if (!seen)
            ++unique_slots;
        }
        // Every backend in the project advertises sub-pixel positioning;
        // we expect at least 3 distinct slots (allow 1 collapse for very
        // narrow / monochrome glyphs at small sizes).
        CHECK(unique_slots >= 3);

        // Repeated lookups at the same fractional offset are stable
        // (cache hit, same atlas slot).
        const Glyph b0_again = cache.lookup(*face, gA, 0.05f); // bin 0
        CHECK(b0_again.atlas_x == b0.atlas_x);
        CHECK(b0_again.atlas_y == b0.atlas_y);
        const Glyph b3_again = cache.lookup(*face, gA, 0.95f); // bin 3
        CHECK(b3_again.atlas_x == b3.atlas_x);
        CHECK(b3_again.atlas_y == b3.atlas_y);
      }
    }
  }

  // 3. Face backend honours `subpixel_x`: bin-0 and bin-2 bitmaps differ.
  {
    auto face = load_face_from_file(p, 24.0f);
    CHECK(face != nullptr);
    if (face) {
      const u32 gA = face->glyph_index(U'A');
      CHECK(gA != 0);
      if (gA != 0) {
        Atlas mask0{Format::grayscale, 128, 1024};
        Atlas color0{Format::bgra, 128, 1024};
        Atlas mask2{Format::grayscale, 128, 1024};
        Atlas color2{Format::bgra, 128, 1024};
        const Glyph g0 = face->render_glyph(gA, mask0, color0, Hint::full, 0.0f);
        const Glyph g2 = face->render_glyph(gA, mask2, color2, Hint::full, 0.5f);
        CHECK(g0.width > 0);
        CHECK(g2.width > 0);

        const Atlas& page0 = (g0.format == Format::bgra) ? color0 : mask0;
        const Atlas& page2 = (g2.format == Format::bgra) ? color2 : mask2;
        const auto px0 = bin_pixels(page0, g0);
        const auto px2 = bin_pixels(page2, g2);

        // If pixel layouts match between bin 0 and bin 2, the backend
        // dropped the fractional offset (sub-pixel positioning broken).
        if (g0.width == g2.width && g0.height == g2.height) {
          CHECK(px0 != px2);
        } else {
          // A different bbox between bins is itself proof the offset moved.
          CHECK(true);
        }
      }
    }
  }

  std::printf("font_subpixel_test: pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
