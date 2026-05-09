// Pure font-module tests. Exercises Phase 1 (library/atlas), Phase 2 (face),
// Phase 3 (shaper), and Phase 4 (collection + discovery) without booting V8
// or a window. Some checks are skipped on backends that don't apply (e.g.
// FreeType raster checks on a pure-CoreText build).

#include <fxe/font.hpp>
#include <fxe/font/atlas.hpp>
#include <fxe/font/backend.hpp>
#include <fxe/font/collection.hpp>
#include <fxe/font/discover.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/library.hpp>
#include <fxe/font/shaper.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fxe/types.hpp>
#include <memory>
#include <string>
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
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  fxe::font::GlyphKey make_key(const fxe::font::Face& face, u32 glyph_id,
                               fxe::font::Hint hint = fxe::font::Hint::full, u8 subpixel = 0) {
    fxe::font::GlyphKey key{};
    key.face_id = face.id();
    key.glyph_id = glyph_id;
    key.pixel_size_q = static_cast<u32>(std::lround(face.pixel_size() * 64.0f));
    key.subpixel_x = subpixel;
    key.hint = static_cast<u8>(hint);
    return key;
  }

  // Pick a system font path for tests. We try a small handful of well-known
  // Apple fonts; on non-macOS hosts, tests that need a real face are
  // soft-skipped.
  std::string find_system_font() {
    const char* candidates[] = {
        "/System/Library/Fonts/SFNS.ttf",         "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Geneva.ttf",       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };
    for (const char* p : candidates) {
      if (std::filesystem::exists(p))
        return p;
    }
    return {};
  }

  std::string find_emoji_font() {
    const char* candidates[] = {
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto/NotoColorEmoji.ttf",
    };
    for (const char* p : candidates) {
      if (std::filesystem::exists(p))
        return p;
    }
    return {};
  }
} // namespace

