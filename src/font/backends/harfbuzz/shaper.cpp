// HarfBuzz-backed Shaper. Pairs naturally with the FreeType face backend
// (which exposes an `hb_font_t` for free); falls back to deriving an
// `hb_font_t` from raw FT_Face when needed.

#include <fxe/font/face.hpp>
#include <fxe/font/shaper.hpp>

#include <hb-ft.h>
#include <hb.h>

#include <memory>
#include <string>
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

    class HarfBuzzShaper final : public Shaper {
    public:
      [[nodiscard]] ShapeRun shape(Face& face, std::string_view utf8,
                                   const ShapeOptions& opts) override {
        ShapeRun out{};
        out.direction = opts.direction;
        if (utf8.empty())
          return out;

        hb_font_t* font = face_freetype_hb_font(face);
        if (!font)
          return out;

        // Apply variations on the face if requested.
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
          // HarfBuzz returns advances/offsets in 26.6 fixed-point.
          g.x_advance = static_cast<float>(poss[i].x_advance) / 64.0f;
          g.y_advance = static_cast<float>(poss[i].y_advance) / 64.0f;
          g.x_offset = static_cast<float>(poss[i].x_offset) / 64.0f;
          g.y_offset = static_cast<float>(poss[i].y_offset) / 64.0f;
          g.cluster = infos[i].cluster;
          out.glyphs.push_back(g);
          out.total_advance += g.x_advance;
        }

        hb_buffer_destroy(buf);
        return out;
      }
    };

  } // namespace

  std::unique_ptr<Shaper> make_harfbuzz_shaper() {
    return std::make_unique<HarfBuzzShaper>();
  }

} // namespace fxe::font
