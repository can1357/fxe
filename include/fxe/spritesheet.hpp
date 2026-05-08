#pragma once

#include <fxe/types.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fxe/color.hpp>
#include <fxe/math.hpp>

namespace fxe {
  using texture_id = u32;
  // Texture ids reserve the low 19 bits for table indices, matching the
  // original sprite encoding: bit 19 marks animated sprites, bit 20 marks
  // multi-texture atlases, and bit 21 marks extra-large tiled sprites. The
  // all-zero id means "no texture". framebuffer_texture_id deliberately uses
  // a high sentinel outside the indexed/flagged range so renderers can detect
  // blur samples that should read the captured frame instead of a spritesheet
  // entry (see primitives::blur_quad and the WGPU renderer/shader path).
  inline constexpr u32 sprite_index_mask = 0x7ffffu;
  inline constexpr u32 animated_sprite_flag = 0x80000u;
  inline constexpr u32 multi_sprite_flag = 0x100000u;
  inline constexpr u32 xl_sprite_flag = 0x200000u;

  inline constexpr u32 sprite_mask = sprite_index_mask;
  inline constexpr u32 asprite_flag = animated_sprite_flag;
  inline constexpr u32 msprite_flag = multi_sprite_flag;
  inline constexpr u32 xlsprite_flag = xl_sprite_flag;

  // Font module flag bits (mirrored in src/wgpu/shaders/main.wgsl).
  // FONT_MASK_FLAG samples the R8 mask atlas with vertex-color modulation;
  // FONT_COLOR_FLAG samples the BGRA color emoji atlas as-is. Both bits
  // live below the sprite_index_mask high-water so they never collide with
  // ids returned by `add_sprite`.
  inline constexpr u32 font_mask_flag = 0x040000u;
  inline constexpr u32 font_color_flag = 0x080000u;
  // User-texture flag for surface caching. The lower 2 bits select one of
  // four user texture slots bound via `renderer::bind_user_texture`. Mirrored
  // in src/wgpu/shaders/main.wgsl as USER_TEX_FLAG / USER_TEX_SLOT_MASK.
  inline constexpr u32 user_tex_flag = 0x200000u;
  inline constexpr u32 user_tex_slot_mask = 0x3u;
  inline constexpr u32 null_texture_id = 0;
  inline constexpr u32 framebuffer_texture_id = 0x7ffffffeu;

  inline constexpr u32 null_texture = null_texture_id;
  inline constexpr u32 framebuffer_texture = framebuffer_texture_id;

  struct sprite {
    math::uvec2 at{};
    math::uvec2 size{};
    texture_id texture = null_texture;
    [[nodiscard]] constexpr float aspect() const noexcept {
      return size.y == 0 ? 1.0f : float(size.x) / float(size.y);
    }
  };

  struct msprite {
    std::vector<sprite> entries;
  };
  struct asprite {
    texture_id base_texture = null_texture;
    std::vector<float> delays;
  };
  struct xlsprite {
    texture_id base_texture = null_texture;
    math::uvec2 tile_size{};
    math::uvec2 grid{};
  };

  struct glyph_info {
    math::vec2 uv0{};
    math::vec2 uv1{};
    math::vec2 offset{};
    math::vec2 size{};
    float advance = 0.0f;
    texture_id tx = null_texture;
    // Pixel-space atlas origin used to recompute normalized UVs when a
    // growable font atlas is resized.
    math::uvec2 atlas_at{};
  };

  struct texture_data {
    math::uvec2 size{};
    std::vector<r8g8b8a8> pixels;
  };

  struct font_variant_info {
    float pixel_height = 16.0f;
    float line_gap = 4.0f;
    float ascent = 16.0f;
    float line_height = 20.0f;
    texture_id texture = null_texture;
    std::unordered_map<char32_t, glyph_info> glyphs;
  };

  struct font_runtime;

  struct font_info {
    float pixel_height = 16.0f;
    float line_gap = 4.0f;
    texture_id texture = null_texture;
    std::unordered_map<char32_t, glyph_info> glyphs;
    std::shared_ptr<font_runtime> runtime;
  };

  namespace font {
    class Face;
  }

  // Returns the font module Face attached to `font`, or nullptr if the
  // font hasn't been migrated to the new path. Used by primitives::draw_text
  // to select between the legacy stb_truetype emitter and the HarfBuzz/CT
  // shaper. Phase 7 of FONT_STACK_OVERHAUL drops the legacy path and makes
  // the result non-null for every initialised default font.
  [[nodiscard]] std::shared_ptr<font::Face> font_face_for(const font_info& font);
  // Returns the font module Face for `font` rasterised at `pixel_size_px`.
  // Used by the text path to select a face whose pixel size matches the
  // physical framebuffer resolution (logical pt × device_pixel_ratio).
  // Faces are cached on the font_info so repeated calls at the same size
  // are O(1).
  [[nodiscard]] std::shared_ptr<font::Face> font_face_for(const font_info& font,
                                                          float pixel_size_px);

  struct spritesheet {
    std::vector<texture_data> textures;
    std::vector<sprite> sprites;
    std::vector<msprite> msprites;
    std::vector<asprite> asprites;
    std::vector<xlsprite> xlsprites;
    font_info default_font;

    [[nodiscard]] texture_id add_texture(texture_data tex);
    [[nodiscard]] texture_id add_sprite(sprite s);
    [[nodiscard]] texture_id resolve_if(texture_id id, float time_seconds) const;
  };

  [[nodiscard]] const font_info& get_font_info();
  void init_default_fonts(spritesheet& sheet);
  void init_default_fonts(spritesheet& sheet, std::span<const u8> ttf_bytes);
  void init_default_fonts(spritesheet& sheet, std::span<const u8> ttf_bytes, float pixel_height);
  [[nodiscard]] const font_variant_info& resolve_font_variant(const font_info& font, float pt);
  [[nodiscard]] glyph_info resolve_font_glyph(const font_info& font,
                                              const font_variant_info& variant, char32_t codepoint);
  // Process-wide spritesheet that owns the default font atlas and any user
  // textures uploaded through it. The first call lazily initialises the
  // default font (system TTF if available, else procedural). Renderers can
  // peek at the first texture in `textures` to upload as their atlas.
  [[nodiscard]] spritesheet& get_default_spritesheet();
  [[nodiscard]] texture_data load_texture(std::span<const u8> encoded_rgba);
  [[nodiscard]] texture_data load_texture_resized(std::span<const u8> encoded,
                                                  math::uvec2 dst_size);
} // namespace fxe
