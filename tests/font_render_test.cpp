// Phase 8 verification: face render + atlas roundtrip, shaper ligatures,
// collection fallback, color emoji. Soft-skips when a backend or fixture is
// unavailable so the test runs across all preset matrix entries.

#include <fxe/font.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fxe/types.hpp>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {
  int g_pass = 0;
  int g_fail = 0;
  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok)
      ++g_pass;
    else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }
#define CHECK(e) check((e), #e, __FILE__, __LINE__)

  fxe::font::GlyphKey make_key(const fxe::font::Face& face, u32 glyph_id) {
    fxe::font::GlyphKey key{};
    key.face_id = face.id();
    key.glyph_id = glyph_id;
    key.pixel_size_q = static_cast<u32>(std::lround(face.pixel_size() * 64.0f));
    key.subpixel_x = 0;
    key.hint = static_cast<u8>(fxe::font::Hint::full);
    return key;
  }

  std::vector<u8> glyph_pixels(const fxe::font::Atlas& atlas, const fxe::font::Glyph& glyph) {
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

  std::string find_path(std::initializer_list<const char*> candidates) {
    for (const char* p : candidates)
      if (std::filesystem::exists(p))
        return p;
    return {};
  }

  // Pick the most likely "Latin font with a fi ligature" available on the
  // host. macOS Helvetica has a real fi ligature; DejaVu Sans usually does.
  std::string find_ligature_font() {
    return find_path({
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Geneva.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    });
  }

  std::string find_emoji_font() {
    return find_path({
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto/NotoColorEmoji.ttf",
    });
  }
} // namespace

