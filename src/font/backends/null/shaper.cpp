// No-op shaper, used only when neither HarfBuzz nor CoreText is available.
// Returns one glyph per UTF-8 byte using `Face::glyph_index` and the per-
// face advance reported by `render_glyph`. Crude, but better than crashing.

#include <fxe/font/face.hpp>
#include <fxe/font/shaper.hpp>

namespace fxe::font {
  namespace {
    char32_t decode_utf8(const char*& it, const char* end) noexcept {
      if (it >= end)
        return 0;
      const auto b0 = static_cast<unsigned char>(*it++);
      if (b0 < 0x80)
        return b0;
      char32_t cp = 0;
      int extra = 0;
      if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        extra = 1;
      } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        extra = 2;
      } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        extra = 3;
      } else {
        return 0xFFFD;
      }
      while (extra-- > 0 && it < end) {
        const auto b = static_cast<unsigned char>(*it++);
        if ((b & 0xC0) != 0x80)
          return 0xFFFD;
        cp = (cp << 6) | (b & 0x3F);
      }
      return cp;
    }

    class NoneShaper final : public Shaper {
    public:
      [[nodiscard]] std::vector<ShapeRun> shape(Face& face, std::string_view utf8,
                                                const ShapeOptions& opts) override {
        ShapeRun out{};
        out.direction = opts.direction;
        out.face = &face;
        const char* it = utf8.data();
        const char* end = it + utf8.size();
        while (it < end) {
          const auto byte_offset = static_cast<std::uint32_t>(it - utf8.data());
          const char32_t cp = decode_utf8(it, end);
          if (cp == 0)
            break;
          ShapedGlyph g{};
          g.glyph_id = face.glyph_index(cp);
          g.cluster = byte_offset;
          // Use em_advance as a coarse advance; real values would require
          // calling render_glyph here, which is too eager.
          g.x_advance = face.metrics().em_advance;
          out.glyphs.push_back(g);
          out.total_advance += g.x_advance;
        }
        std::vector<ShapeRun> runs;
        runs.push_back(std::move(out));
        return runs;
      }
    };
  } // namespace

  std::unique_ptr<Shaper> make_none_shaper() {
    return std::make_unique<NoneShaper>();
  }

} // namespace fxe::font
