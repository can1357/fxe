#pragma once

#include <fxe/types.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <fxe/render_stats.hpp>
#include <fxe/vertex.hpp>

namespace fxe {
  enum class primitive_effect : u8 {
    color = 0,
    transparent,
    text_mask,
    text_color,
    framebuffer_sample,
    vertical_blur,
    horizontal_blur,
    luminance_filter,
  };

  struct command_buffer {
    std::vector<vertex> vertex_buffer;
    std::array<std::vector<u32>, static_cast<usize>(vertex_topology::max)> index_buffers{};
    mutable std::shared_ptr<void> mesh_instance{};
    mutable void* mesh_owner = nullptr;
    u32 epoch = 1;

    void inc_epoch() noexcept {
      ++epoch;
    }

    write_args allocate(usize vtx, usize idx, vertex_topology topology) {
      inc_epoch();
      auto& vertices = vertex_buffer;
      auto& indices = index_buffers[static_cast<usize>(topology)];
      usize vertex_base = vertices.size();
      usize index_base = indices.size();
      vertices.resize(vertex_base + vtx);
      indices.resize(index_base + idx, static_cast<u32>(vertex_base));
      return {vertices.data() + vertex_base, indices.data() + index_base};
    }

    vertex* allocate_strip(usize vtx, vertex_topology topology) {
      usize idx = calc_indices_strip(vtx, topology);
      auto [v, i] = allocate(vtx, idx, topology);
      fill_indices_strip(i, vtx, topology);
      return v;
    }

    vertex* allocate_list(usize vtx, vertex_topology topology) {
      usize idx = calc_indices_list(vtx, topology);
      auto [v, i] = allocate(vtx, idx, topology);
      fill_indices_list(i, idx, topology);
      return v;
    }

    void write_list(std::span<const vertex> vertices, vertex_topology top) {
      usize idx = calc_indices_list(vertices.size(), top);
      auto [vout, iout] = allocate(vertices.size(), idx, top);
      std::memcpy(vout, vertices.data(), vertices.size_bytes());
      fill_indices_list(iout, idx, top);
    }

    void write_strip(std::span<const vertex> vertices, vertex_topology top) {
      usize idx = calc_indices_strip(vertices.size(), top);
      auto [vout, iout] = allocate(vertices.size(), idx, top);
      std::memcpy(vout, vertices.data(), vertices.size_bytes());
      fill_indices_strip(iout, vertices.size(), top);
    }

    virtual bool queue_opt(const command_buffer&, const math::mat4x4&, const math::vec4&) {
      return false;
    }
    virtual bool queue_opt(const command_buffer&, const math::mat4x4&, const math::mat4x4&,
                           const math::vec4&, const render_config&) {
      return false;
    }

    void transform(const math::mat4x4& tf, const std::optional<math::vec4>& tint = std::nullopt) {
      inc_epoch();
      for (auto& v : vertex_buffer)
        v = tint ? v.transform(tf, *tint) : v.transform(tf);
    }

    // True iff `m` is the 4x4 identity. Cheap (16 fp comparisons), called
    // hot from queue() to choose the memcpy fast-path. We compare against
    // the bit pattern rather than approximate-equal: callers either pass
    // the unmodified default math::identity() (which the JS bindings install
    // when no transform argument is given) or an arbitrary user matrix.
    [[nodiscard]] static bool is_identity(const math::mat4x4& m) noexcept {
      const float* p = &m[0][0];
      return p[0] == 1.0f && p[1] == 0.0f && p[2] == 0.0f && p[3] == 0.0f && p[4] == 0.0f &&
             p[5] == 1.0f && p[6] == 0.0f && p[7] == 0.0f && p[8] == 0.0f && p[9] == 0.0f &&
             p[10] == 1.0f && p[11] == 0.0f && p[12] == 0.0f && p[13] == 0.0f && p[14] == 0.0f &&
             p[15] == 1.0f;
    }

    inline static std::atomic<u64> g_q_fast{0};
    inline static std::atomic<u64> g_q_tinted{0};
    inline static std::atomic<u64> g_q_xform{0};

    void queue(const command_buffer& src, const math::mat4x4& tf = math::identity(),
               const std::optional<math::vec4>& tint = std::nullopt) {
      if (vertex_buffer.size() > 512 && queue_opt(src, tf, tint.value_or(math::vec4{1, 1, 1, 1})))
        return;
      inc_epoch();
      auto& stats = current_render_stats();
      ++stats.queue_calls;
      stats.vertices_submitted += src.vertex_buffer.size();
      for (const auto& ib : src.index_buffers)
        stats.indices_submitted += ib.size();
      const usize vertex_base = vertex_buffer.size();
      const usize src_n = src.vertex_buffer.size();
      vertex_buffer.resize(vertex_base + src_n);

      // Fast path: identity transform + no tint.
      // ~42% of the steady-state stress-grid frame was spent in per-vertex
      // mat4 * vec4 here. queueInto() in the fxe-ui reconciler emits paint
      // primitives in already-resolved screen coords, so almost every queue
      // call lands here.
      if (!tint && is_identity(tf)) {
        ++g_q_fast;
        if (src_n != 0)
          std::memcpy(vertex_buffer.data() + vertex_base, src.vertex_buffer.data(),
                      src_n * sizeof(vertex));
      } else if (!tint) {
        ++g_q_xform;
        for (usize i = 0; i != src_n; ++i)
          vertex_buffer[vertex_base + i] = src.vertex_buffer[i].transform(tf);
      } else {
        ++g_q_tinted;
        const math::vec4& t = *tint;
        for (usize i = 0; i != src_n; ++i)
          vertex_buffer[vertex_base + i] = src.vertex_buffer[i].transform(tf, t);
      }

      const u32 vbase32 = static_cast<u32>(vertex_base);
      for (usize n = 0; n != index_buffers.size(); ++n) {
        auto& dst = index_buffers[n];
        const auto& si = src.index_buffers[n];
        const usize dst_base = dst.size();
        const usize src_idx_n = si.size();
        dst.resize(dst_base + src_idx_n);
        if (vbase32 == 0 && src_idx_n != 0) {
          std::memcpy(dst.data() + dst_base, si.data(), src_idx_n * sizeof(u32));
        } else {
          for (usize i = 0; i != src_idx_n; ++i)
            dst[dst_base + i] = si[i] + vbase32;
        }
      }
    }

    void clear() {
      vertex_buffer.clear();
      for (auto& indices : index_buffers)
        indices.clear();
      inc_epoch();
    }

    [[nodiscard]] std::pair<math::vec3, math::vec3> get_boundaries() const;
    [[nodiscard]] std::pair<math::vec3, math::vec3> get_boundaries(const math::mat4x4& tf) const;
    virtual ~command_buffer() = default;
    [[nodiscard]] command_buffer clone() const;
    [[nodiscard]] bool is_empty() const noexcept;
  };
} // namespace fxe
