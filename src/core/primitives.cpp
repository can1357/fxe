// Port of gfw/gfwx primitive helpers into the fxe::primitives namespace.
//
// The public API in <fxe/primitives.hpp> is the contract we have to honour;
// the geometry math mirrors framework/src/gfw/gfwx.cpp directly so
// the rendered output matches the original engine. Differences from the
// original:
//   * fxe vertex/colour/texture types replace the gfw::* originals.
//   * Math is glm-backed (column-major). `mat[i]` is the i-th column.
//   * apply_no_w(m, v) is a local helper since fxe::math::vec4 has no member
//     of that name. It treats v as a 3D point, applies the transform, and
//     forces the resulting w to 0 (screen-space / non-world vertices).
//   * Text helpers use the simplified (font_info + text_style) layout from
//     the new header rather than the gfw::text_state machinery; control
//     code parsing was dropped, only `wrap_after` and case folding remain.
//   * Blur primitives tag post-fx samples with fxe::framebuffer_texture_id so
//     backends can bind the current captured frame without a public DX-style
//     framebuffer handle.

#include <fxe/primitives.hpp>
#include <fxe/types.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <algorithm>
#include <fxe/color.hpp>
#include <fxe/command_buffer.hpp>
#include <fxe/font.hpp>
#include <fxe/math.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/vertex.hpp>
#include <limits>

namespace fxe::primitives {
  namespace {
    // ---------------------------------------------------------------------------
    // Small helpers reused by every primitive.
    // ---------------------------------------------------------------------------
    [[maybe_unused]] static void adjust_indices(const u32* src, u32* dst, usize n) noexcept {
      for (usize i = 0; i != n; ++i)
        dst[i] += src[i];
    }

    // Apply matrix to a 3D point dropping the input/output w. The vertex
    // pipeline reuses pos.w as the "is_world" flag, so non-world geometry
    // must end up with w == 0 even when the matrix has translation in
    // column 3.
    [[nodiscard]] inline math::vec4 apply_no_w(const math::mat4x4& m, math::vec4 v) noexcept {
      math::vec4 r = m * math::vec4{v.x, v.y, v.z, 1.0f};
      r.w = 0.0f;
      return r;
    }

    // Build a mat4x4 in the gfwx convention: columns 0..2 are scaled basis
    // vectors with w==0, column 3 is translation with w==0 (so the resulting
    // vertices stay in screen space when fed through apply_no_w).
    [[nodiscard]] inline math::mat4x4 make_screen_transform(math::vec2 at, math::vec2 size,
                                                            float depth) noexcept {
      math::mat4x4 m{1.0f};
      m[0] = math::vec4{size.x, 0.0f, 0.0f, 0.0f};
      m[1] = math::vec4{0.0f, size.y, 0.0f, 0.0f};
      m[2] = math::vec4{0.0f, 0.0f, 1.0f, 0.0f};
      m[3] = math::vec4{at.x, at.y, depth, 0.0f};
      return m;
    }

    // Rounded-rectangle constants — mirror the original.
    constexpr int kRoundedEdgeCount = 16;
    constexpr float kRoundedEdgeStepToRadian = math::pi / (2.0f * float(kRoundedEdgeCount));

    // 5x5 Gaussian samples used by the screen-space blur expander.
    constexpr float kBlurKernel[3][3] = {
        {0.99999999999998f, 0.60653065971262f, 0.13533528323661f},
        {0.60653065971262f, 0.36787944117143f, 0.082084998623897f},
        {0.13533528323661f, 0.082084998623897f, 0.018315638888734f},
    };

    // Linearly interpolate two colour samples in 0..255 space.
    [[nodiscard]] inline r8g8b8a8 lerp_color(r8g8b8a8 a, r8g8b8a8 b, float t) noexcept {
      t = math::fclamp(t, 0.0f, 1.0f);
      auto mix = [&](u8 x, u8 y) { return u8(float(x) * (1.0f - t) + float(y) * t); };
      return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
    }

    // Decode a single UTF-8 codepoint, advancing `it`.

    constexpr int kMaxCurveSubdivisions = 16;
    constexpr texture_id kPaintLinearTexture = 0x7ffffff0u;
    constexpr texture_id kPaintRadialTexture = 0x7ffffff1u;
    constexpr texture_id kPaintConicTexture = 0x7ffffff2u;

    struct flat_subpath {
      std::vector<math::vec2> points;
      bool closed = false;
    };

    [[nodiscard]] inline float cross2(math::vec2 a, math::vec2 b) noexcept {
      return a.x * b.y - a.y * b.x;
    }

    [[nodiscard]] inline float signed_area(std::span<const math::vec2> pts) noexcept {
      float a = 0.0f;
      for (usize i = 0; i < pts.size(); ++i) {
        const math::vec2 p = pts[i];
        const math::vec2 q = pts[(i + 1) % pts.size()];
        a += p.x * q.y - q.x * p.y;
      }
      return a * 0.5f;
    }

    [[nodiscard]] inline r8g8b8a8 pack_color(math::vec4 c) noexcept {
      return {
          u8(math::fclamp(c.r, 0.0f, 1.0f) * 255.0f), u8(math::fclamp(c.g, 0.0f, 1.0f) * 255.0f),
          u8(math::fclamp(c.b, 0.0f, 1.0f) * 255.0f), u8(math::fclamp(c.a, 0.0f, 1.0f) * 255.0f)};
    }

    [[nodiscard]] inline math::vec4 unpack_color(r8g8b8a8 c) noexcept {
      return {float(c.r) / 255.0f, float(c.g) / 255.0f, float(c.b) / 255.0f, float(c.a) / 255.0f};
    }

    [[nodiscard]] r8g8b8a8 sample_stops(const paint_value& paint, float t) {
      if (paint.stops.empty())
        return paint.color;
      t = math::fclamp(t, 0.0f, 1.0f);
      const gradient_stop* prev = &paint.stops.front();
      const gradient_stop* next = &paint.stops.back();
      for (const auto& stop : paint.stops) {
        if (stop.t <= t)
          prev = &stop;
        if (stop.t >= t) {
          next = &stop;
          break;
        }
      }
      const float span = next->t - prev->t;
      const float local = std::fabs(span) > 0.00001f ? (t - prev->t) / span : 0.0f;
      const math::vec4 a = unpack_color(prev->color);
      const math::vec4 b = unpack_color(next->color);
      return pack_color(a * (1.0f - local) + b * local);
    }

    [[nodiscard]] r8g8b8a8 sample_paint(const paint_value& paint, math::vec2 p) {
      switch (paint.kind) {
      case paint_kind::solid:
        return paint.color;
      case paint_kind::linear: {
        const math::vec2 a{paint.p0.x, paint.p0.y};
        const math::vec2 b{paint.p1.x, paint.p1.y};
        const math::vec2 d = b - a;
        const float denom = math::dot(d, d);
        const float t = denom > 0.00001f ? math::dot(p - a, d) / denom : 0.0f;
        return sample_stops(paint, t);
      }
      case paint_kind::radial: {
        const math::vec2 c{paint.p0.x, paint.p0.y};
        const float radius = paint.p0.z > 0.00001f ? paint.p0.z : 1.0f;
        return sample_stops(paint, math::length(p - c) / radius);
      }
      case paint_kind::conic: {
        const math::vec2 c{paint.p0.x, paint.p0.y};
        float a = std::atan2(p.y - c.y, p.x - c.x) - paint.p0.z;
        constexpr float tau = math::pi * 2.0f;
        while (a < 0.0f)
          a += tau;
        while (a >= tau)
          a -= tau;
        return sample_stops(paint, a / tau);
      }
      }
      return paint.color;
    }

    [[nodiscard]] texture_id paint_texture_id(const paint_value& paint) noexcept {
      switch (paint.kind) {
      case paint_kind::linear:
        return kPaintLinearTexture;
      case paint_kind::radial:
        return kPaintRadialTexture;
      case paint_kind::conic:
        return kPaintConicTexture;
      case paint_kind::solid:
        return null_texture;
      }
      return null_texture;
    }

    void append_unique(std::vector<math::vec2>& pts, math::vec2 p) {
      if (!pts.empty() && math::length(pts.back() - p) < 0.001f)
        return;
      pts.push_back(p);
    }

    void flatten_path(const path_2d& path, std::vector<flat_subpath>& out) {
      math::vec2 cur{0.0f, 0.0f};
      math::vec2 start{0.0f, 0.0f};
      flat_subpath* sub = nullptr;
      usize p = 0;
      auto ensure = [&]() -> flat_subpath& {
        if (!sub || sub->closed) {
          out.push_back({});
          sub = &out.back();
        }
        return *sub;
      };
      for (path_cmd cmd : path.commands) {
        switch (cmd) {
        case path_cmd::move: {
          cur = {path.params[p], path.params[p + 1]};
          p += 2;
          out.push_back({});
          sub = &out.back();
          append_unique(sub->points, cur);
          start = cur;
          break;
        }
        case path_cmd::line: {
          cur = {path.params[p], path.params[p + 1]};
          p += 2;
          append_unique(ensure().points, cur);
          break;
        }
        case path_cmd::quad: {
          const math::vec2 c{path.params[p], path.params[p + 1]};
          const math::vec2 dst{path.params[p + 2], path.params[p + 3]};
          p += 4;
          auto& pts = ensure().points;
          for (int i = 1; i <= kMaxCurveSubdivisions; ++i) {
            const float t = float(i) / float(kMaxCurveSubdivisions);
            const float u = 1.0f - t;
            append_unique(pts, cur * (u * u) + c * (2.0f * u * t) + dst * (t * t));
          }
          cur = dst;
          break;
        }
        case path_cmd::cubic: {
          const math::vec2 c1{path.params[p], path.params[p + 1]};
          const math::vec2 c2{path.params[p + 2], path.params[p + 3]};
          const math::vec2 dst{path.params[p + 4], path.params[p + 5]};
          p += 6;
          auto& pts = ensure().points;
          for (int i = 1; i <= kMaxCurveSubdivisions; ++i) {
            const float t = float(i) / float(kMaxCurveSubdivisions);
            const float u = 1.0f - t;
            append_unique(pts, cur * (u * u * u) + c1 * (3.0f * u * u * t) +
                                   c2 * (3.0f * u * t * t) + dst * (t * t * t));
          }
          cur = dst;
          break;
        }
        case path_cmd::arc: {
          const math::vec2 c{path.params[p], path.params[p + 1]};
          const float r = path.params[p + 2];
          float a0 = path.params[p + 3];
          float a1 = path.params[p + 4];
          const bool ccw = path.params[p + 5] != 0.0f;
          p += 6;
          constexpr float tau = math::pi * 2.0f;
          float sweep = a1 - a0;
          if (ccw && sweep > 0.0f)
            sweep -= tau;
          if (!ccw && sweep < 0.0f)
            sweep += tau;
          const int segments = std::clamp(int(std::ceil(std::fabs(sweep) / (math::pi / 8.0f))), 1,
                                          kMaxCurveSubdivisions);
          auto& pts = ensure().points;
          for (int i = 1; i <= segments; ++i) {
            const float a = a0 + sweep * (float(i) / float(segments));
            append_unique(pts, {c.x + std::cos(a) * r, c.y + std::sin(a) * r});
          }
          cur = pts.back();
          break;
        }
        case path_cmd::close: {
          auto& s = ensure();
          append_unique(s.points, start);
          s.closed = true;
          cur = start;
          break;
        }
        }
      }
    }

