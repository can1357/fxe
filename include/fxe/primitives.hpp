#pragma once

#include <fxe/types.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <fxe/command_buffer.hpp>

namespace fxe::primitives {
  // ---------------------------------------------------------------------------
  // Helper aggregates — keep as plain structs, the V8 binding layer reads them.
  // ---------------------------------------------------------------------------
  struct texture_info {
    texture_id texture = null_texture;
    math::vec2 src{0.0f, 0.0f};
    math::vec2 dst{1.0f, 1.0f};
  };

  template <usize N> using color_list = std::array<r8g8b8a8, N>;
  template <typename T, usize N> struct optional_list {
    std::array<T, N> data{};
    constexpr optional_list() = default;
    constexpr optional_list(T v) {
      data.fill(v);
    }
    template <typename... Vs>
      requires(sizeof...(Vs) == N)
    constexpr optional_list(Vs... vs) : data{T(vs)...} {}
    constexpr T& operator[](usize i) {
      return data[i];
    }
    constexpr const T& operator[](usize i) const {
      return data[i];
    }
    constexpr auto begin() const {
      return data.begin();
    }
    constexpr auto end() const {
      return data.end();
    }
    constexpr usize size() const {
      return N;
    }
  };

  enum text_flag : u32 {
    text_bold = 1u << 0,
    text_mono = 1u << 1,
    text_uppercase = 1u << 2,
    text_lowercase = 1u << 3,
    text_no_control = 1u << 4,
    text_vcenter = 1u << 5,
    text_hcenter = 1u << 6,
    text_italic = 1u << 7,
    text_rjust = 1u << 8,
  };
  inline constexpr u32 wrap_after_disabled = 0xffffffffu;

  enum class whitespace_glyphs : u32 {
    none = 0,    // tab/newline render as ordinary glyph (or absent)
    visible = 1, // render whitespace as faint visual marks
  };

  struct text_style {
    r8g8b8a8 color = white;
    float pt = 16.0f; // pixel height for one line of text
    u32 flags = 0;
    u32 wrap_after = wrap_after_disabled;
    // Optional override for line height. Zero/negative → use the face's
    // metrics-derived value.
    float line_height = 0.0f;
    // Visual tab stop in pixels (logical). Zero = render TAB literally.
    float tab_size = 0.0f;
    // Tab origin x in pixels (logical). Tab stops snap to multiples of
    // tab_size relative to this origin. Defaults to the draw origin.x.
    float tab_origin_x = 0.0f;
    whitespace_glyphs whitespace = whitespace_glyphs::none;
    // OpenType feature settings (e.g. {"liga", 1}, {"ss01", 1}). Empty list
    // = use the font module's default features (calt + liga + kern).
    std::vector<std::pair<std::array<char, 4>, u32>> features{};
    // OpenType variation axes (e.g. {"wght", 600}). Empty list = leave the
    // face at its default axes.
    std::vector<std::pair<std::array<char, 4>, float>> variations{};
  };

  enum class paint_kind : u32 { solid = 0, linear = 1, radial = 2, conic = 3 };
  enum class fill_rule : u32 { nonzero = 0, evenodd = 1 };
  enum class line_join : u32 { miter = 0, bevel = 1, round = 2 };
  enum class line_cap : u32 { butt = 0, square = 1, round = 2 };

  struct gradient_stop {
    float t = 0.0f;
    r8g8b8a8 color = white;
  };

  struct paint_value {
    paint_kind kind = paint_kind::solid;
    r8g8b8a8 color = white;
    math::vec4 p0{};
    math::vec4 p1{};
    std::vector<gradient_stop> stops;

    [[nodiscard]] static paint_value solid(r8g8b8a8 color) {
      paint_value p;
      p.color = color;
      return p;
    }
  };

  enum class path_cmd : u8 { move, line, quad, cubic, arc, close };

  struct path_2d {
    std::vector<path_cmd> commands;
    std::vector<float> params;