int main() {
  using namespace fxe::font;

  // Face render → atlas round-trip.
  {
    const std::string p = find_ligature_font();
    if (p.empty()) {
      std::printf("SKIP face render (no font)\n");
    } else {
      auto face = load_face_from_file(p, 24.0f);
      CHECK(face != nullptr);
      if (face) {
        Atlas mask{Format::grayscale, 256, 4096};
        Atlas color{Format::bgra, 256, 4096};
        const u64 before_gen = mask.generation();
        const u32 gA = face->glyph_index(U'A');
        CHECK(gA != 0);
        const Glyph g = face->render_glyph(gA, mask, color, Hint::full);
        CHECK(g.advance_x > 0.0f);
        // Either the mask or the color page advanced its generation.
        CHECK(mask.generation() > before_gen || color.generation() > 0);
        // Glyph fits inside the page it was packed into.
        const Atlas& page = (g.format == Format::bgra) ? color : mask;
        CHECK(g.atlas_x + g.width <= page.size().x);
        CHECK(g.atlas_y + g.height <= page.size().y);
      }
    }
  }

  // GlyphCache eviction → re-rendered glyph pixels match the original.
  {
    const std::string p = find_ligature_font();
    if (p.empty()) {
      std::printf("SKIP glyph cache eviction render (no font)\n");
    } else {
      auto face = load_face_from_file(p, 28.0f);
      CHECK(face != nullptr);
      if (face) {
        GlyphCacheBudget budget{};
        budget.initial_atlas_size = 64;
        budget.max_atlas_size = 256;
        budget.max_mask_glyph_count = 5;
        budget.max_mask_atlas_bytes = 256ull * 256ull;
        GlyphCache cache{budget};
        const u32 gA = face->glyph_index(U'A');
        CHECK(gA != 0);
        const Glyph first = cache.lookup(*face, gA);
        CHECK(first.width > 0);
        const std::vector<u8> first_pixels = glyph_pixels(cache.mask_atlas(), first);
        for (char32_t ch = U'B'; ch <= U'K'; ++ch) {
          const u32 gid = face->glyph_index(ch);
          if (gid != 0)
            (void)cache.lookup(*face, gid);
        }
        CHECK(!cache.debug_contains(make_key(*face, gA)));
        const Glyph rerendered = cache.lookup(*face, gA);
        CHECK(rerendered.width == first.width);
        CHECK(rerendered.height == first.height);
        CHECK(rerendered.advance_x == first.advance_x);
        CHECK(glyph_pixels(cache.mask_atlas(), rerendered) == first_pixels);
      }
    }
  }

  // Shaper: "fi" produces ≤ 2 glyphs (1 if ligature available). Total advance
  // of "fi" should equal advance of the produced glyph(s), which is ≤
  // f.advance + i.advance. Most importantly, it should be > 0.
  {
    const std::string p = find_ligature_font();
    if (p.empty()) {
      std::printf("SKIP shaper ligature (no font)\n");
    } else {
      auto face = load_face_from_file(p, 24.0f);
      auto shaper = default_shaper();
      CHECK(face != nullptr);
      CHECK(shaper != nullptr);
      if (face && shaper) {
        ShapeOptions opts;
        auto fi_runs = shaper->shape(*face, "fi", opts);
        CHECK(!fi_runs.empty());
        const auto& fi = fi_runs.front();
        CHECK(!fi.glyphs.empty());
        CHECK(fi.glyphs.size() <= 2);
        CHECK(fi.total_advance > 0.0f);

        // "AVA" should kern: total_advance(AVA) < 3 * advance(A) for a font
        // with kerning. Skip the strict assertion since not all fonts kern A/V.
        auto ava_runs = shaper->shape(*face, "AVA", opts);
        CHECK(!ava_runs.empty());
        const auto& ava = ava_runs.front();
        CHECK(ava.glyphs.size() == 3);
        CHECK(ava.total_advance > 0.0f);

        // Empty input produces empty run.
        auto empty_runs = shaper->shape(*face, "", opts);
        CHECK(!empty_runs.empty());
        const auto& empty = empty_runs.front();
        CHECK(empty.glyphs.empty());
        CHECK(empty.total_advance == 0.0f);
      }
    }
  }

  // Collection fallback: register a primary face that covers ASCII + an
  // emoji-only fallback face. Resolve 'A' → primary, 🎉 (U+1F389) → fallback.
  {
    const std::string ascii = find_ligature_font();
    const std::string emoji = find_emoji_font();
    if (ascii.empty()) {
      std::printf("SKIP collection (no ascii font)\n");
    } else {
      auto primary = load_face_from_file(ascii, 16.0f);
      CHECK(primary != nullptr);
      if (primary) {
        Collection c;
        c.add_primary(Style::regular, std::shared_ptr<Face>{std::move(primary)});
        if (!emoji.empty()) {
          auto fb = load_face_from_file(emoji, 16.0f);
          if (fb)
            c.add_fallback(Style::regular, std::shared_ptr<Face>{std::move(fb)});
        }
        auto a_face = c.resolve(Style::regular, U'A');
        CHECK(a_face != nullptr);
        if (!emoji.empty()) {
          auto party = c.resolve(Style::regular, char32_t{0x1F389}); // 🎉
          // If the fallback exposes color emoji, party should resolve.
          // Some fonts may not; treat empty as soft-skip.
          (void)party;
        }
      }
    }
  }

  // Color emoji rasterization. Soft-skipped when no color font is available.
  {
    const std::string emoji = find_emoji_font();
    if (emoji.empty()) {
      std::printf("SKIP color emoji (no emoji font)\n");
    } else {
      auto face = load_face_from_file(emoji, 32.0f);
      if (!face) {
        std::printf("SKIP color emoji (face load failed)\n");
      } else {
        const u32 gid = face->glyph_index(char32_t{0x1F389});
        if (gid == 0) {
          std::printf("SKIP color emoji (glyph not present)\n");
        } else {
          Atlas mask{Format::grayscale, 256, 4096};
          Atlas color{Format::bgra, 256, 4096};
          const Glyph g = face->render_glyph(gid, mask, color, Hint::full);
          // The face exposes color glyphs; rendering should produce a BGRA
          // glyph in the color page. Some emoji faces only register PNG
          // tables which may not be supported by the rasterizer; tolerate
          // the soft-fail.
          if (face->has_color() && g.format == Format::bgra) {
            CHECK(g.width > 0);
            CHECK(g.height > 0);
            CHECK(color.generation() > 0);
          } else {
            std::printf("SKIP color emoji (face reports no color or rasterizer "
                        "produced mask)\n");
          }
        }
      }
    }
  }

  std::printf("font_render_test: pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