    [[nodiscard]] bool point_in_tri(math::vec2 p, math::vec2 a, math::vec2 b, math::vec2 c,
                                    float winding) noexcept {
      const float ab = cross2(b - a, p - a) * winding;
      const float bc = cross2(c - b, p - b) * winding;
      const float ca = cross2(a - c, p - c) * winding;
      return ab >= -0.0001f && bc >= -0.0001f && ca >= -0.0001f;
    }

    void triangulate_simple(std::span<const math::vec2> polygon, std::vector<u32>& indices) {
      if (polygon.size() < 3)
        return;
      std::vector<u32> order(polygon.size());
      for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
      const float winding = signed_area(polygon) >= 0.0f ? 1.0f : -1.0f;
      usize guard = polygon.size() * polygon.size();
      while (order.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (usize i = 0; i < order.size(); ++i) {
          const u32 ia = order[(i + order.size() - 1) % order.size()];
          const u32 ib = order[i];
          const u32 ic = order[(i + 1) % order.size()];
          const math::vec2 a = polygon[ia];
          const math::vec2 b = polygon[ib];
          const math::vec2 c = polygon[ic];
          if (cross2(b - a, c - b) * winding <= 0.00001f)
            continue;
          bool contains = false;
          for (u32 idx : order) {
            if (idx == ia || idx == ib || idx == ic)
              continue;
            if (point_in_tri(polygon[idx], a, b, c, winding)) {
              contains = true;
              break;
            }
          }
          if (contains)
            continue;
          if (winding > 0.0f) {
            indices.push_back(ia);
            indices.push_back(ib);
            indices.push_back(ic);
          } else {
            indices.push_back(ic);
            indices.push_back(ib);
            indices.push_back(ia);
          }
          order.erase(order.begin() + static_cast<isize>(i));
          clipped = true;
          break;
        }
        if (!clipped)
          break;
      }
      if (order.size() == 3) {
        if (winding > 0.0f) {
          indices.push_back(order[0]);
          indices.push_back(order[1]);
          indices.push_back(order[2]);
        } else {
          indices.push_back(order[2]);
          indices.push_back(order[1]);
          indices.push_back(order[0]);
        }
      }
    }
    [[nodiscard]] inline char32_t decode_utf8(const char*& it, const char* end) noexcept {
      if (it >= end)
        return 0;
      auto b0 = static_cast<unsigned char>(*it++);
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
        return b0;
      }
      while (extra-- && it < end) {
        auto b = static_cast<unsigned char>(*it++);
        cp = (cp << 6) | (b & 0x3F);
      }
      return cp;
    }