    path_2d& move_to(float x, float y);
    path_2d& line_to(float x, float y);
    path_2d& quad_to(float cx, float cy, float x, float y);
    path_2d& cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y);
    path_2d& arc(float cx, float cy, float radius, float start_angle, float end_angle,
                 bool ccw = false);
    path_2d& close();
    void reset();
  };

  // ---------------------------------------------------------------------------
  // Lines
  // ---------------------------------------------------------------------------
  void draw_line(command_sink& r, math::vec4 src, math::vec4 dst, const color_list<2>& color,
                 float thickness);
  void draw_line(command_sink& r, math::vec4 src, math::vec4 dst, r8g8b8a8 color,
                 float thickness = 0.0f);
  void draw_line(command_sink& r, std::span<const math::vec4> points,
                 std::span<const r8g8b8a8> colors, float thickness);
  void draw_line(command_sink& r, std::span<const math::vec4> points, r8g8b8a8 color,
                 float thickness = 0.0f);

  // ---------------------------------------------------------------------------
  // Triangles & quads (corner form + transform form)
  // ---------------------------------------------------------------------------
  void draw_triangle(command_sink& r, math::vec4 a, math::vec4 b, math::vec4 c,
                     const color_list<3>& color, float thickness = 0.0f);
  void draw_triangle(command_sink& r, math::vec4 a, math::vec4 b, math::vec4 c, r8g8b8a8 color,
                     float thickness = 0.0f);
  void fill_triangle(command_sink& r, math::vec4 a, math::vec4 b, math::vec4 c,
                     const color_list<3>& color);
  void fill_triangle(command_sink& r, math::vec4 a, math::vec4 b, math::vec4 c, r8g8b8a8 color);

  void draw_quad(command_sink& r, math::vec4 p1, math::vec4 p2, math::vec4 p3, math::vec4 p4,
                 const color_list<4>& color, float thickness);
  void fill_quad(command_sink& r, math::vec4 p1, math::vec4 p2, math::vec4 p3, math::vec4 p4,
                 const color_list<4>& color, const texture_info& tx = {});
  void draw_quad(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color,
                 float thickness = 0.0f);
  void fill_quad(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color,
                 const texture_info& tx = {});

  void draw_quad_rounded(command_sink& r, math::vec4 p1, math::vec4 p2, math::vec4 p3,
                         math::vec4 p4, const optional_list<float, 4>& rnd,
                         const color_list<4>& color, float thickness);
  void fill_quad_rounded(command_sink& r, math::vec4 p1, math::vec4 p2, math::vec4 p3,
                         math::vec4 p4, const optional_list<float, 4>& rnd,
                         const color_list<4>& color, const texture_info& tx = {});

  // Rect: low-corner = origin in the supplied transform / vec2-at form.
  void draw_rect(command_sink& r, const math::mat4x4& transform, float shift,
                 const color_list<4>& color, float thickness);
  void fill_rect(command_sink& r, const math::mat4x4& transform, float shift,
                 const color_list<4>& color, const texture_info& tx = {});
  void draw_rect_rounded(command_sink& r, const math::mat4x4& transform,
                         const optional_list<float, 4>& rnd, float shift,
                         const color_list<4>& color, float thickness);
  void fill_rect_rounded(command_sink& r, const math::mat4x4& transform,
                         const optional_list<float, 4>& rnd, float shift,
                         const color_list<4>& color, const texture_info& tx = {});
  void draw_rect(command_sink& r, math::vec2 at, math::vec2 size, float depth, r8g8b8a8 color,
                 float thickness = 0.0f);
  void fill_rect(command_sink& r, math::vec2 at, math::vec2 size, float depth, r8g8b8a8 color,
                 const texture_info& tx = {});
  void draw_textured_quad(command_buffer& r, const math::vec2& pos, const math::vec2& size,
                          float depth, texture_id tex, math::vec2 uv0, math::vec2 uv1,
                          r8g8b8a8 tint);
  void fill_rect(command_sink& r, math::vec2 at, math::vec2 size, float depth,
                 const paint_value& paint);
  void fill_rect_rounded(command_sink& r, const math::mat4x4& transform,
                         const optional_list<float, 4>& rnd, float shift, const paint_value& paint);

  // ---------------------------------------------------------------------------
  // Ellipses, cylinders, spheres
  // ---------------------------------------------------------------------------
  void draw_ellipse(command_sink& r, const math::mat4x4& transform, const color_list<2>& color,
                    float thickness_per_radius, float percentage = 1.0f, usize edges = 64,
                    texture_id tx = null_texture);
  void fill_ellipse(command_sink& r, const math::mat4x4& transform, const color_list<2>& color,
                    float percentage = 1.0f, usize edges = 64, texture_id tx = null_texture);
  void draw_ellipse(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color,
                    float thickness_per_radius, float percentage = 1.0f, usize edges = 64,
                    texture_id tx = null_texture);
  void fill_ellipse(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color,
                    float percentage = 1.0f, usize edges = 64, texture_id tx = null_texture);

  void fill_cylinder(command_sink& r, const math::mat4x4& transform, const color_list<2>& color,
                     float percentage = 1.0f, usize edges = 64);
  void fill_sphere(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color,
                   float perc_x = 1.0f, float perc_y = 1.0f, usize edges = 32);

  void fill_pyramid(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color);
  void draw_pyramid(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color,
                    float thickness);
  void fill_box(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color);
  void draw_box(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color, float thickness);
  void fill_cbox(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color);
  void draw_cbox(command_sink& r, const math::mat4x4& transform, r8g8b8a8 color, float thickness);

  // ---------------------------------------------------------------------------
  // 3D primitives — eight-corner colour gradient supported on box/cbox.
  // box: low corner at origin. cbox: centred on origin.
  // ---------------------------------------------------------------------------
  void fill_pyramid(command_sink& r, const math::mat4x4& transform, const color_list<2>& color);
  void draw_pyramid(command_sink& r, const math::mat4x4& transform, const color_list<2>& color,
                    float thickness);

  void fill_box(command_sink& r, const math::mat4x4& transform, const color_list<8>& color);
  void draw_box(command_sink& r, const math::mat4x4& transform, const color_list<8>& color,
                float thickness);
  void fill_cbox(command_sink& r, const math::mat4x4& transform, const color_list<8>& color);
  void draw_cbox(command_sink& r, const math::mat4x4& transform, const color_list<8>& color,
                 float thickness);

  // 2D-collapse helpers retained from previous API for backwards compat.
  void fill_box(command_sink& r, math::vec3 mn, math::vec3 mx, r8g8b8a8 color);
  void draw_box(command_sink& r, math::vec3 mn, math::vec3 mx, r8g8b8a8 color,
                float thickness = 0.0f);

  // ---------------------------------------------------------------------------
  // Paths and higher-level paint.
  // ---------------------------------------------------------------------------
  void fill_path(command_sink& r, const path_2d& path, const paint_value& paint,
                 fill_rule rule = fill_rule::nonzero, float depth = 0.0f);
  void fill_path(command_sink& r, const path_2d& path, r8g8b8a8 color,
                 fill_rule rule = fill_rule::nonzero, float depth = 0.0f);
  void stroke_path(command_sink& r, const path_2d& path, const paint_value& paint, float line_width,
                   line_join join = line_join::miter, line_cap cap = line_cap::butt,
                   float depth = 0.0f);
  void stroke_path(command_sink& r, const path_2d& path, r8g8b8a8 color, float line_width,
                   line_join join = line_join::miter, line_cap cap = line_cap::butt,
                   float depth = 0.0f);

  void draw_shadow_rect(command_sink& r, float x, float y, float w, float h, float depth,
                        r8g8b8a8 color, float blur, float spread, float offset_x, float offset_y,
                        float screen_w, float screen_h);
  void draw_shadow_rect_rounded(command_sink& r, float x, float y, float w, float h,
                                const optional_list<float, 4>& rnd, float depth, r8g8b8a8 color,
                                float blur, float spread, float offset_x, float offset_y,
                                float screen_w, float screen_h);
  void draw_inner_shadow_rect_rounded(command_sink& r, float x, float y, float w, float h,
                                      const optional_list<float, 4>& rnd, float depth,
                                      r8g8b8a8 color, float blur, float spread, float offset_x,
                                      float offset_y, float screen_w, float screen_h);

  // ---------------------------------------------------------------------------
  // Text. Uses spritesheet glyph map populated by init_default_fonts(spritesheet&).
  // ---------------------------------------------------------------------------
  [[nodiscard]] math::vec2 calc_text(std::string_view text, const font_info& font, float pt);
  [[nodiscard]] math::vec2 calc_text(std::string_view text, text_style style,
                                     const font_info& font);
  // Returns (size_x, size_y, advance_x, glyph_count) of the drawn run for callers
  // that need to lay out follow-on geometry.
  math::vec4 draw_text(command_sink& r, math::vec2 at, float depth, std::string_view text,
                       const font_info& font, text_style style = {});
  math::vec4 draw_text(command_sink& r, const math::mat4x4& transform, std::string_view text,
                       const font_info& font, text_style style = {});

  // --- Phase 0 editor primitives -------------------------------------------
  // One trampoline, N styled spans on the same baseline.
  // Each span is laid out left-to-right starting at `at`. The baseline
  // position is shared. Returns (width, height, advance_x, glyph_count) like
  // draw_text. tab_size on each span's style is honoured against `at.x`.
  struct text_span {
    std::string_view text;
    text_style style;
    const font_info* font = nullptr; // null → use fallback_font
    bool underline = false;          // baseline-relative straight underline
    bool strikethrough = false;      // mid-x strike line
  };
  math::vec4 draw_text_spans(command_sink& r, math::vec2 at, float depth,
                             std::span<const text_span> spans, const font_info& fallback_font);

  // Paint many axis-aligned rects in one trampoline. Used by editor
  // selections (one rect per visible line per cursor).
  void draw_selection_rects(command_sink& r, std::span<const math::vec4> rects, r8g8b8a8 color,
                            float depth = 0.0f);

  // Decoration underline (squiggle / dashed / dotted / solid) for diagnostics
  // and spell-check. `y` is the baseline at which the decoration sits.
  enum class decoration_style : u32 {
    solid = 0,
    dashed = 1,
    dotted = 2,
    wavy = 3,
  };
  void draw_decoration_underline(command_sink& r, float x1, float x2, float y,
                                 decoration_style style, r8g8b8a8 color, float thickness = 1.0f,
                                 float depth = 0.0f);

  // ---------------------------------------------------------------------------
  // Blur helpers — emit textured quads that the post-process chain blurs in the
  // final composite. dispersion is in screen pixels.
  // ---------------------------------------------------------------------------
  void blur_quad(command_sink& r, math::vec4 p1, math::vec4 p2, math::vec4 p3, math::vec4 p4,
                 const color_list<4>& color, float dispersion, math::vec2 screen_size);
  void blur_rect(command_sink& r, const math::mat4x4& transform, float shift,
                 const color_list<4>& color, float dispersion, math::vec2 screen_size);
  void blur_rect(command_sink& r, math::vec2 at, math::vec2 size, float depth, r8g8b8a8 color,
                 float dispersion, math::vec2 screen_size);
} // namespace fxe::primitives
