#include <fxe/command_buffer.hpp>
#include <fxe/types.hpp>

#include <algorithm>
#include <string_view>
namespace fxe {
  command_buffer command_buffer::clone() const {
    command_buffer out;
    out.vertex_buffer = vertex_buffer;
    for (usize i = 0; i != index_buffers.size(); ++i)
      out.index_buffers[i] = index_buffers[i];
    out.epoch = epoch;
    return out;
  }

  bool command_view::is_empty() const noexcept {
    if (!vertices().empty())
      return false;
    for (usize i = 0; i != static_cast<usize>(vertex_topology::max); ++i)
      if (!indices(static_cast<vertex_topology>(i)).empty())
        return false;
    return true;
  }

  std::pair<math::vec3, math::vec3> command_view::get_boundaries() const {
    auto verts = vertices();
    if (verts.empty())
      return {{0, 0, 0}, {0, 0, 0}};
    math::vec3 mn{math::flt_max, math::flt_max, math::flt_max};
    math::vec3 mx{-math::flt_max, -math::flt_max, -math::flt_max};
    for (const auto& v : verts) {
      mn = math::vec_min(mn, v.pos);
      mx = math::vec_max(mx, v.pos);
    }
    return {mn, mx};
  }

  std::pair<math::vec3, math::vec3> command_view::get_boundaries(const math::mat4x4& tf) const {
    auto verts = vertices();
    if (verts.empty())
      return {{0, 0, 0}, {0, 0, 0}};
    math::vec3 mn{math::flt_max, math::flt_max, math::flt_max};
    math::vec3 mx{-math::flt_max, -math::flt_max, -math::flt_max};
    for (const auto& v : verts) {
      auto p = math::vec3(tf * math::vec4(v.pos, 1.0f));
      mn = math::vec_min(mn, p);
      mx = math::vec_max(mx, p);
    }
    return {mn, mx};
  }

  void command_sink::queue(const command_view& src, const math::mat4x4& tf,
                           const std::optional<math::vec4>& tint) {
    if (vertices().size() > 512 && queue_opt(src, tf, tint.value_or(math::vec4{1, 1, 1, 1})))
      return;

    auto src_vertices = src.vertices();
    std::array<std::span<const u32>, static_cast<usize>(vertex_topology::max)> src_indices{};
    usize first_top = 0;
    bool found_index_stream = false;
    usize total_indices = 0;
    for (usize i = 0; i != src_indices.size(); ++i) {
      src_indices[i] = src.indices(static_cast<vertex_topology>(i));
      total_indices += src_indices[i].size();
      if (!found_index_stream && !src_indices[i].empty()) {
        first_top = i;
        found_index_stream = true;
      }
    }

    auto& stats = current_render_stats();
    ++stats.queue_calls;
    stats.vertices_submitted += src_vertices.size();
    stats.indices_submitted += total_indices;

    const usize vertex_base = vertices().size();
    const usize src_n = src_vertices.size();
    auto [vout, iout] =
        allocate(src_n, src_indices[first_top].size(), static_cast<vertex_topology>(first_top));

    // Fast path: identity transform + no tint.
    // ~42% of the steady-state stress-grid frame was spent in per-vertex
    // mat4 * vec4 here. queueInto() in the fxe-ui reconciler emits paint
    // primitives in already-resolved screen coords, so almost every queue
    // call lands here.
    if (!tint && is_identity(tf)) {
      ++g_q_fast;
      if (src_n != 0)
        std::memcpy(vout, src_vertices.data(), src_vertices.size_bytes());
    } else if (!tint) {
      ++g_q_xform;
      for (usize i = 0; i != src_n; ++i)
        vout[i] = src_vertices[i].transform(tf);
    } else {
      ++g_q_tinted;
      const math::vec4& t = *tint;
      for (usize i = 0; i != src_n; ++i)
        vout[i] = src_vertices[i].transform(tf, t);
    }

    const u32 vbase32 = static_cast<u32>(vertex_base);
    auto copy_indices = [vbase32](std::span<const u32> si, u32* out) {
      if (si.empty())
        return;
      if (vbase32 == 0) {
        std::memcpy(out, si.data(), si.size_bytes());
      } else {
        for (usize i = 0; i != si.size(); ++i)
          out[i] = si[i] + vbase32;
      }
    };

    copy_indices(src_indices[first_top], iout);
    for (usize i = 0; i != src_indices.size(); ++i) {
      if (i == first_top || src_indices[i].empty())
        continue;
      auto [unused_vertices, extra_indices] =
          allocate(0, src_indices[i].size(), static_cast<vertex_topology>(i));
      (void)unused_vertices;
      copy_indices(src_indices[i], extra_indices);
    }
  }

  std::string r8g8b8a8::to_string() const {
    char out[10]{};
    static constexpr char hex[] = "0123456789abcdef";
    out[0] = '#';
    const u8 bytes[] = {r, g, b, a};
    for (int i = 0; i != 4; ++i) {
      out[1 + i * 2] = hex[bytes[i] >> 4];
      out[2 + i * 2] = hex[bytes[i] & 0xf];
    }
    return out;
  }

  r8g8b8a8 color_by_name(std::string_view name) noexcept {
    auto cmp = [](const named_color& kv, std::string_view n) {
      return std::string_view(kv.first) < n;
    };
    auto it = std::lower_bound(color_table.begin(), color_table.end(), name, cmp);
    if (it == color_table.end() || std::string_view(it->first) != name)
      return transparent;
    return it->second;
  }
} // namespace fxe