    // Apply text_style case-folding rules to a single ASCII codepoint.
    [[nodiscard]] inline char32_t apply_case(char32_t c, u32 flags) noexcept {
      if ((flags & text_uppercase) && c >= U'a' && c <= U'z')
        return c - 0x20;
      if ((flags & text_lowercase) && c >= U'A' && c <= U'Z')
        return c + 0x20;
      return c;
    }
  } // namespace

  path_2d& path_2d::move_to(float x, float y) {
    commands.push_back(path_cmd::move);
    params.insert(params.end(), {x, y});
    return *this;
  }

  path_2d& path_2d::line_to(float x, float y) {
    commands.push_back(path_cmd::line);
    params.insert(params.end(), {x, y});
    return *this;
  }

  path_2d& path_2d::quad_to(float cx, float cy, float x, float y) {
    commands.push_back(path_cmd::quad);
    params.insert(params.end(), {cx, cy, x, y});
    return *this;
  }

  path_2d& path_2d::cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    commands.push_back(path_cmd::cubic);
    params.insert(params.end(), {c1x, c1y, c2x, c2y, x, y});
    return *this;
  }

  path_2d& path_2d::arc(float cx, float cy, float radius, float start_angle, float end_angle,
                        bool ccw) {
    commands.push_back(path_cmd::arc);
    params.insert(params.end(), {cx, cy, radius, start_angle, end_angle, ccw ? 1.0f : 0.0f});
    return *this;
  }

  path_2d& path_2d::close() {
    commands.push_back(path_cmd::close);
    return *this;
  }

  void path_2d::reset() {
    commands.clear();
    params.clear();
  }

  // ---------------------------------------------------------------------------
  // Lines
  // ---------------------------------------------------------------------------
  void draw_line(command_buffer& r, math::vec4 src, math::vec4 dst, const color_list<2>& color,
                 float thickness) {
    if (thickness <= 0.0f) [[likely]] {
      auto v = r.allocate_list(2, vertex_topology::line);
      v[0] = make_vertex4(src, {}, null_texture, color[0]);
      v[1] = make_vertex4(dst, {}, null_texture, color[1]);
      return;
    }
    if (src.w > 0.0f) {
      // World-thick: build perpendicular basis from the line direction.
      math::vec3 d{dst.x - src.x, dst.y - src.y, dst.z - src.z};
      math::mat4x4 rot = math::direction_to_matrix(d) * (thickness * 0.5f);
      const math::vec4 x = rot[0]; // perpendicular #1
      const math::vec4 y = rot[1]; // perpendicular #2

      auto [v, i] = r.allocate(8, 36, vertex_topology::triangle);
      // L0..L3
      v[0] = make_vertex4(src - x + y, {}, null_texture, color[0]);
      v[1] = make_vertex4(src - x - y, {}, null_texture, color[0]);
      v[2] = make_vertex4(src + x + y, {}, null_texture, color[0]);
      v[3] = make_vertex4(src + x - y, {}, null_texture, color[0]);
      // H0..H3
      v[4] = make_vertex4(dst - x + y, {}, null_texture, color[1]);
      v[5] = make_vertex4(dst - x - y, {}, null_texture, color[1]);
      v[6] = make_vertex4(dst + x + y, {}, null_texture, color[1]);
      v[7] = make_vertex4(dst + x - y, {}, null_texture, color[1]);

      // 12 triangles, identical to the gfwx index pattern.
      static constexpr u32 kIdx[36] = {
          1, 0, 5, 0, 5, 4, 5, 4, 7, 4, 7, 6, 7, 6, 3, 6, 3, 2,
          4, 6, 0, 6, 0, 2, 0, 2, 1, 2, 1, 3, 1, 3, 5, 3, 5, 7,
      };
      for (int n = 0; n != 36; ++n)
        i[n] += kIdx[n];
      return;
    }
    // Screen-thick line: 4-vertex strip.
    auto v = r.allocate_strip(4, vertex_topology::triangle);
    math::vec4 dir{dst.x - src.x, dst.y - src.y, 0.0f, 0.0f};
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len > 0.0f)
      dir = dir * (1.0f / len);
    math::vec4 k = dir * (thickness * 0.5f);
    math::vec4 c{k.y, -k.x, 0.0f, 0.0f};
    v[0] = make_vertex4(src - k - c, {}, null_texture, color[0]);
    v[1] = make_vertex4(src - k + c, {}, null_texture, color[0]);
    v[2] = make_vertex4(dst + k - c, {}, null_texture, color[1]);
    v[3] = make_vertex4(dst + k + c, {}, null_texture, color[1]);
  }

  void draw_line(command_buffer& r, math::vec4 src, math::vec4 dst, r8g8b8a8 color,
                 float thickness) {
    draw_line(r, src, dst, color_list<2>{color, color}, thickness);
  }

  void draw_line(command_buffer& r, std::span<const math::vec4> points,
                 std::span<const r8g8b8a8> colors, float thickness) {
    if (points.size() <= 1) [[unlikely]]
      return;
    if (thickness <= 0.0f) [[likely]] {
      auto v = r.allocate_strip(points.size(), vertex_topology::line);
      for (usize n = 0; n != points.size(); ++n) {
        const r8g8b8a8 c =
            n < colors.size() ? colors[n] : (colors.empty() ? r8g8b8a8{} : colors.back());
        v[n] = make_vertex4(points[n], {}, null_texture, c);
      }
      return;
    }
    for (usize i = 0; i + 1 < points.size(); ++i) {
      const r8g8b8a8 ca = i < colors.size() ? colors[i] : r8g8b8a8{};
      const r8g8b8a8 cb = i + 1 < colors.size() ? colors[i + 1] : ca;
      draw_line(r, points[i], points[i + 1], color_list<2>{ca, cb}, thickness);
    }
  }

  void draw_line(command_buffer& r, std::span<const math::vec4> points, r8g8b8a8 color,
                 float thickness) {
    if (points.size() <= 1) [[unlikely]]
      return;
    if (thickness <= 0.0f) [[likely]] {
      auto v = r.allocate_strip(points.size(), vertex_topology::line);
      for (usize n = 0; n != points.size(); ++n)
        v[n] = make_vertex4(points[n], {}, null_texture, color);
      return;
    }
    for (usize i = 0; i + 1 < points.size(); ++i)
      draw_line(r, points[i], points[i + 1], color, thickness);
  }

  // ---------------------------------------------------------------------------
  // Triangles & quads
  // ---------------------------------------------------------------------------
  void draw_triangle(command_buffer& r, math::vec4 a, math::vec4 b, math::vec4 c,
                     const color_list<3>& color, float thickness) {
    const math::vec4 pts[4] = {a, b, c, a};
    const r8g8b8a8 cols[4] = {color[0], color[1], color[2], color[0]};
    draw_line(r, std::span{pts, 4}, std::span{cols, 4}, thickness);
  }

  void draw_triangle(command_buffer& r, math::vec4 a, math::vec4 b, math::vec4 c, r8g8b8a8 color,
                     float thickness) {
    draw_triangle(r, a, b, c, color_list<3>{color, color, color}, thickness);
  }

  void fill_triangle(command_buffer& r, math::vec4 a, math::vec4 b, math::vec4 c,
                     const color_list<3>& color) {
    auto v = r.allocate_strip(3, vertex_topology::triangle);
    v[0] = make_vertex4(a, {}, null_texture, color[0]);
    v[1] = make_vertex4(b, {}, null_texture, color[1]);
    v[2] = make_vertex4(c, {}, null_texture, color[2]);
  }

  void fill_triangle(command_buffer& r, math::vec4 a, math::vec4 b, math::vec4 c, r8g8b8a8 color) {
    fill_triangle(r, a, b, c, color_list<3>{color, color, color});
  }

  void draw_quad(command_buffer& r, math::vec4 p1, math::vec4 p2, math::vec4 p3, math::vec4 p4,
                 const color_list<4>& color, float thickness) {
    const math::vec4 pts[5] = {p1, p2, p4, p3, p1};
    const r8g8b8a8 cols[5] = {color[0], color[1], color[3], color[2], color[0]};
    draw_line(r, std::span{pts, 5}, std::span{cols, 5}, thickness);
  }

  void fill_quad(command_buffer& r, math::vec4 p1, math::vec4 p2, math::vec4 p3, math::vec4 p4,
                 const color_list<4>& color, const texture_info& ti) {
    auto v = r.allocate_strip(4, vertex_topology::triangle);
    v[0] = make_vertex4(p1, {ti.src.x, ti.src.y}, ti.texture, color[0]);
    v[1] = make_vertex4(p2, {ti.dst.x, ti.src.y}, ti.texture, color[1]);
    v[2] = make_vertex4(p3, {ti.src.x, ti.dst.y}, ti.texture, color[2]);
    v[3] = make_vertex4(p4, {ti.dst.x, ti.dst.y}, ti.texture, color[3]);
  }

  void draw_quad(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color,
                 float thickness) {
    const math::vec4 p1 = apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 p2 = apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 p3 = apply_no_w(transform, {0.0f, 1.0f, 0.0f, 0.0f});
    const math::vec4 p4 = apply_no_w(transform, {1.0f, 1.0f, 0.0f, 0.0f});
    draw_quad(r, p1, p2, p3, p4, color_list<4>{color, color, color, color}, thickness);
  }

  void fill_quad(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color,
                 const texture_info& ti) {
    const math::vec4 p1 = apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 p2 = apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 p3 = apply_no_w(transform, {0.0f, 1.0f, 0.0f, 0.0f});
    const math::vec4 p4 = apply_no_w(transform, {1.0f, 1.0f, 0.0f, 0.0f});
    fill_quad(r, p1, p2, p3, p4, color_list<4>{color, color, color, color}, ti);
  }

  // ---- Rounded quad / rect ----
  void draw_quad_rounded(command_buffer& r, math::vec4 p1, math::vec4 p2, math::vec4 p3,
                         math::vec4 p4, const optional_list<float, 4>& rnd,
                         const color_list<4>& color, float thickness) {
    constexpr usize kPointCount = usize(kRoundedEdgeCount) * 4 + 1;
    math::vec4 pos_list[kPointCount];
    r8g8b8a8 col_list[kPointCount];
    usize it = 0;

    auto arc2 = [&](math::vec4 a, r8g8b8a8 ca, math::vec4 c, r8g8b8a8 cc, math::vec4 b, r8g8b8a8 cb,
                    float radius) {
      const math::vec4 ac = a - c;
      const math::vec4 bc = b - c;
      float r1 = radius / std::sqrt(ac.x * ac.x + ac.y * ac.y + ac.z * ac.z + ac.w * ac.w);
      float r2 = radius / std::sqrt(bc.x * bc.x + bc.y * bc.y + bc.z * bc.z + bc.w * bc.w);
      for (int i = 0; i != kRoundedEdgeCount; ++i) {
        auto [sv, cv] = math::fsincos(float(i) * kRoundedEdgeStepToRadian);
        float ba = (1.0f - sv) * r1;
        float bb = (1.0f - cv) * r2;
        float bc_ = 1.0f - (ba + bb);
        pos_list[it] = a * ba + b * bb + c * bc_;
        // bilinear blend of three corner colours.
        col_list[it] = lerp_color(lerp_color(cc, ca, ba), cb, bb);
        ++it;
      }
    };

    arc2(p3, color[2], p1, color[0], p2, color[1], rnd[0]);
    arc2(p1, color[0], p2, color[1], p4, color[3], rnd[1]);
    arc2(p2, color[1], p4, color[3], p3, color[2], rnd[2]);
    arc2(p4, color[3], p3, color[2], p1, color[0], rnd[3]);

    pos_list[kPointCount - 1] = pos_list[0];
    col_list[kPointCount - 1] = col_list[0];

    draw_line(r, std::span<const math::vec4>{pos_list, kPointCount},
              std::span<const r8g8b8a8>{col_list, kPointCount}, thickness);
  }

  void fill_quad_rounded(command_buffer& r, math::vec4 p1, math::vec4 p2, math::vec4 p3,
                         math::vec4 p4, const optional_list<float, 4>& rnd,
                         const color_list<4>& color, const texture_info& ti) {
    constexpr usize kVertexCount = usize(kRoundedEdgeCount) * 4;
    constexpr usize kIndexCount = (kVertexCount - 2) * 3;

    auto [vt, id] = r.allocate(kVertexCount, kIndexCount, vertex_topology::triangle);

    const math::vec2 uv[4] = {
        {ti.src.x, ti.src.y},
        {ti.dst.x, ti.src.y},
        {ti.src.x, ti.dst.y},
        {ti.dst.x, ti.dst.y},
    };

    usize cursor = 0;
    auto arc2 = [&](math::vec4 a, math::vec2 ua, r8g8b8a8 ca, math::vec4 c, math::vec2 uc,
                    r8g8b8a8 cc, math::vec4 b, math::vec2 ub, r8g8b8a8 cb, float radius) {
      const math::vec4 ac = a - c;
      const math::vec4 bc = b - c;
      float r1 = radius / std::sqrt(ac.x * ac.x + ac.y * ac.y + ac.z * ac.z + ac.w * ac.w);
      float r2 = radius / std::sqrt(bc.x * bc.x + bc.y * bc.y + bc.z * bc.z + bc.w * bc.w);
      for (int i = 0; i != kRoundedEdgeCount; ++i) {
        auto [sv, cv] = math::fsincos(float(i) * kRoundedEdgeStepToRadian);
        float ba = (1.0f - sv) * r1;
        float bb = (1.0f - cv) * r2;
        float bc_ = 1.0f - (ba + bb);
        math::vec4 p = a * ba + b * bb + c * bc_;
        math::vec2 u{ua.x * ba + ub.x * bb + uc.x * bc_, ua.y * ba + ub.y * bb + uc.y * bc_};
        r8g8b8a8 col = lerp_color(lerp_color(cc, ca, ba), cb, bb);
        vt[cursor++] = make_vertex4(p, u, ti.texture, col);
      }
    };

    arc2(p3, uv[2], color[2], p1, uv[0], color[0], p2, uv[1], color[1], rnd[0]);
    arc2(p1, uv[0], color[0], p2, uv[1], color[1], p4, uv[3], color[3], rnd[1]);
    arc2(p2, uv[1], color[1], p4, uv[3], color[3], p3, uv[2], color[2], rnd[2]);
    arc2(p4, uv[3], color[3], p3, uv[2], color[2], p1, uv[0], color[0], rnd[3]);

    for (usize i = 2; i != kVertexCount; ++i) {
      id[(i - 2) * 3 + 0] += 0;
      id[(i - 2) * 3 + 1] += u32(i - 1);
      id[(i - 2) * 3 + 2] += u32(i);
    }
  }

  // ---- Rect (transform form) ----
  void draw_rect(command_buffer& r, const math::mat4x4& transform, float shift,
                 const color_list<4>& color, float thickness) {
    draw_quad(r, apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {0.0f - shift, 1.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {1.0f - shift, 1.0f, 0.0f, 0.0f}), color, thickness);
  }

  void fill_rect(command_buffer& r, const math::mat4x4& transform, float shift,
                 const color_list<4>& color, const texture_info& ti) {
    fill_quad(r, apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {0.0f - shift, 1.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {1.0f - shift, 1.0f, 0.0f, 0.0f}), color, ti);
  }

  void draw_rect_rounded(command_buffer& r, const math::mat4x4& transform,
                         const optional_list<float, 4>& rnd, float shift,
                         const color_list<4>& color, float thickness) {
    draw_quad_rounded(r, apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f}),
                      apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f}),
                      apply_no_w(transform, {0.0f - shift, 1.0f, 0.0f, 0.0f}),
                      apply_no_w(transform, {1.0f - shift, 1.0f, 0.0f, 0.0f}), rnd, color,
                      thickness);
  }

  void fill_rect_rounded(command_buffer& r, const math::mat4x4& transform,
                         const optional_list<float, 4>& rnd, float shift,
                         const color_list<4>& color, const texture_info& ti) {
    fill_quad_rounded(r, apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f}),
                      apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f}),
                      apply_no_w(transform, {0.0f - shift, 1.0f, 0.0f, 0.0f}),
                      apply_no_w(transform, {1.0f - shift, 1.0f, 0.0f, 0.0f}), rnd, color, ti);
  }

  void draw_rect(command_buffer& r, math::vec2 at, math::vec2 size, float depth, r8g8b8a8 color,
                 float thickness) {
    draw_rect(r, make_screen_transform(at, size, depth), 0.0f,
              color_list<4>{color, color, color, color}, thickness);
  }

  void fill_rect(command_buffer& r, math::vec2 at, math::vec2 size, float depth, r8g8b8a8 color,
                 const texture_info& ti) {
    fill_rect(r, make_screen_transform(at, size, depth), 0.0f,
              color_list<4>{color, color, color, color}, ti);
  }

  void fill_rect(command_buffer& r, math::vec2 at, math::vec2 size, float depth,
                 const paint_value& paint) {
    if (paint.kind == paint_kind::solid) {
      fill_rect(r, at, size, depth, paint.color);
      return;
    }
    const math::vec4 p1{at.x, at.y, depth, 0.0f};
    const math::vec4 p2{at.x + size.x, at.y, depth, 0.0f};
    const math::vec4 p3{at.x, at.y + size.y, depth, 0.0f};
    const math::vec4 p4{at.x + size.x, at.y + size.y, depth, 0.0f};
    const texture_id tag = paint_texture_id(paint);
    auto v = r.allocate_strip(4, vertex_topology::triangle);
    v[0] = make_vertex4(p1, {}, tag, sample_paint(paint, at));
    v[1] = make_vertex4(p2, {}, tag, sample_paint(paint, {at.x + size.x, at.y}));
    v[2] = make_vertex4(p3, {}, tag, sample_paint(paint, {at.x, at.y + size.y}));
    v[3] = make_vertex4(p4, {}, tag, sample_paint(paint, {at.x + size.x, at.y + size.y}));
  }

  void fill_rect_rounded(command_buffer& r, const math::mat4x4& transform,
                         const optional_list<float, 4>& rnd, float shift,
                         const paint_value& paint) {
    if (paint.kind == paint_kind::solid) {
      fill_rect_rounded(r, transform, rnd, shift,
                        color_list<4>{paint.color, paint.color, paint.color, paint.color});
      return;
    }
    constexpr usize kVertexCount = usize(kRoundedEdgeCount) * 4;
    constexpr usize kIndexCount = (kVertexCount - 2) * 3;
    auto [vt, id] = r.allocate(kVertexCount, kIndexCount, vertex_topology::triangle);
    const texture_id tag = paint_texture_id(paint);
    usize cursor = 0;
    auto arc2 = [&](math::vec4 a, math::vec4 c, math::vec4 b, float radius) {
      const math::vec4 ac = a - c;
      const math::vec4 bc = b - c;
      float r1 = radius / std::sqrt(ac.x * ac.x + ac.y * ac.y + ac.z * ac.z + ac.w * ac.w);
      float r2 = radius / std::sqrt(bc.x * bc.x + bc.y * bc.y + bc.z * bc.z + bc.w * bc.w);
      for (int i = 0; i != kRoundedEdgeCount; ++i) {
        auto [sv, cv] = math::fsincos(float(i) * kRoundedEdgeStepToRadian);
        float ba = (1.0f - sv) * r1;
        float bb = (1.0f - cv) * r2;
        float bc_ = 1.0f - (ba + bb);
        math::vec4 p = a * ba + b * bb + c * bc_;
        vt[cursor++] = make_vertex4(p, {}, tag, sample_paint(paint, {p.x, p.y}));
      }
    };
    const math::vec4 p1 = apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 p2 = apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 p3 = apply_no_w(transform, {0.0f - shift, 1.0f, 0.0f, 0.0f});
    const math::vec4 p4 = apply_no_w(transform, {1.0f - shift, 1.0f, 0.0f, 0.0f});
    arc2(p3, p1, p2, rnd[0]);
    arc2(p1, p2, p4, rnd[1]);
    arc2(p2, p4, p3, rnd[2]);
    arc2(p4, p3, p1, rnd[3]);
    for (usize i = 2; i != kVertexCount; ++i) {
      id[(i - 2) * 3 + 0] += 0;
      id[(i - 2) * 3 + 1] += u32(i - 1);
      id[(i - 2) * 3 + 2] += u32(i);
    }
  }

  // ---------------------------------------------------------------------------
  // Ellipse / cylinder / sphere
  // ---------------------------------------------------------------------------
  void draw_ellipse(command_buffer& r, const math::mat4x4& transform, const color_list<2>& color,
                    float thickness_per_radius, float percentage, usize num_edges, texture_id tx) {
    const usize num_eff = usize(math::ftrunc(float(num_edges + 1) * percentage));
    if (num_eff <= 1)
      return;
    const float edge_to_rad = -(2.0f * math::pi) / float(num_edges);

    if (thickness_per_radius > 0.0f) {
      auto vt = r.allocate_strip(num_eff * 2, vertex_topology::triangle);
      for (usize i = 0; i != num_eff; ++i) {
        auto [s, c] = math::fsincos(float(i) * edge_to_rad + math::pi);
        math::vec4 dir{s, c, 0.0f, 0.0f};
        vt[i * 2 + 0] =
            make_vertex4(apply_no_w(transform, dir), {std::fabs(s), 1.0f}, tx, color[1]);
        vt[i * 2 + 1] = make_vertex4(apply_no_w(transform, dir * (1.0f + thickness_per_radius)),
                                     {std::fabs(s), 0.0f}, tx, color[0]);
      }
      return;
    }

    auto vt = r.allocate_strip(num_eff, vertex_topology::line);
    for (usize i = 0; i != num_eff; ++i) {
      auto [s, c] = math::fsincos(float(i) * edge_to_rad + math::pi);
      vt[i] = make_vertex4(apply_no_w(transform, math::vec4{s, c, 0.0f, 0.0f}), {}, null_texture,
                           color[0]);
    }
  }

  void fill_ellipse(command_buffer& r, const math::mat4x4& transform, const color_list<2>& color,
                    float percentage, usize num_edges, texture_id tx) {
    const usize num_eff = usize(math::ftrunc(float(num_edges + 1) * percentage));
    if (num_eff <= 1)
      return;
    const float edge_to_rad = -(2.0f * math::pi) / float(num_edges);

    auto [vt, id] = r.allocate(num_eff + 1, (num_eff - 1) * 3, vertex_topology::triangle);
    // Centre.
    vt[0] = make_vertex4(transform[3], {0.0f, 1.0f}, tx, color[1]);
    for (usize i = 0; i != num_eff; ++i) {
      auto [s, c] = math::fsincos(float(i) * edge_to_rad + math::pi);
      vt[i + 1] = make_vertex4(apply_no_w(transform, math::vec4{s, c, 0.0f, 0.0f}),
                               {std::fabs(s), 0.0f}, tx, color[0]);
    }
    for (usize i = 1; i != num_eff; ++i) {
      *id++ += 0;
      *id++ += u32(i);
      *id++ += u32(i + 1);
    }
  }

  void draw_ellipse(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color,
                    float thickness_per_radius, float percentage, usize num_edges, texture_id tx) {
    draw_ellipse(r, transform, color_list<2>{color, color}, thickness_per_radius, percentage,
                 num_edges, tx);
  }

  void fill_ellipse(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color,
                    float percentage, usize num_edges, texture_id tx) {
    fill_ellipse(r, transform, color_list<2>{color, color}, percentage, num_edges, tx);
  }

  void fill_cylinder(command_buffer& r, const math::mat4x4& transform, const color_list<2>& color,
                     float percentage, usize num_edges) {
    const usize num_eff = usize(math::ftrunc(float(num_edges + 1) * percentage));
    if (num_eff <= 1)
      return;
    const float edge_to_rad = -(2.0f * math::pi) / float(num_edges);

    auto vt = r.allocate_strip(num_eff * 2, vertex_topology::triangle);
    for (usize i = 0; i != num_eff; ++i) {
      auto [s, c] = math::fsincos(float(i) * edge_to_rad + math::pi);
      vt[i * 2 + 0] = make_vertex4(apply_no_w(transform, math::vec4{s, c, 0.0f, 0.0f}), {},
                                   null_texture, color[0]);
      vt[i * 2 + 1] = make_vertex4(apply_no_w(transform, math::vec4{s, c, 1.0f, 0.0f}), {},
                                   null_texture, color[1]);
    }
  }

  void fill_sphere(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color, float perc_x,
                   float perc_y, usize num_edges) {
    const usize nx = usize(math::ftrunc(float(num_edges + 1) * perc_x));
    const usize ny = usize(math::ftrunc(float(num_edges + 1) * perc_y));
    if (nx <= 1 || ny <= 1)
      return;

    const float edge_to_2pi = -(2.0f * math::pi) / float(num_edges);
    const float edge_to_pi = -(math::pi) / float(num_edges);

    auto [vt, id] = r.allocate(nx * ny, (ny - 1) * (nx - 1) * 6, vertex_topology::triangle);

    for (usize y = 0; y != ny; ++y) {
      auto [spitch, cpitch] = math::fsincos(float(y) * edge_to_pi - math::pi * 0.5f);
      for (usize x = 0; x != nx; ++x) {
        auto [syaw, cyaw] = math::fsincos(float(x) * edge_to_2pi);
        const float dy = std::fabs((float(y) / float(ny)) - 0.5f) * 2.0f;
        vt[y * nx + x] = make_vertex4(
            apply_no_w(transform, math::vec4{cpitch * cyaw, cpitch * syaw, -spitch, 0.0f}), {},
            null_texture, color.darken(dy));
      }
    }

    for (usize y = 0; y + 1 != ny; ++y) {
      for (usize x = 0; x + 1 != nx; ++x) {
        *id++ += u32(x + y * nx);
        *id++ += u32(x + (y + 1) * nx);
        *id++ += u32((x + 1) + (y + 1) * nx);
        *id++ += u32(x + y * nx);
        *id++ += u32((x + 1) + y * nx);
        *id++ += u32((x + 1) + (y + 1) * nx);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Pyramid / box / cbox
  // ---------------------------------------------------------------------------
  void fill_pyramid(command_buffer& r, const math::mat4x4& transform, const color_list<2>& color) {
    auto [vt, id] = r.allocate(5, 18, vertex_topology::triangle);
    vt[0] =
        make_vertex4(apply_no_w(transform, {-0.5f, -0.5f, 0.0f, 0.0f}), {}, null_texture, color[0]);
    vt[1] =
        make_vertex4(apply_no_w(transform, {+0.5f, -0.5f, 0.0f, 0.0f}), {}, null_texture, color[0]);
    vt[2] =
        make_vertex4(apply_no_w(transform, {-0.5f, +0.5f, 0.0f, 0.0f}), {}, null_texture, color[0]);
    vt[3] =
        make_vertex4(apply_no_w(transform, {+0.5f, +0.5f, 0.0f, 0.0f}), {}, null_texture, color[0]);
    vt[4] =
        make_vertex4(apply_no_w(transform, {0.0f, 0.0f, 1.0f, 0.0f}), {}, null_texture, color[1]);

    static constexpr u32 kIdx[18] = {
        0, 4, 2, 4, 2, 3, 3, 4, 1, 4, 1, 0, 0, 1, 2, 1, 2, 3,
    };
    for (int n = 0; n != 18; ++n)
      id[n] += kIdx[n];
  }

  void draw_pyramid(command_buffer& r, const math::mat4x4& transform, const color_list<2>& color,
                    float thickness) {
    const math::vec4 p0 = apply_no_w(transform, {-0.5f, -0.5f, 0.0f, 0.0f});
    const math::vec4 p1 = apply_no_w(transform, {+0.5f, -0.5f, 0.0f, 0.0f});
    const math::vec4 p2 = apply_no_w(transform, {-0.5f, +0.5f, 0.0f, 0.0f});
    const math::vec4 p3 = apply_no_w(transform, {+0.5f, +0.5f, 0.0f, 0.0f});
    const math::vec4 ap_ = apply_no_w(transform, {0.0f, 0.0f, 1.0f, 0.0f});

    {
      const math::vec4 base[5] = {p0, p1, p3, p2, p0};
      draw_line(r, std::span{base, 5}, color[0], thickness);
    }
    {
      const math::vec4 a[3] = {p0, ap_, p3};
      const r8g8b8a8 ca[3] = {color[0], color[1], color[0]};
      draw_line(r, std::span{a, 3}, std::span{ca, 3}, thickness);
    }
    {
      const math::vec4 a[3] = {p1, ap_, p2};
      const r8g8b8a8 ca[3] = {color[0], color[1], color[0]};
      draw_line(r, std::span{a, 3}, std::span{ca, 3}, thickness);
    }
  }

  void fill_pyramid(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color) {
    fill_pyramid(r, transform, color_list<2>{color, color});
  }

  void draw_pyramid(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color,
                    float thickness) {
    draw_pyramid(r, transform, color_list<2>{color, color}, thickness);
  }

  namespace {
    // Shared 12-triangle (36 index) topology for box / cbox fills.
    static constexpr u32 kBoxIdx[36] = {
        1, 0, 5, 0, 5, 4, 5, 4, 7, 4, 7, 6, 7, 6, 3, 6, 3, 2,
        4, 6, 0, 6, 0, 2, 0, 2, 1, 2, 1, 3, 1, 3, 5, 3, 5, 7,
    };
  } // namespace

  void fill_box(command_buffer& r, const math::mat4x4& transform, const color_list<8>& color) {
    auto [v, i] = r.allocate(8, 36, vertex_topology::triangle);
    const math::vec4 l0 = apply_no_w(transform, {0.0f, 1.0f, 0.0f, 0.0f});
    const math::vec4 l1 = apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 l2 = apply_no_w(transform, {1.0f, 1.0f, 0.0f, 0.0f});
    const math::vec4 l3 = apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 up = transform[2];
    v[0] = make_vertex4(l0, {}, null_texture, color[1]);
    v[1] = make_vertex4(l1, {}, null_texture, color[0]);
    v[2] = make_vertex4(l2, {}, null_texture, color[3]);
    v[3] = make_vertex4(l3, {}, null_texture, color[2]);
    v[4] = make_vertex4(l0 + up, {}, null_texture, color[1 + 4]);
    v[5] = make_vertex4(l1 + up, {}, null_texture, color[0 + 4]);
    v[6] = make_vertex4(l2 + up, {}, null_texture, color[3 + 4]);
    v[7] = make_vertex4(l3 + up, {}, null_texture, color[2 + 4]);
    for (int n = 0; n != 36; ++n)
      i[n] += kBoxIdx[n];
  }

  void draw_box(command_buffer& r, const math::mat4x4& transform, const color_list<8>& color,
                float thickness) {
    const math::vec4 l0 = apply_no_w(transform, {0.0f, 1.0f, 0.0f, 0.0f});
    const math::vec4 l1 = apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 l2 = apply_no_w(transform, {1.0f, 1.0f, 0.0f, 0.0f});
    const math::vec4 l3 = apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f});
    const math::vec4 up = transform[2];
    const math::vec4 h0 = l0 + up;
    const math::vec4 h1 = l1 + up;
    const math::vec4 h2 = l2 + up;
    const math::vec4 h3 = l3 + up;
    const r8g8b8a8 lc[4] = {color[1], color[0], color[3], color[2]};
    const r8g8b8a8 hc[4] = {color[1 + 4], color[0 + 4], color[3 + 4], color[2 + 4]};

    {
      const math::vec4 ring[5] = {h0, h1, h3, h2, h0};
      const r8g8b8a8 cring[5] = {hc[0], hc[1], hc[2], hc[3], hc[0]};
      draw_line(r, std::span{ring, 5}, std::span{cring, 5}, thickness);
    }
    {
      const math::vec4 ring[5] = {l0, l1, l3, l2, l0};
      const r8g8b8a8 cring[5] = {lc[0], lc[1], lc[2], lc[3], lc[0]};
      draw_line(r, std::span{ring, 5}, std::span{cring, 5}, thickness);
    }
    draw_line(r, l0, h0, color_list<2>{lc[0], hc[0]}, thickness);
    draw_line(r, l1, h1, color_list<2>{lc[1], hc[1]}, thickness);
    draw_line(r, l2, h2, color_list<2>{lc[2], hc[2]}, thickness);
    draw_line(r, l3, h3, color_list<2>{lc[3], hc[3]}, thickness);
  }

  void fill_cbox(command_buffer& r, const math::mat4x4& transform, const color_list<8>& color) {
    auto [v, i] = r.allocate(8, 36, vertex_topology::triangle);
    const math::vec4 l0 = apply_no_w(transform, {-1.0f, +1.0f, -1.0f, 0.0f});
    const math::vec4 l1 = apply_no_w(transform, {-1.0f, -1.0f, -1.0f, 0.0f});
    const math::vec4 l2 = apply_no_w(transform, {+1.0f, +1.0f, -1.0f, 0.0f});
    const math::vec4 l3 = apply_no_w(transform, {+1.0f, -1.0f, -1.0f, 0.0f});
    const math::vec4 up2 = transform[2] * 2.0f;
    v[0] = make_vertex4(l0, {}, null_texture, color[1]);
    v[1] = make_vertex4(l1, {}, null_texture, color[0]);
    v[2] = make_vertex4(l2, {}, null_texture, color[3]);
    v[3] = make_vertex4(l3, {}, null_texture, color[2]);
    v[4] = make_vertex4(l0 + up2, {}, null_texture, color[1 + 4]);
    v[5] = make_vertex4(l1 + up2, {}, null_texture, color[0 + 4]);
    v[6] = make_vertex4(l2 + up2, {}, null_texture, color[3 + 4]);
    v[7] = make_vertex4(l3 + up2, {}, null_texture, color[2 + 4]);
    for (int n = 0; n != 36; ++n)
      i[n] += kBoxIdx[n];
  }

  void draw_cbox(command_buffer& r, const math::mat4x4& transform, const color_list<8>& color,
                 float thickness) {
    const math::vec4 l0 = apply_no_w(transform, {-1.0f, +1.0f, -1.0f, 0.0f});
    const math::vec4 l1 = apply_no_w(transform, {-1.0f, -1.0f, -1.0f, 0.0f});
    const math::vec4 l2 = apply_no_w(transform, {+1.0f, +1.0f, -1.0f, 0.0f});
    const math::vec4 l3 = apply_no_w(transform, {+1.0f, -1.0f, -1.0f, 0.0f});
    const math::vec4 up2 = transform[2] * 2.0f;
    const math::vec4 h0 = l0 + up2;
    const math::vec4 h1 = l1 + up2;
    const math::vec4 h2 = l2 + up2;
    const math::vec4 h3 = l3 + up2;
    const r8g8b8a8 lc[4] = {color[1], color[0], color[3], color[2]};
    const r8g8b8a8 hc[4] = {color[1 + 4], color[0 + 4], color[3 + 4], color[2 + 4]};

    {
      const math::vec4 ring[5] = {h0, h1, h3, h2, h0};
      const r8g8b8a8 cring[5] = {hc[0], hc[1], hc[2], hc[3], hc[0]};
      draw_line(r, std::span{ring, 5}, std::span{cring, 5}, thickness);
    }
    {
      const math::vec4 ring[5] = {l0, l1, l3, l2, l0};
      const r8g8b8a8 cring[5] = {lc[0], lc[1], lc[2], lc[3], lc[0]};
      draw_line(r, std::span{ring, 5}, std::span{cring, 5}, thickness);
    }
    draw_line(r, l0, h0, color_list<2>{lc[0], hc[0]}, thickness);
    draw_line(r, l1, h1, color_list<2>{lc[1], hc[1]}, thickness);
    draw_line(r, l2, h2, color_list<2>{lc[2], hc[2]}, thickness);
    draw_line(r, l3, h3, color_list<2>{lc[3], hc[3]}, thickness);
  }

  void fill_box(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color) {
    fill_box(r, transform, color_list<8>{color, color, color, color, color, color, color, color});
  }
  void draw_box(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color, float thickness) {
    draw_box(r, transform, color_list<8>{color, color, color, color, color, color, color, color},
             thickness);
  }
  void fill_cbox(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color) {
    fill_cbox(r, transform, color_list<8>{color, color, color, color, color, color, color, color});
  }
  void draw_cbox(command_buffer& r, const math::mat4x4& transform, r8g8b8a8 color,
                 float thickness) {
    draw_cbox(r, transform, color_list<8>{color, color, color, color, color, color, color, color},
              thickness);
  }

  void fill_box(command_buffer& r, math::vec3 mn, math::vec3 mx, r8g8b8a8 color) {
    math::mat4x4 m{1.0f};
    m[0] = math::vec4{mx.x - mn.x, 0.0f, 0.0f, 0.0f};
    m[1] = math::vec4{0.0f, mx.y - mn.y, 0.0f, 0.0f};
    m[2] = math::vec4{0.0f, 0.0f, mx.z - mn.z, 0.0f};
    m[3] = math::vec4{mn.x, mn.y, mn.z, 0.0f};
    fill_box(r, m, color);
  }

  void draw_box(command_buffer& r, math::vec3 mn, math::vec3 mx, r8g8b8a8 color, float thickness) {
    math::mat4x4 m{1.0f};
    m[0] = math::vec4{mx.x - mn.x, 0.0f, 0.0f, 0.0f};
    m[1] = math::vec4{0.0f, mx.y - mn.y, 0.0f, 0.0f};
    m[2] = math::vec4{0.0f, 0.0f, mx.z - mn.z, 0.0f};
    m[3] = math::vec4{mn.x, mn.y, mn.z, 0.0f};
    draw_box(r, m, color, thickness);
  }

  void fill_path(command_buffer& r, const path_2d& path, const paint_value& paint, fill_rule,
                 float depth) {
    std::vector<flat_subpath> subpaths;
    flatten_path(path, subpaths);
    for (auto& sub : subpaths) {
      if (sub.points.size() >= 2 && math::length(sub.points.front() - sub.points.back()) < 0.001f)
        sub.points.pop_back();
      if (sub.points.size() < 3)
        continue;
      std::vector<u32> local_indices;
      triangulate_simple(sub.points, local_indices);
      if (local_indices.empty())
        continue;
      auto [vt, id] =
          r.allocate(sub.points.size(), local_indices.size(), vertex_topology::triangle);
      const texture_id tag = paint_texture_id(paint);
      for (usize i = 0; i < sub.points.size(); ++i) {
        const math::vec2 p = sub.points[i];
        vt[i] = make_vertex4({p.x, p.y, depth, 0.0f}, {}, tag, sample_paint(paint, p));
      }
      for (usize i = 0; i < local_indices.size(); ++i)
        id[i] += local_indices[i];
    }
  }

  void fill_path(command_buffer& r, const path_2d& path, r8g8b8a8 color, fill_rule rule,
                 float depth) {
    fill_path(r, path, paint_value::solid(color), rule, depth);
  }

  void stroke_path(command_buffer& r, const path_2d& path, const paint_value& paint,
                   float line_width, line_join join, line_cap, float depth) {
    if (line_width <= 0.0f)
      return;
    std::vector<flat_subpath> subpaths;
    flatten_path(path, subpaths);
    const float half = line_width * 0.5f;
    for (auto& sub : subpaths) {
      if (sub.points.size() < 2)
        continue;
      const bool closed =
          sub.closed || math::length(sub.points.front() - sub.points.back()) < 0.001f;
      if (closed && math::length(sub.points.front() - sub.points.back()) < 0.001f)
        sub.points.pop_back();
      const usize n = sub.points.size();
      if (n < 2)
        continue;
      const usize segment_count = closed ? n : n - 1;
      auto [vt, id] = r.allocate(segment_count * 4, segment_count * 6, vertex_topology::triangle);
      const texture_id tag = paint_texture_id(paint);
      for (usize i = 0; i < segment_count; ++i) {
        const math::vec2 a = sub.points[i];
        const math::vec2 b = sub.points[(i + 1) % n];
        math::vec2 d = b - a;
        const float len = math::length(d);
        if (len <= 0.0001f)
          continue;
        d *= 1.0f / len;
        const math::vec2 normal{-d.y * half, d.x * half};
        const usize base = i * 4;
        vt[base + 0] = make_vertex4({a.x - normal.x, a.y - normal.y, depth, 0.0f}, {}, tag,
                                    sample_paint(paint, a));
        vt[base + 1] = make_vertex4({a.x + normal.x, a.y + normal.y, depth, 0.0f}, {}, tag,
                                    sample_paint(paint, a));
        vt[base + 2] = make_vertex4({b.x - normal.x, b.y - normal.y, depth, 0.0f}, {}, tag,
                                    sample_paint(paint, b));
        vt[base + 3] = make_vertex4({b.x + normal.x, b.y + normal.y, depth, 0.0f}, {}, tag,
                                    sample_paint(paint, b));
        id[i * 6 + 0] += u32(base + 0);
        id[i * 6 + 1] += u32(base + 1);
        id[i * 6 + 2] += u32(base + 2);
        id[i * 6 + 3] += u32(base + 1);
        id[i * 6 + 4] += u32(base + 2);
        id[i * 6 + 5] += u32(base + 3);
      }
      if (join == line_join::round && n > 2) {
        for (usize i = 0; i < n; ++i)
          fill_ellipse(r,
                       make_screen_transform(sub.points[i] - math::vec2{half, half},
                                             {line_width, line_width}, depth),
                       sample_paint(paint, sub.points[i]), 1.0f, 12);
      }
    }
  }

  void stroke_path(command_buffer& r, const path_2d& path, r8g8b8a8 color, float line_width,
                   line_join join, line_cap cap, float depth) {
    stroke_path(r, path, paint_value::solid(color), line_width, join, cap, depth);
  }

  void draw_shadow_rect(command_buffer& r, float x, float y, float w, float h, float depth,
                        r8g8b8a8 color, float blur, float spread, float offset_x, float offset_y,
                        float screen_w, float screen_h) {
    const float sx = x + offset_x - spread;
    const float sy = y + offset_y - spread;
    const float sw = math::fmax(0.0f, w + spread * 2.0f);
    const float sh = math::fmax(0.0f, h + spread * 2.0f);
    if (sw <= 0.0f || sh <= 0.0f)
      return;
    if (blur > 0.0f && screen_w > 0.0f && screen_h > 0.0f) {
      blur_rect(r, {sx, sy}, {sw, sh}, depth, color, blur, {screen_w, screen_h});
      return;
    }
    fill_rect(r, {sx, sy}, {sw, sh}, depth, color);
  }

  void draw_shadow_rect_rounded(command_buffer& r, float x, float y, float w, float h,
                                const optional_list<float, 4>& rnd, float depth, r8g8b8a8 color,
                                float blur, float spread, float offset_x, float offset_y,
                                float screen_w, float screen_h) {
    if (blur > 0.0f) {
      draw_shadow_rect(r, x, y, w, h, depth, color, blur, spread, offset_x, offset_y, screen_w,
                       screen_h);
      return;
    }
    const float sx = x + offset_x - spread;
    const float sy = y + offset_y - spread;
    const float sw = math::fmax(0.0f, w + spread * 2.0f);
    const float sh = math::fmax(0.0f, h + spread * 2.0f);
    optional_list<float, 4> radii = rnd;
    for (usize i = 0; i < radii.size(); ++i)
      radii[i] = math::fmax(0.0f, radii[i] + spread);
    fill_rect_rounded(r, make_screen_transform({sx, sy}, {sw, sh}, depth), radii, 0.0f,
                      color_list<4>{color, color, color, color});
  }

  // ---------------------------------------------------------------------------
  // Text
  // ---------------------------------------------------------------------------

  math::vec2 calc_text(std::string_view text, const font_info& font, float pt) {
    text_style style{};
    style.pt = pt;
    return calc_text(text, style, font);
  }

  namespace {
    // Forward decls for the font module emitter (defined further below). We
    // forward-declare so calc_text/draw_text dispatching to them stays in
    // source order without restructuring the file.
    [[nodiscard]] math::vec2 measure_text_via_face(std::string_view text, font::Face& face,
                                                   text_style style, float dpr);
    [[nodiscard]] math::vec4 draw_text_via_face_screen(command_buffer& r, math::vec2 origin,
                                                       float depth, std::string_view text,
                                                       font::Face& face, text_style style,
                                                       float dpr);
  } // namespace

  math::vec2 calc_text(std::string_view text, text_style style, const font_info& font) {
    const float dpr = font::device_pixel_ratio();
    const float effective_pt = style.pt * dpr;
    if (auto face = font_face_for(font, effective_pt); face) {
      return measure_text_via_face(text, *face, style, dpr);
    }
    const font_variant_info& variant = resolve_font_variant(font, style.pt);
    const float line_height = variant.line_height;

    const char* it = text.data();
    const char* end = it + text.size();

    float x = 0.0f;
    float x_max = 0.0f;
    float y = 0.0f;
    u32 glyphs_on_line = 0;
    bool produced_anything = false;

    {
      const char* preload = text.data();
      const char* preload_end = preload + text.size();
      while (preload < preload_end) {
        char32_t cp = decode_utf8(preload, preload_end);
        if (cp == 0 || cp == U'\n')
          continue;
        (void)resolve_font_glyph(font, variant, apply_case(cp, style.flags));
      }
    }
    while (it < end) {
      char32_t cp = decode_utf8(it, end);
      if (cp == 0)
        continue;
      if (cp == U'\n') {
        x_max = math::fmax(x_max, x);
        y += line_height;
        x = 0.0f;
        glyphs_on_line = 0;
        continue;
      }
      cp = apply_case(cp, style.flags);
      glyph_info g = resolve_font_glyph(font, variant, cp);
      x += g.advance;
      ++glyphs_on_line;
      produced_anything = true;
      if (style.wrap_after != wrap_after_disabled && glyphs_on_line >= style.wrap_after) {
        x_max = math::fmax(x_max, x);
        y += line_height;
        x = 0.0f;
        glyphs_on_line = 0;
      }
    }
    if (glyphs_on_line || (!produced_anything)) {
      x_max = math::fmax(x_max, x);
      if (produced_anything)
        y += line_height;
    } else {
      // trailing wrap already accounted for the line.
    }

    return math::vec2{x_max, y};
  }

  // ---------------------------------------------------------------------------
  // Font module-based text emitter. Used by `draw_text` whenever the
  // `font_info` argument carries a `font::Face` (i.e. has been migrated to
  // the new pipeline). The legacy stb_truetype emitter remains for
  // back-compat until Phase 7 of FONT_STACK_OVERHAUL deletes it.
  // ---------------------------------------------------------------------------
  namespace {
    [[nodiscard]] font::ShapeOptions style_to_shape_opts(const text_style& s) {
      font::ShapeOptions opts{};
      opts.features.reserve(s.features.size());
      for (const auto& f : s.features) {
        font::Feature ff;
        for (usize i = 0; i < 4; ++i)
          ff.tag.chars[i] = f.first[i];
        ff.value = f.second;
        opts.features.push_back(ff);
      }
      opts.variations.reserve(s.variations.size());
      for (const auto& v : s.variations) {
        font::Variation fv;
        for (usize i = 0; i < 4; ++i)
          fv.tag.chars[i] = v.first[i];
        fv.value = v.second;
        opts.variations.push_back(fv);
      }
      return opts;
    }

    // Emit a shaped run produced by font::Shaper into the command buffer.
    // `pen_fb` is the framebuffer-pixel pen (i.e. logical × dpr); the face's
    // metrics, glyph dimensions, and shaper advances are all in framebuffer
    // pixels because the face was loaded at `pt × dpr`. We snap to integer
    // framebuffer pixels for crisp glyph alignment, drive the cache's
    // subpixel bin from the framebuffer fractional, and emit quads scaled
    // back into logical pixels by `1 / dpr`.
    [[nodiscard]] math::vec2 emit_shaped_run_screen(command_buffer& r, math::vec2 pen_fb,
                                                    float depth, float dpr, font::Face& face,
                                                    const font::ShapeRun& run, r8g8b8a8 color) {
      auto& cache = font::shared_glyph_cache();
      const float inv_dpr = dpr > 0.0f ? 1.0f / dpr : 1.0f;
      for (const auto& sg : run.glyphs) {
        // Quantise the current pen position into a (whole-pixel, sub-pixel
        // bin) pair using round-to-nearest, not floor. The cache stores one
        // bitmap per quarter-pixel bin; floor would map e.g. pen=5.99 to
        // (5, bin 3 = 0.75) — which paints 0.24 px to the *left* of where
        // the glyph actually wants to land. Rounding to the nearest quarter
        // pixel keeps the per-glyph error bounded by ⅛ px and matches the
        // approach used by Skia / Ghostty for sub-pixel glyph positioning.
        const float quarters_f = pen_fb.x * 4.0f;
        const float quarters_r = std::nearbyint(quarters_f);
        const float pen_fb_floor = std::floor(quarters_r * 0.25f);
        const float bin_index = quarters_r - pen_fb_floor * 4.0f; // 0..3
        const float sub_fb = bin_index * 0.25f;
        const auto& g = cache.lookup(face, sg.glyph_id, sub_fb);
        if (g.width > 0 && g.height > 0) {
          const float quad_fb_x = pen_fb_floor + g.offset_x + sg.x_offset;
          // Snap the dest Y to the nearest framebuffer pixel. The mask page
          // is sampled with a nearest-neighbour filter (mask atlas is rendered
          // at framebuffer resolution and we want crisp 1:1 texels), so a
          // fractional dest Y would land each texel between two destination
          // rows and produce visibly jagged baselines. Vertical sub-pixel
          // positioning is not currently baked into the cache anyway, so we
          // are not losing precision by snapping here.
          const float quad_fb_y = std::nearbyint(pen_fb.y + g.offset_y - sg.y_offset);
          const math::vec2 quad_pos{quad_fb_x * inv_dpr, quad_fb_y * inv_dpr};
          const math::vec2 quad_size{float(g.width) * inv_dpr, float(g.height) * inv_dpr};
          const math::mat4x4 m = make_screen_transform(quad_pos, quad_size, depth);
          texture_info ti;
          if (g.format == font::Format::bgra) {
            ti.texture = font_color_flag | 1u;
          } else {
            ti.texture = font_mask_flag | 1u;
          }
          const auto& atlas =
              (g.format == font::Format::bgra) ? cache.color_atlas() : cache.mask_atlas();
          const float aw = atlas.size().x ? float(atlas.size().x) : 1.0f;
          const float ah = atlas.size().y ? float(atlas.size().y) : 1.0f;
          ti.src = {(float(g.atlas_x) + 0.5f) / aw, (float(g.atlas_y) + 0.5f) / ah};
          ti.dst = {(float(g.atlas_x + g.width) - 0.5f) / aw,
                    (float(g.atlas_y + g.height) - 0.5f) / ah};
          fill_quad(r, m, color, ti);
        }
        pen_fb.x += sg.x_advance;
        pen_fb.y += sg.y_advance;
      }
      return pen_fb;
    }

    [[nodiscard]] math::vec4 draw_text_via_face_screen(command_buffer& r, math::vec2 origin,
                                                       float depth, std::string_view text,
                                                       font::Face& face, text_style style,
                                                       float dpr) {
      auto shaper = font::default_shaper();
      if (!shaper)
        return {0.0f, 0.0f, 0.0f, 0.0f};
      if (!std::isfinite(dpr) || dpr <= 0.0f)
        dpr = 1.0f;
      const float inv_dpr = 1.0f / dpr;
      const auto m = face.metrics();
      // Face metrics are in framebuffer pixels (face was loaded at pt × dpr).
      // line_height returned to the caller is in logical pixels; the user
      // can override it via style.line_height in logical pixels.
      const float face_line_h_fb = m.line_height > 0.0f ? m.line_height : face.pixel_size() * 1.2f;
      const float line_height_logical =
          style.line_height > 0.0f ? style.line_height : face_line_h_fb * inv_dpr;
      const float line_height_fb = line_height_logical * dpr;
      const r8g8b8a8 col = style.color;
      math::vec2 pen_fb{origin.x * dpr, origin.y * dpr + m.ascent};
      const float origin_fb_x = origin.x * dpr;
      float advance_total_logical = 0.0f;
      float max_advance_logical = 0.0f;
      u32 glyph_count = 0;
      const char* it = text.data();
      const char* end = it + text.size();
      const char* line_start = it;
      while (it <= end) {
        if (it == end || *it == '\n') {
          const std::string_view line{line_start, static_cast<usize>(it - line_start)};
          font::ShapeOptions opts = style_to_shape_opts(style);
          auto runs = shaper->shape(face, line, opts);
          math::vec2 line_pen_fb = pen_fb;
          for (const auto& run : runs) {
            font::Face* run_face = run.face ? run.face : &face;
            line_pen_fb = emit_shaped_run_screen(r, line_pen_fb, depth, dpr, *run_face, run, col);
            advance_total_logical += run.total_advance * inv_dpr;
            glyph_count += static_cast<u32>(run.glyphs.size());
          }
          const math::vec2 next_pen_fb = line_pen_fb;
          const float row_advance_logical = (next_pen_fb.x - origin_fb_x) * inv_dpr;
          max_advance_logical = math::fmax(max_advance_logical, row_advance_logical);
          if (it == end)
            break;
          ++it;
          line_start = it;
          pen_fb.x = origin_fb_x;
          pen_fb.y += line_height_fb;
          continue;
        }
        ++it;
      }
      const float h_fb =
          (pen_fb.y - origin.y * dpr - m.ascent) + (glyph_count ? line_height_fb : 0.0f);
      return math::vec4{max_advance_logical, h_fb * inv_dpr, advance_total_logical,
                        float(glyph_count)};
    }

    [[nodiscard]] math::vec2 measure_text_via_face(std::string_view text, font::Face& face,
                                                   text_style style, float dpr) {
      auto shaper = font::default_shaper();
      if (!shaper)
        return {0.0f, 0.0f};
      if (!std::isfinite(dpr) || dpr <= 0.0f)
        dpr = 1.0f;
      const float inv_dpr = 1.0f / dpr;
      const auto m = face.metrics();
      const float face_line_h_fb = m.line_height > 0.0f ? m.line_height : face.pixel_size() * 1.2f;
      const float line_height_logical =
          style.line_height > 0.0f ? style.line_height : face_line_h_fb * inv_dpr;
      const char* it = text.data();
      const char* end = it + text.size();
      const char* line_start = it;
      float x_max_logical = 0.0f;
      u32 lines = 0;
      while (it <= end) {
        if (it == end || *it == '\n') {
          const std::string_view line{line_start, static_cast<usize>(it - line_start)};
          font::ShapeOptions opts = style_to_shape_opts(style);
          auto runs = shaper->shape(face, line, opts);
          float line_advance = 0.0f;
          for (const auto& run : runs)
            line_advance += run.total_advance;
          x_max_logical = math::fmax(x_max_logical, line_advance * inv_dpr);
          if (it == end) {
            ++lines;
            break;
          }
          ++it;
          line_start = it;
          ++lines;
          continue;
        }
        ++it;
      }
      return {x_max_logical, static_cast<float>(lines) * line_height_logical};
    }
  } // namespace

  math::vec4 draw_text(command_buffer& r, math::vec2 at, float depth, std::string_view text,
                       const font_info& font, text_style style) {
    const float dpr = font::device_pixel_ratio();
    const float effective_pt = style.pt * dpr;
    if (auto face = font_face_for(font, effective_pt); face) {
      return draw_text_via_face_screen(r, at, depth, text, *face, style, dpr);
    }
    const font_variant_info& variant = resolve_font_variant(font, style.pt);
    const float line_height = variant.line_height;
    const r8g8b8a8 col = style.color;

    const math::vec2 origin = at;
    math::vec2 pen{origin.x, origin.y + variant.ascent};
    float advance_total = 0.0f;
    float row_advance = 0.0f;
    float max_advance = 0.0f;
    u32 glyphs_on_line = 0;
    u32 glyph_count = 0;

    {
      const char* preload = text.data();
      const char* preload_end = preload + text.size();
      while (preload < preload_end) {
        char32_t cp = decode_utf8(preload, preload_end);
        if (cp == 0 || cp == U'\n')
          continue;
        (void)resolve_font_glyph(font, variant, apply_case(cp, style.flags));
      }
    }
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
      char32_t cp = decode_utf8(it, end);
      if (cp == 0)
        continue;
      if (cp == U'\n') {
        max_advance = math::fmax(max_advance, row_advance);
        pen.x = origin.x;
        pen.y += line_height;
        row_advance = 0.0f;
        glyphs_on_line = 0;
        continue;
      }
      cp = apply_case(cp, style.flags);
      glyph_info g = resolve_font_glyph(font, variant, cp);
      if (g.size.x > 0.0f && g.size.y > 0.0f) {
        const math::vec2 lo{pen.x + g.offset.x, pen.y + g.offset.y};
        const math::mat4x4 m = make_screen_transform(lo, g.size, depth);
        texture_info ti;
        ti.texture = g.tx != null_texture ? g.tx : variant.texture;
        ti.src = g.uv0;
        ti.dst = g.uv1;
        fill_quad(r, m, col, ti);
      }
      pen.x += g.advance;
      row_advance += g.advance;
      advance_total += g.advance;
      ++glyphs_on_line;
      ++glyph_count;
      if (style.wrap_after != wrap_after_disabled && glyphs_on_line >= style.wrap_after) {
        max_advance = math::fmax(max_advance, row_advance);
        pen.x = origin.x;
        pen.y += line_height;
        row_advance = 0.0f;
        glyphs_on_line = 0;
      }
    }
    max_advance = math::fmax(max_advance, row_advance);
    const float h = (pen.y - origin.y - variant.ascent) + (glyph_count ? line_height : 0.0f);
    return math::vec4{max_advance, h, advance_total, float(glyph_count)};
  }
  math::vec4 draw_text(command_buffer& r, const math::mat4x4& transform, std::string_view text,
                       const font_info& font, text_style style) {
    const font_variant_info& variant = resolve_font_variant(font, style.pt);
    const float line_height = variant.line_height;
    const r8g8b8a8 col = style.color;

    const math::vec4 right = transform[0];
    const math::vec4 down = transform[1];
    const math::vec4 origin = transform[3];

    float pen_x = 0.0f;
    float pen_y = variant.ascent;
    float row_advance = 0.0f;
    float max_advance = 0.0f;
    float advance_total = 0.0f;
    u32 glyphs_on_line = 0;
    u32 glyph_count = 0;

    {
      const char* preload = text.data();
      const char* preload_end = preload + text.size();
      while (preload < preload_end) {
        char32_t cp = decode_utf8(preload, preload_end);
        if (cp == 0 || cp == U'\n')
          continue;
        (void)resolve_font_glyph(font, variant, apply_case(cp, style.flags));
      }
    }
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
      char32_t cp = decode_utf8(it, end);
      if (cp == 0)
        continue;
      if (cp == U'\n') {
        max_advance = math::fmax(max_advance, row_advance);
        pen_x = 0.0f;
        pen_y += line_height;
        row_advance = 0.0f;
        glyphs_on_line = 0;
        continue;
      }
      cp = apply_case(cp, style.flags);
      glyph_info g = resolve_font_glyph(font, variant, cp);
      if (g.size.x > 0.0f && g.size.y > 0.0f) {
        const float lx = pen_x + g.offset.x;
        const float ly = pen_y + g.offset.y;
        const float sx = g.size.x;
        const float sy = g.size.y;
        const math::vec4 p1 = origin + right * lx + down * ly;
        const math::vec4 p2 = origin + right * (lx + sx) + down * ly;
        const math::vec4 p3 = origin + right * lx + down * (ly + sy);
        const math::vec4 p4 = origin + right * (lx + sx) + down * (ly + sy);
        texture_info ti;
        ti.texture = g.tx != null_texture ? g.tx : variant.texture;
        ti.src = g.uv0;
        ti.dst = g.uv1;
        fill_quad(r, p1, p2, p3, p4, color_list<4>{col, col, col, col}, ti);
      }
      pen_x += g.advance;
      row_advance += g.advance;
      advance_total += g.advance;
      ++glyphs_on_line;
      ++glyph_count;
      if (style.wrap_after != wrap_after_disabled && glyphs_on_line >= style.wrap_after) {
        max_advance = math::fmax(max_advance, row_advance);
        pen_x = 0.0f;
        pen_y += line_height;
        row_advance = 0.0f;
        glyphs_on_line = 0;
      }
    }
    max_advance = math::fmax(max_advance, row_advance);
    const float h = pen_y - variant.ascent + (glyph_count ? line_height : 0.0f);
    return math::vec4{max_advance, h, advance_total, float(glyph_count)};
  }

  // ---------------------------------------------------------------------------
  // Blur — emit the 5x5 Gaussian sample pattern used by the original engine.
  // Samples are tagged with framebuffer_texture_id; GPU backends interpret that
  // reserved texture id as the captured/current frame, while CPU-only tests can
  // still inspect the exact vertex/index output deterministically.
  void blur_quad(command_buffer& r, math::vec4 p1, math::vec4 p2, math::vec4 p3, math::vec4 p4,
                 const color_list<4>& color, float dispersion, math::vec2 screen_size) {
    auto [vt, id] = r.allocate(4 * 26, 6 * 26, vertex_topology::triangle);

    // First quad: capture-only base colours used as the blur source mask.
    const r8g8b8a8 base[4] = {
        r8g8b8a8{color[0].r, color[0].g, color[0].b, static_cast<u8>(color[0].a == 0 ? 0 : 255)},
        r8g8b8a8{color[1].r, color[1].g, color[1].b, static_cast<u8>(color[1].a == 0 ? 0 : 255)},
        r8g8b8a8{color[2].r, color[2].g, color[2].b, static_cast<u8>(color[2].a == 0 ? 0 : 255)},
        r8g8b8a8{color[3].r, color[3].g, color[3].b, static_cast<u8>(color[3].a == 0 ? 0 : 255)},
    };
    vt[0] = make_vertex4(p1, {}, null_texture, base[0]);
    vt[1] = make_vertex4(p2, {}, null_texture, base[1]);
    vt[2] = make_vertex4(p3, {}, null_texture, base[2]);
    vt[3] = make_vertex4(p4, {}, null_texture, base[3]);

    auto cvt = [](u8 a) {
      const u32 x = 255u - a;
      return float(x * x) / 255.0f;
    };
    const float ca[4] = {cvt(color[0].a), cvt(color[1].a), cvt(color[2].a), cvt(color[3].a)};

    const math::vec2 ires{
        (screen_size.x > 0.0f ? dispersion / screen_size.x : 0.0f),
        (screen_size.y > 0.0f ? dispersion / screen_size.y : 0.0f),
    };

    usize cursor = 4;
    for (int x = -2; x <= +2; ++x) {
      for (int y = -2; y <= +2; ++y) {
        const float w = kBlurKernel[std::abs(x)][std::abs(y)];
        const math::vec2 uv{float(x) * ires.x, float(y) * ires.y};
        // Sentinel contract: tx == framebuffer_texture_id means the renderer
        // treats this quad as a captured-frame sample in the composite pass.
        const texture_id tex = framebuffer_texture_id;
        const u8 a0 = u8(math::fclamp(ca[0] * w, 0.0f, 255.0f));
        const u8 a1 = u8(math::fclamp(ca[1] * w, 0.0f, 255.0f));
        const u8 a2 = u8(math::fclamp(ca[2] * w, 0.0f, 255.0f));
        const u8 a3 = u8(math::fclamp(ca[3] * w, 0.0f, 255.0f));
        vt[cursor++] = make_vertex4(p1, uv, tex, r8g8b8a8{255, 255, 255, a0});
        vt[cursor++] = make_vertex4(p2, uv, tex, r8g8b8a8{255, 255, 255, a1});
        vt[cursor++] = make_vertex4(p3, uv, tex, r8g8b8a8{255, 255, 255, a2});
        vt[cursor++] = make_vertex4(p4, uv, tex, r8g8b8a8{255, 255, 255, a3});
      }
    }

    for (int q = 0; q != 26; ++q) {
      id[q * 6 + 0] += u32(q * 4 + 0);
      id[q * 6 + 1] += u32(q * 4 + 1);
      id[q * 6 + 2] += u32(q * 4 + 2);
      id[q * 6 + 3] += u32(q * 4 + 1);
      id[q * 6 + 4] += u32(q * 4 + 2);
      id[q * 6 + 5] += u32(q * 4 + 3);
    }
  }

  void blur_rect(command_buffer& r, const math::mat4x4& transform, float shift,
                 const color_list<4>& color, float dispersion, math::vec2 screen_size) {
    blur_quad(r, apply_no_w(transform, {0.0f, 0.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {1.0f, 0.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {0.0f - shift, 1.0f, 0.0f, 0.0f}),
              apply_no_w(transform, {1.0f - shift, 1.0f, 0.0f, 0.0f}), color, dispersion,
              screen_size);
  }

  void blur_rect(command_buffer& r, math::vec2 at, math::vec2 size, float depth, r8g8b8a8 color,
                 float dispersion, math::vec2 screen_size) {
    blur_rect(r, make_screen_transform(at, size, depth), 0.0f,
              color_list<4>{color, color, color, color}, dispersion, screen_size);
  }
} // namespace fxe::primitives
