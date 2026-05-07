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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

    std::vector<std::uint8_t> blob(8 * 8, 200);
    AtlasRegion r1 = mask.pack(8, 8, blob.data());
    CHECK(r1.ok);
    AtlasRegion r2 = mask.pack(16, 16, blob.data());
    CHECK(r2.ok);
    CHECK(r2.x != r1.x || r2.y != r1.y);

    // Force atlas growth by packing something larger than the initial size.
    std::vector<std::uint8_t> big(80 * 80, 1);
    AtlasRegion r3 = mask.pack(80, 80, big.data());
    CHECK(r3.ok);
    CHECK(mask.size().x >= 80);

    Atlas color{Format::bgra, 32, 1024};
    CHECK(color.bytes_per_pixel() == 4);
    std::vector<std::uint8_t> px(8 * 8 * 4, 0);
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

      const std::uint32_t gA = face->glyph_index(U'A');
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
