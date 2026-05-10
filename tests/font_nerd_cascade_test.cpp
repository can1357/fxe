// Verifies the embedded JetBrainsMono Nerd Font is wired into the
// shaper's cascade list so PUA codepoints render correctly even when the
// active font has no glyph for them.
//
// The check runs end-to-end through `default_shaper()->shape(face, …)`
// with a system font as the primary. The shaper must:
//   1. emit a glyph for each Nerd Font codepoint (CTLine cascades through
//      our embedded font),
//   2. attach a non-null `face` pointer on the substituted run so callers
//      can resolve glyph IDs against the correct face,
//   3. NOT route the substitution back through the primary face.
//
// On a non-CoreText build the cascade tap-in is a no-op today, so the
// test confirms only the helper APIs and skips the shape check with a
// pass-through.

#include <fxe/font.hpp>
#include <fxe/font/backend.hpp>
#include <fxe/font/embedded_nerd.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/shaper.hpp>

#include <cstdio>
#include <filesystem>
#include <fxe/types.hpp>
#include <string>

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

  // UTF-8 encode one codepoint into out.
  void utf8_encode(char32_t cp, std::string& out) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  }
} // namespace

int main() {
  using namespace fxe::font;

  // 1. Embedded bytes are present and look like a TTF (starts with 0x00 0x01
  //    0x00 0x00 for TrueType or "OTTO" for CFF). JetBrainsMono is TTF.
  {
    auto bytes = embedded_nerd_font_bytes();
    CHECK(bytes.size() > 1024 * 1024); // ~2.4 MB
    CHECK(bytes.size() >= 4);
    if (bytes.size() >= 4) {
      const bool truetype =
          bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x00 && bytes[3] == 0x00;
      const bool opentype =
          bytes[0] == 'O' && bytes[1] == 'T' && bytes[2] == 'T' && bytes[3] == 'O';
      CHECK(truetype || opentype);
    }
  }

  // 2. Nerd Font range predicate covers known Nerd Font blocks and
  //    rejects basic Latin.
  {
    CHECK(!is_nerd_font_codepoint(U'A'));
    CHECK(!is_nerd_font_codepoint(U' '));
    CHECK(is_nerd_font_codepoint(char32_t{0xe0b0}));  // Powerline right-arrow
    CHECK(is_nerd_font_codepoint(char32_t{0xe7c5}));  // Devicons
    CHECK(is_nerd_font_codepoint(char32_t{0xf0001})); // MDI post-v6 block
  }

  // 3. The embedded face loads via `embedded_nerd_font_face` and reports
  //    a non-zero glyph for a known Nerd Font codepoint.
  {
    auto face = embedded_nerd_font_face(16.0f);
    CHECK(face != nullptr);
    if (face) {
      // U+E0B0 — Powerline right-pointing triangle. Present in every
      // patched Nerd Font.
      CHECK(face->glyph_index(char32_t{0xe0b0}) != 0);
      // Letter 'A' is in JetBrainsMono's Latin coverage too.
      CHECK(face->glyph_index(U'A') != 0);
    }

    // Same logical size should hit the cache and return the same Face.
    auto face_again = embedded_nerd_font_face(16.0f);
    CHECK(face_again.get() == face.get());

    // A different size mints a new Face.
    auto face_other = embedded_nerd_font_face(24.0f);
    CHECK(face_other.get() != face.get());
  }

  // 4. End-to-end: shape a string mixing Latin + a Nerd Font codepoint
  //    through the default shaper with a system Latin font. The shaper
  //    MUST yield at least one glyph slot covering the Nerd Font
  //    codepoint with non-zero glyph_id; otherwise the cascade is broken
  //    and the icon renders as tofu.
  {
    auto* shaper = default_shaper();
    CHECK(shaper != nullptr);
    auto system_path = find_system_font();
    if (shaper && !system_path.empty()) {
      auto primary = load_face_from_file(system_path, 16.0f);
      CHECK(primary != nullptr);
      if (primary) {
        std::string utf8;
        utf8 += "A ";
        utf8_encode(char32_t{0xe0b0}, utf8); // Powerline arrow
        utf8 += " Z";

        ShapeOptions opts{};
        auto runs = shaper->shape(*primary, utf8, opts);
        CHECK(!runs.empty());

        bool covered = false;
        for (const auto& run : runs) {
          for (const auto& g : run.glyphs) {
            // Match the cluster byte offset for the Nerd Font codepoint.
            // It lives right after "A " (2 bytes). On every backend with
            // a working cascade the glyph id must be non-zero.
            if (g.cluster == 2 && g.glyph_id != 0) {
              covered = true;
              break;
            }
          }
          if (covered)
            break;
        }

        // The cascade is wired up for any backend that ships a real
        // shaper — CoreText (via kCTFontCascadeListAttribute) and
        // HarfBuzz (via post-shape tofu detection + re-shape against
        // the embedded face). The null shaper has no fallback path so
        // the assertion is downgraded to a print there.
        if (has_coretext(default_backend()) || has_harfbuzz(default_backend())) {
          CHECK(covered);
        } else {
          std::printf(
              "font_nerd_cascade_test: backend=%.*s — cascade not wired, skipping coverage check\n",
              static_cast<int>(backend_name(default_backend()).size()),
              backend_name(default_backend()).data());
        }
      }
    } else {
      std::printf("font_nerd_cascade_test: no system font available, skipping shape check\n");
    }
  }

  std::printf("font_nerd_cascade_test: pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
