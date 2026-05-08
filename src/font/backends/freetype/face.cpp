// FreeType-backed Face. Cross-platform fallback rasterizer. Uses HarfBuzz's
// `hb_ft_font_create` so the same face can be passed to the HarfBuzz shaper
// without re-loading the font data.
//
// FreeType is NOT thread-safe across faces sharing a library, so every FT
// call goes through `Library::lock()`.

#include <fxe/font/atlas.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/library.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#if FXE_FONT_HAS_HARFBUZZ
#include <hb-ft.h>
#include <hb.h>
#endif

#include <atomic>
#include <cmath>
#include <cstring>
#include <fxe/types.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fxe::font {
  namespace {
    std::atomic<u64> g_face_id{1};

    Style style_from_ft(FT_Face f) noexcept {
      const bool b = (f->style_flags & FT_STYLE_FLAG_BOLD) != 0;
      const bool i = (f->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
      if (b && i)
        return Style::bold_italic;
      if (b)
        return Style::bold;
      if (i)
        return Style::italic;
      return Style::regular;
    }

    int to_ft_load_flags(Hint h) noexcept {
      switch (h) {
      case Hint::none:
        return FT_LOAD_NO_HINTING;
      case Hint::light:
        return FT_LOAD_TARGET_LIGHT;
      case Hint::full:
        return FT_LOAD_TARGET_NORMAL;
      }
      return FT_LOAD_DEFAULT;
    }

    class FreeTypeFace final : public Face {
    public:
      FreeTypeFace(FT_Face face, std::vector<u8> bytes, float pixel_size)
          : face_(face), bytes_(std::move(bytes)), pixel_size_(pixel_size),
            id_(g_face_id.fetch_add(1)) {
        // Set pixel size up-front. FT requires pixel sizes in 26.6 fixed-point
        // for `FT_Set_Char_Size`, so multiply by 64.
        auto& lib = shared_library();
        auto guard = lib.lock();
        FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(std::lround(pixel_size_)));
#if FXE_FONT_HAS_HARFBUZZ
        hb_font_ = hb_ft_font_create_referenced(face_);
        if (hb_font_)
          hb_ft_font_set_funcs(hb_font_);
#endif
      }
      ~FreeTypeFace() override {
#if FXE_FONT_HAS_HARFBUZZ
        if (hb_font_)
          hb_font_destroy(hb_font_);
#endif
        if (face_) {
          auto& lib = shared_library();
          auto guard = lib.lock();
          FT_Done_Face(face_);
        }
      }

      [[nodiscard]] u64 id() const noexcept override {
        return id_;
      }

      [[nodiscard]] std::string family_name() const override {
        return face_->family_name ? std::string{face_->family_name} : std::string{};
      }
      [[nodiscard]] Style style() const noexcept override {
        return style_from_ft(face_);
      }
      [[nodiscard]] float pixel_size() const noexcept override {
        return pixel_size_;
      }

      [[nodiscard]] FaceMetrics metrics() const noexcept override {
        FaceMetrics m{};
        const float scale = pixel_size_ / static_cast<float>(face_->units_per_EM);
        m.ascent = static_cast<float>(face_->ascender) * scale;
        m.descent = static_cast<float>(-face_->descender) * scale;
        m.line_gap =
            static_cast<float>(face_->height - (face_->ascender - face_->descender)) * scale;
        m.line_height = static_cast<float>(face_->height) * scale;
        m.underline_position = static_cast<float>(face_->underline_position) * scale;
        m.underline_thickness = static_cast<float>(face_->underline_thickness) * scale;
        // Rough em advance: glyph index for 'M'.
        FT_UInt gi = FT_Get_Char_Index(face_, 'M');
        if (gi) {
          if (FT_Load_Glyph(face_, gi, FT_LOAD_DEFAULT) == 0) {
            m.em_advance = static_cast<float>(face_->glyph->advance.x) / 64.0f;
          }
        }
        return m;
      }

      [[nodiscard]] bool has_color() const noexcept override {
        return (face_->face_flags & FT_FACE_FLAG_COLOR) != 0;
      }

      [[nodiscard]] u32 glyph_index(char32_t cp) const noexcept override {
        return FT_Get_Char_Index(face_, static_cast<FT_ULong>(cp));
      }

      void set_variations(std::span<const Variation> vs) override {
        if (vs.empty())
          return;
        if ((face_->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) == 0)
          return;
        FT_MM_Var* var = nullptr;
        if (FT_Get_MM_Var(face_, &var) != 0 || !var)
          return;
        std::vector<FT_Fixed> coords(static_cast<usize>(var->num_axis), 0);
        // Default axis values are stored as 16.16 fixed point.
        for (FT_UInt i = 0; i < var->num_axis; ++i) {
          coords[i] = var->axis[i].def;
        }
        for (const auto& v : vs) {
          for (FT_UInt i = 0; i < var->num_axis; ++i) {
            const u32 want = v.tag.packed();
            if (var->axis[i].tag == want) {
              coords[i] = static_cast<FT_Fixed>(v.value * 65536.0f);
              break;
            }
          }
        }
        FT_Set_Var_Design_Coordinates(face_, var->num_axis, coords.data());
        FT_Done_MM_Var(face_->glyph->library, var);
      }

      [[nodiscard]] Glyph render_glyph(u32 glyph_id, Atlas& mask, Atlas& color, Hint hint,
                                       float subpixel_x = 0.0f) override {
        Glyph out{};
        if (glyph_id == 0)
          return out;

        auto guard = shared_library().lock();
        int load_flags = to_ft_load_flags(hint) | FT_LOAD_RENDER;
        const bool color_glyph = has_color();
        if (color_glyph)
          load_flags |= FT_LOAD_COLOR;

        // Sub-pixel positioning: FreeType expresses the pen as a 26.6 fixed-
        // point delta. Convert the [0, 1) px shift the cache hands us into
        // 26.6 (× 64) and apply it before loading. Variable fonts and color
        // emoji ignore this shift; mono-mask faces honour it via the hinter.
        FT_Vector shift{};
        if (std::isfinite(subpixel_x) && subpixel_x > 0.0f) {
          float clamped = subpixel_x;
          if (clamped >= 1.0f)
            clamped -= std::floor(clamped);
          shift.x = static_cast<FT_Pos>(std::lround(clamped * 64.0f));
        }
        FT_Set_Transform(face_, nullptr, &shift);

        if (FT_Load_Glyph(face_, static_cast<FT_UInt>(glyph_id), load_flags) != 0)
          return out;
        FT_GlyphSlot slot = face_->glyph;
        const FT_Bitmap& bm = slot->bitmap;
        out.advance_x = static_cast<float>(slot->advance.x) / 64.0f;
        if (bm.width == 0 || bm.rows == 0)
          return out;

        out.offset_x = static_cast<float>(slot->bitmap_left);
        out.offset_y = static_cast<float>(-slot->bitmap_top);
        out.width = bm.width;
        out.height = bm.rows;

        AtlasRegion region;
        if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
          out.format = Format::bgra;
          region = color.pack(bm.width, bm.rows, bm.buffer);
        } else if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
          out.format = Format::grayscale;
          // FT may pad rows; copy row-by-row if pitch != width.
          if (static_cast<u32>(std::abs(bm.pitch)) == bm.width && bm.pitch >= 0) {
            region = mask.pack(bm.width, bm.rows, bm.buffer);
          } else {
            std::vector<u8> tight(static_cast<usize>(bm.width) * bm.rows, 0);
            for (unsigned y = 0; y < bm.rows; ++y) {
              std::memcpy(tight.data() + static_cast<usize>(y) * bm.width,
                          bm.buffer + static_cast<isize>(y) * bm.pitch, bm.width);
            }
            region = mask.pack(bm.width, bm.rows, tight.data());
          }
        } else {
          // Unsupported modes (mono, LCD) are reported as empty for now. The
          // Hint::light/none path emits gray bitmaps so we shouldn't get here
          // in practice.
          return Glyph{};
        }
        if (!region.ok)
          return Glyph{};
        out.atlas_x = region.x;
        out.atlas_y = region.y;
        return out;
      }

      [[nodiscard]] void* native_handle() const noexcept override {
        return face_;
      }

