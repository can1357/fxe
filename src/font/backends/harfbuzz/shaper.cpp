// HarfBuzz-backed Shaper. Pairs naturally with the FreeType face backend
// (which exposes an `hb_font_t` for free); falls back to deriving an
// `hb_font_t` from raw FT_Face when needed.
//
// Nerd Font cascade: HarfBuzz does NOT do font cascade itself — when the
// primary face has no glyph for a codepoint it emits glyph_id == 0
// (.notdef / tofu). After the primary shape we scan for these tofu glyphs
// whose codepoint falls in a Nerd Font block, re-shape just that cluster
// with the embedded Nerd Font face (loaded at the same pixel size), and
// splice the resulting glyphs in as a separate ShapeRun. The result
// matches the CoreText behaviour — apps using any HarfBuzz-driven backend
// (FreeType/Linux/Windows) get free icon coverage too.

#include <fxe/font/embedded_nerd.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/shaper.hpp>

#include <hb-ft.h>
#include <hb.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace fxe::font {

  // Provided by face_freetype.cpp.
  hb_font_t* face_freetype_hb_font(Face& f);

  namespace {

    hb_direction_t to_hb_dir(Direction d) noexcept {
      return d == Direction::rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
    }

    hb_script_t to_hb_script(std::string_view s) noexcept {
      if (s.size() != 4)
        return HB_SCRIPT_UNKNOWN;
      return hb_script_from_string(s.data(), 4);
    }

    hb_language_t to_hb_lang(std::string_view s) noexcept {
      if (s.empty())
        return hb_language_get_default();
      return hb_language_from_string(s.data(), static_cast<int>(s.size()));
    }

    // Decode the UTF-8 codepoint starting at offset, returning the
    // codepoint and the number of bytes consumed. On malformed input
    // returns (U+FFFD, 1) so the caller can advance past the bad byte.
    struct Utf8Decoded {
      char32_t cp;
      usize bytes;
    };

    Utf8Decoded decode_utf8_at(std::string_view utf8, usize offset) noexcept {
      if (offset >= utf8.size())
        return {0xfffd, 0};
      const u8 b0 = static_cast<u8>(utf8[offset]);
      auto cont = [&](usize i) -> int {
        if (offset + i >= utf8.size())
          return -1;
        const u8 b = static_cast<u8>(utf8[offset + i]);
        if ((b & 0xc0) != 0x80)
          return -1;
        return b & 0x3f;
      };
      if (b0 < 0x80)
        return {b0, 1};
      if ((b0 & 0xe0) == 0xc0) {
        int c1 = cont(1);
        if (c1 < 0)
          return {0xfffd, 1};
        return {static_cast<char32_t>(((b0 & 0x1f) << 6) | c1), 2};
      }
      if ((b0 & 0xf0) == 0xe0) {
        int c1 = cont(1), c2 = cont(2);
        if (c1 < 0 || c2 < 0)
          return {0xfffd, 1};
        return {static_cast<char32_t>(((b0 & 0x0f) << 12) | (c1 << 6) | c2), 3};
      }
      if ((b0 & 0xf8) == 0xf0) {
        int c1 = cont(1), c2 = cont(2), c3 = cont(3);
        if (c1 < 0 || c2 < 0 || c3 < 0)
          return {0xfffd, 1};
        return {static_cast<char32_t>(((b0 & 0x07) << 18) | (c1 << 12) | (c2 << 6) | c3), 4};
      }
      return {0xfffd, 1};
    }

    class HarfBuzzShaper final : public Shaper {
    public:
      [[nodiscard]] std::vector<ShapeRun> shape(Face& face, std::string_view utf8,
                                                const ShapeOptions& opts) override {
        std::vector<ShapeRun> runs;
        if (utf8.empty()) {
          ShapeRun empty{};
          empty.face = &face;
          empty.direction = opts.direction;
          runs.push_back(std::move(empty));
          return runs;
        }

        ShapeRun primary{};
        if (!shape_one(face, utf8, opts, primary)) {
          runs.push_back(std::move(primary));
          return runs;
        }

        // Walk the primary glyphs. Tofu (glyph_id == 0) whose source
        // codepoint is in a Nerd Font block triggers a fallback shape
        // pass against the embedded face; everything else stays in the
        // primary run.
        ShapeRun current{};
        current.face = primary.face;
        current.direction = primary.direction;

        const usize glyph_count = primary.glyphs.size();
        for (usize i = 0; i < glyph_count; ++i) {
          const ShapedGlyph& g = primary.glyphs[i];
          bool fallback_eligible = false;
          char32_t cp = 0;
          usize cluster_bytes = 0;
          if (g.glyph_id == 0) {
            auto decoded = decode_utf8_at(utf8, g.cluster);
            cp = decoded.cp;
            cluster_bytes = decoded.bytes;
            if (cluster_bytes > 0 && is_nerd_font_codepoint(cp)) {
              fallback_eligible = true;
            }
          }

          if (!fallback_eligible) {
            current.glyphs.push_back(g);
            current.total_advance += g.x_advance;
            continue;
          }

          // Flush the accumulated primary glyphs.
          if (!current.glyphs.empty()) {
            runs.push_back(std::move(current));
            current = ShapeRun{};
            current.face = primary.face;
            current.direction = primary.direction;
          }

          // Load (or reuse) the embedded Nerd Font face at the same size
          // and shape just the cluster substring.
          auto nerd = embedded_nerd_font_face(face.pixel_size());
          if (!nerd) {
            current.glyphs.push_back(g); // give up — render the tofu
            current.total_advance += g.x_advance;
            continue;
          }

          ShapeRun sub{};
          if (!shape_one(*nerd, utf8.substr(g.cluster, cluster_bytes), opts, sub)) {
            current.glyphs.push_back(g);
            current.total_advance += g.x_advance;
            continue;
          }

          // Rebase the sub-shape cluster offsets onto the parent buffer
          // so callers can map glyphs back to the original UTF-8.
          for (auto& sg : sub.glyphs) {
            sg.cluster += g.cluster;
          }
          // Cache the shared_ptr so the Face outlives this call. We rely on
          // `embedded_nerd_font_face` retaining a process-wide reference;
          // see src/font/embedded_nerd_font.cpp.
          sub.face = nerd.get();
          runs.push_back(std::move(sub));
          // Skip any additional tofu glyphs that share this cluster —
          // HB rarely emits >1 notdef per codepoint but be defensive.
          while (i + 1 < glyph_count && primary.glyphs[i + 1].cluster == g.cluster) {
            ++i;
          }
        }

        if (!current.glyphs.empty()) {
          runs.push_back(std::move(current));
        } else if (runs.empty()) {
          // Whole string fell through to the cascade (or was empty after
          // filtering); attach a sentinel run pointing at the primary so
          // downstream code that assumes runs.size() >= 1 stays happy.
          ShapeRun sentinel{};
          sentinel.face = primary.face;
          sentinel.direction = primary.direction;
          runs.push_back(std::move(sentinel));
        }
        return runs;
      }

    private:
      // Shapes `utf8` with `face` and writes glyphs/direction/face into
      // `out`. Returns false if the face has no HarfBuzz binding (in
      // which case `out` is left empty but with face/direction set so
      // callers can return a degenerate run).
      bool shape_one(Face& face, std::string_view utf8, const ShapeOptions& opts, ShapeRun& out) {
        out.face = &face;
        out.direction = opts.direction;
        out.glyphs.clear();
        out.total_advance = 0.0f;
        if (utf8.empty())
          return true;

        hb_font_t* font = face_freetype_hb_font(face);
        if (!font)
          return false;

        if (!opts.variations.empty()) {
          face.set_variations(opts.variations);
        }

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0,
                           static_cast<int>(utf8.size()));
        hb_buffer_set_direction(buf, to_hb_dir(opts.direction));
        if (!opts.script.empty())
          hb_buffer_set_script(buf, to_hb_script(opts.script));
        else
          hb_buffer_guess_segment_properties(buf);
        hb_buffer_set_language(buf, to_hb_lang(opts.language));

        std::vector<Feature> feats = opts.features.empty() ? default_features() : opts.features;
        std::vector<hb_feature_t> hb_feats;
        hb_feats.reserve(feats.size());
        for (const auto& f : feats) {
          hb_feature_t hf{};
          hf.tag = f.tag.packed();
          hf.value = f.value;
          hf.start = f.start;
          hf.end = f.end;
          hb_feats.push_back(hf);
        }

        hb_shape(font, buf, hb_feats.data(), static_cast<unsigned>(hb_feats.size()));

        unsigned n = 0;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &n);
        hb_glyph_position_t* poss = hb_buffer_get_glyph_positions(buf, &n);
        out.glyphs.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
          ShapedGlyph g{};
          g.glyph_id = infos[i].codepoint;
          g.x_advance = static_cast<float>(poss[i].x_advance) / 64.0f;
          g.y_advance = static_cast<float>(poss[i].y_advance) / 64.0f;
          g.x_offset = static_cast<float>(poss[i].x_offset) / 64.0f;
          g.y_offset = static_cast<float>(poss[i].y_offset) / 64.0f;
          g.cluster = infos[i].cluster;
          out.glyphs.push_back(g);
          out.total_advance += g.x_advance;
        }

        hb_buffer_destroy(buf);
        return true;
      }
    };

  } // namespace

  std::unique_ptr<Shaper> make_harfbuzz_shaper() {
    return std::make_unique<HarfBuzzShaper>();
  }

} // namespace fxe::font