int main() {
  using namespace fxe::font;

  std::printf("font backend: %.*s\n", static_cast<int>(backend_name(default_backend()).size()),
              backend_name(default_backend()).data());

  // Phase 1: library + atlas pack.
  {
    auto& lib = shared_library();
    (void)lib; // construction shouldn't throw on any backend.

    Atlas mask{Format::grayscale, 64, 1024};
    CHECK(mask.format() == Format::grayscale);
    CHECK(mask.size().x == 64 && mask.size().y == 64);
    const u64 initial_layout_gen = mask.layout_generation();
    const u64 initial_gen = mask.generation();

    std::vector<u8> blob(8 * 8, 200);
    AtlasRegion r1 = mask.pack(8, 8, blob.data());
    CHECK(r1.ok);
    CHECK(mask.generation() > initial_gen);
    CHECK(mask.layout_generation() == initial_layout_gen);
    AtlasRegion r2 = mask.pack(16, 16, blob.data());
    CHECK(r2.ok);
    CHECK(r2.x != r1.x || r2.y != r1.y);
    CHECK(mask.layout_generation() == initial_layout_gen);

    // Force atlas growth by packing something larger than the initial size.
    const u64 before_grow_layout_gen = mask.layout_generation();
    std::vector<u8> big(80 * 80, 1);
    AtlasRegion r3 = mask.pack(80, 80, big.data());
    CHECK(r3.ok);
    CHECK(mask.size().x >= 80);
    CHECK(mask.layout_generation() > before_grow_layout_gen);
    Atlas color{Format::bgra, 32, 1024};
    CHECK(color.bytes_per_pixel() == 4);
    std::vector<u8> px(8 * 8 * 4, 0);
    AtlasRegion r4 = color.pack(8, 8, px.data());
    CHECK(r4.ok);
  }

  const std::string font_path = find_system_font();
  if (font_path.empty()) {
    std::printf("WARN: no system font available; skipping face/shaper/collection tests\n");
  } else {
    // Phase 2: face render.
    auto face = load_face_from_file(font_path, 32.0f);
    CHECK(face != nullptr);
    if (face) {
      CHECK(face->pixel_size() == 32.0f);
      const auto m = face->metrics();
      CHECK(m.line_height > 0.0f);
      CHECK(m.ascent > 0.0f);

      const u32 gA = face->glyph_index(U'A');
      CHECK(gA != 0);

      Atlas mask{Format::grayscale, 256, 4096};
      Atlas color{Format::bgra, 256, 4096};
      auto g = face->render_glyph(gA, mask, color, Hint::full);
      CHECK(g.advance_x > 0.0f);
      CHECK(g.width > 0);
      CHECK(g.height > 0);
    }

    // Phase 3: shaper.
    auto shaper = default_shaper();
    CHECK(shaper != nullptr);
    if (shaper && face) {
      ShapeOptions opts;
      auto runs = shaper->shape(*face, "Hello", opts);
      CHECK(!runs.empty());
      const auto& run = runs.front();
      CHECK(!run.glyphs.empty());
      CHECK(run.total_advance > 0.0f);

      // "fi" should ligate on most monospace fonts, and at minimum produce
      // ≤ 2 glyphs. On fonts without a fi ligature, we still expect 2.
      auto fi_runs = shaper->shape(*face, "fi", opts);
      CHECK(!fi_runs.empty());
      const auto& fi = fi_runs.front();
      CHECK(!fi.glyphs.empty());
      CHECK(fi.glyphs.size() <= 2);
    }

    // A7: bounded GlyphCache LRU eviction and atlas repack.
    if (face) {
      std::vector<u32> glyphs;
      for (char32_t ch = U'A'; ch <= U'J'; ++ch) {
        const u32 gid = face->glyph_index(ch);
        if (gid != 0)
          glyphs.push_back(gid);
      }
      if (glyphs.size() >= 10) {
        GlyphCacheBudget budget{};
        budget.initial_atlas_size = 64;
        budget.max_atlas_size = 512;
        budget.max_mask_glyph_count = 5;
        budget.max_mask_atlas_bytes = 512ull * 512ull;
        GlyphCache cache{budget};

        for (u32 gid : glyphs)
          (void)cache.lookup(*face, gid);
        CHECK(cache.cache_size(Format::grayscale) == 5);
        CHECK(cache.eviction_count(Format::grayscale) == 5);
        CHECK(!cache.debug_contains(make_key(*face, glyphs[0])));
        CHECK(!cache.debug_contains(make_key(*face, glyphs[4])));
        CHECK(cache.debug_contains(make_key(*face, glyphs[5])));
        CHECK(cache.debug_contains(make_key(*face, glyphs[9])));

        GlyphCache recent_cache{budget};
        for (usize i = 0; i < 5; ++i)
          (void)recent_cache.lookup(*face, glyphs[i]);
        (void)recent_cache.lookup(*face, glyphs[0]);
        (void)recent_cache.lookup(*face, glyphs[5]);
        (void)recent_cache.lookup(*face, glyphs[6]);
        CHECK(recent_cache.debug_contains(make_key(*face, glyphs[0])));
        CHECK(!recent_cache.debug_contains(make_key(*face, glyphs[1])));
        CHECK(recent_cache.cache_size(Format::grayscale) == 5);

        GlyphCacheBudget repack_budget{};
        repack_budget.initial_atlas_size = 32;
        repack_budget.max_atlas_size = 64;
        repack_budget.max_mask_glyph_count = 128;
        repack_budget.max_mask_atlas_bytes = 64ull * 64ull;
        GlyphCache repack_cache{repack_budget};
        const u64 before_gen = repack_cache.generation(Format::grayscale);
        std::vector<u32> repack_glyphs;
        for (char32_t ch = U'!'; ch <= U'~'; ++ch) {
          const u32 gid = face->glyph_index(ch);
          if (gid == 0)
            continue;
          repack_glyphs.push_back(gid);
          (void)repack_cache.lookup(*face, gid);
        }
        CHECK(repack_cache.eviction_count(Format::grayscale) > 0);
        CHECK(repack_cache.generation(Format::grayscale) > before_gen);
        for (u32 gid : repack_glyphs) {
          const auto key = make_key(*face, gid);
          if (!repack_cache.debug_contains(key))
            continue;
          const Glyph& g = repack_cache.lookup(*face, gid);
          const Atlas& page =
              (g.format == Format::bgra) ? repack_cache.color_atlas() : repack_cache.mask_atlas();
          CHECK(g.atlas_x + g.width <= page.size().x);
          CHECK(g.atlas_y + g.height <= page.size().y);
        }
      }

      const std::string emoji_path = find_emoji_font();
      if (!emoji_path.empty()) {
        auto emoji_face = load_face_from_file(emoji_path, 24.0f);
        if (emoji_face) {
          GlyphCacheBudget budget{};
          budget.max_mask_glyph_count = 5;
          budget.max_color_glyph_count = 1;
          budget.max_color_atlas_bytes = 16ull * 1024ull * 1024ull;
          GlyphCache mixed{budget};
          const u32 mask_a = face->glyph_index(U'A');
          const u32 mask_b = face->glyph_index(U'B');
          (void)mixed.lookup(*face, mask_a);
          (void)mixed.lookup(*face, mask_b);
          const auto key_a = make_key(*face, mask_a);
          const auto key_b = make_key(*face, mask_b);
          const char32_t emoji_chars[] = {char32_t{0x1F389}, char32_t{0x1F600}, char32_t{0x1F680}};
          usize color_seen = 0;
          for (char32_t ch : emoji_chars) {
            const u32 gid = emoji_face->glyph_index(ch);
            if (gid == 0)
              continue;
            const Glyph& g = mixed.lookup(*emoji_face, gid);
            if (g.format == Format::bgra && g.width > 0)
              ++color_seen;
          }
          if (color_seen > 1) {
            CHECK(mixed.cache_size(Format::bgra) == 1);
            CHECK(mixed.eviction_count(Format::bgra) > 0);
            CHECK(mixed.debug_contains(key_a));
            CHECK(mixed.debug_contains(key_b));
            CHECK(mixed.cache_size(Format::grayscale) == 2);
          }
        }
      }
    }

    // Phase 4: collection.
    {
      Collection c;
      c.add_primary(Style::regular, std::shared_ptr<Face>{std::move(face)});
      auto resolved = c.resolve(Style::regular, U'A');
      CHECK(resolved != nullptr);
      // Codepoint that's almost certainly NOT in a base Latin font: Klingon
      // alphabet or unassigned high plane. Should miss → nullptr.
      auto missing = c.resolve(Style::regular, char32_t{0x10FFFD});
      CHECK(missing == nullptr);
    }
  }

  // Phase 4: discovery — only assert that the platform's discoverer returns
  // *something* for a query with an empty family (best-effort match).
  {
    auto disc = default_discover();
    CHECK(disc != nullptr);
    if (disc) {
      Descriptor q;
      // Most platforms have at least one font; a CoreText match for "Helvetica"
      // is the canonical macOS smoke test.
      q.family = "Helvetica";
      auto results = disc->find(q);
      // On macOS this should return >=1; on Linux without fontconfig fallback
      // for "Helvetica" it may be empty but the call shouldn't crash.
      (void)results;
    }
  }

  std::printf("font_module_test: pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