#if FXE_FONT_HAS_HARFBUZZ
      [[nodiscard]] hb_font_t* hb_font() const noexcept {
        return hb_font_;
      }
#endif

    private:
      FT_Face face_ = nullptr;
      std::vector<u8> bytes_;
      float pixel_size_ = 0.0f;
      u64 id_ = 0;
#if FXE_FONT_HAS_HARFBUZZ
      hb_font_t* hb_font_ = nullptr;
#endif
    };

  } // namespace

#if FXE_FONT_HAS_HARFBUZZ
  // Helper used by the HarfBuzz shaper to recover the hb_font_t from a Face
  // produced by this backend. Returns nullptr for non-FT faces.
  hb_font_t* face_freetype_hb_font(Face& f) {
    auto* ft = dynamic_cast<FreeTypeFace*>(&f);
    return ft ? ft->hb_font() : nullptr;
  }
#endif

  std::unique_ptr<Face> load_face_freetype(std::span<const u8> bytes, float pixel_size,
                                           u32 face_index) {
    if (bytes.empty())
      return nullptr;
    FT_Face face = nullptr;
    std::vector<u8> owned(bytes.begin(), bytes.end());
    {
      auto& lib = shared_library();
      auto guard = lib.lock();
      FT_Library ftlib = static_cast<FT_Library>(lib.raw());
      if (!ftlib)
        return nullptr;
      if (FT_New_Memory_Face(ftlib, owned.data(), static_cast<FT_Long>(owned.size()),
                             static_cast<FT_Long>(face_index), &face) != 0) {
        return nullptr;
      }
    }
    return std::make_unique<FreeTypeFace>(face, std::move(owned), pixel_size);
  }

} // namespace fxe::font
