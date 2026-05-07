#pragma once

#include <fxe/types.hpp>

#include <array>
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
      usize vertex_base = vertex_buffer.size();
      vertex_buffer.reserve(vertex_base + src.vertex_buffer.size());
      for (const auto& v : src.vertex_buffer)
        vertex_buffer.push_back(tint ? v.transform(tf, *tint) : v.transform(tf));
      for (usize n = 0; n != index_buffers.size(); ++n) {
        auto& dst = index_buffers[n];
        auto& si = src.index_buffers[n];
        dst.reserve(dst.size() + si.size());
        for (auto index : si)
          dst.push_back(index + static_cast<u32>(vertex_base));
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
