#pragma once

#include <fxe/types.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include <fxe/color.hpp>
#include <fxe/math.hpp>
#include <fxe/spritesheet.hpp>

namespace fxe {
  struct viewport_desc {
    math::vec2 at{};
    math::vec2 size{};
    math::vec2 depth_range{};
  };
  struct scissor_rect {
    math::ivec2 begin{};
    math::ivec2 end{};
  };
  struct render_config {
    viewport_desc viewport{};
    scissor_rect scissor{};
  };

  struct vshader_cbuf {
    math::mat4x4 world_view_proj{1.0f};
    math::mat4x4 screen_ndc{1.0f};
    math::vec4 tint{1, 1, 1, 1};
    math::vec2 capture_offset{0.5f, 0.5f};
    math::vec2 capture_scale{0.5f, -0.5f};
    math::vec4 eye_pos{};
    math::vec4 eye_dir{};
    float time = 0.0f;
    u8 _pad[60]{};
  };
  static_assert(sizeof(vshader_cbuf) % 256 == 0);

  struct vertex {
    math::vec3 pos{};
    float is_world = 0.0f;
    r8g8b8a8 color{};
    math::vec3 uv{};
    // uv.z stores raw texture_id bits for the GPU backend. The Dawn vertex
    // layout must expose this slot as Uint32, not Float32: small texture ids
    // are subnormal float bit patterns and may be flushed if fetched as f32.

    [[nodiscard]] vertex transform(const math::mat4x4& mat) const noexcept {
      vertex r = *this;
      math::vec4 p = mat * math::vec4(pos, is_world == 0.0f ? 1.0f : is_world);
      r.pos = math::vec3(p);
      // Preserve screen/world flag — the shader uses is_world as a boolean to
      // pick screen_ndc vs world_view_proj. Overwriting it with p.w would
      // promote screen-space vertices to world-space (clipping them off-screen)
      // every time queue() copies them with an identity transform.
      r.is_world = is_world;
      return r;
    }
    [[nodiscard]] vertex transform(const math::mat4x4& mat, const math::vec4& tint) const noexcept {
      vertex r = transform(mat);
      auto c = color.xyzw();
      r.color = r8g8b8a8{u8(math::fclamp(c.r * tint.r, 0.0f, 1.0f) * 255.0f),
                         u8(math::fclamp(c.g * tint.g, 0.0f, 1.0f) * 255.0f),
                         u8(math::fclamp(c.b * tint.b, 0.0f, 1.0f) * 255.0f),
                         u8(math::fclamp(c.a * tint.a, 0.0f, 1.0f) * 255.0f)};
      return r;
    }
  };
  static_assert(sizeof(vertex) == 32, "fxe::vertex must preserve the original 32-byte GPU layout");
  static_assert(alignof(vertex) == 4);

  [[nodiscard]] inline vertex make_vertex4(math::vec4 pos, math::vec2 tex_pos, texture_id tex_id,
                                           r8g8b8a8 color) noexcept {
    return {math::vec3(pos), pos.w, color, {tex_pos.x, tex_pos.y, std::bit_cast<float>(tex_id)}};
  }
  [[nodiscard]] inline vertex make_vertex(math::vec2 pos, float depth, math::vec2 tex_pos,
                                          texture_id tex_id, r8g8b8a8 color) noexcept {
    return make_vertex4({pos.x, pos.y, depth, 0.0f}, tex_pos, tex_id, color);
  }
  [[nodiscard]] inline vertex make_vertex(math::vec3 pos, math::vec2 tex_pos, texture_id tex_id,
                                          r8g8b8a8 color) noexcept {
    return make_vertex4({pos.x, pos.y, pos.z, 1.0f}, tex_pos, tex_id, color);
  }

  enum class vertex_topology : u32 { triangle = 0, line = 1, max };
  using write_args = std::tuple<vertex*, u32*>;

  [[nodiscard]] inline usize calc_indices_strip(usize n, vertex_topology top) noexcept {
    usize m = top == vertex_topology::line ? 2 : 3;
    return n < m ? 0 : (n - (m - 1)) * m;
  }
  inline void fill_indices_strip(u32* dst, usize n, vertex_topology top) noexcept {
    usize m = top == vertex_topology::line ? 2 : 3;
    if (n < m)
      return;
    for (usize i = 0; i != n - (m - 1); ++i)
      for (usize j = 0; j != m; ++j)
        *dst++ += u32(i + j);
  }
  [[nodiscard]] inline usize calc_indices_list(usize n, vertex_topology top) noexcept {
    usize m = top == vertex_topology::line ? 2 : 3;
    return (n / m) * m;
  }
  inline void fill_indices_list(u32* dst, usize n, vertex_topology) noexcept {
    for (usize i = 0; i != n; ++i)
      dst[i] += u32(i);
  }
} // namespace fxe
