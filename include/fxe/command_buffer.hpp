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

  struct command_view {
    [[nodiscard]] virtual std::span<const vertex> vertices() const noexcept = 0;
    [[nodiscard]] virtual std::span<const u32> indices(vertex_topology topology) const noexcept = 0;
    [[nodiscard]] virtual u32 epoch_value() const noexcept = 0;

    [[nodiscard]] bool is_empty() const noexcept;
    [[nodiscard]] std::pair<math::vec3, math::vec3> get_boundaries() const;
    [[nodiscard]] std::pair<math::vec3, math::vec3> get_boundaries(const math::mat4x4& tf) const;

    virtual ~command_view() = default;
  };

  struct command_sink : virtual command_view {
    [[nodiscard]] virtual write_args allocate(usize vtx, usize idx, vertex_topology topology) = 0;

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

    virtual bool queue_opt(const command_view&, const math::mat4x4&, const math::vec4&) {
      return false;
    }
    virtual bool queue_opt(const command_view&, const math::mat4x4&, const math::mat4x4&,
                           const math::vec4&, const render_config&) {
      return false;
    }

    virtual void transform(const math::mat4x4& tf,
                           const std::optional<math::vec4>& tint = std::nullopt) = 0;

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

    virtual void queue(const command_view& src, const math::mat4x4& tf = math::identity(),
                       const std::optional<math::vec4>& tint = std::nullopt);
    virtual void clear() = 0;
  };

  struct command_buffer : public command_sink {
    std::vector<vertex> vertex_buffer;
    std::array<std::vector<u32>, static_cast<usize>(vertex_topology::max)> index_buffers{};
    mutable std::shared_ptr<void> mesh_instance{};
    mutable void* mesh_owner = nullptr;
    u32 epoch = 1;

    void inc_epoch() noexcept {
      ++epoch;
    }

    [[nodiscard]] std::span<const vertex> vertices() const noexcept override {
      return vertex_buffer;
    }

    [[nodiscard]] std::span<const u32> indices(vertex_topology topology) const noexcept override {
      return index_buffers[static_cast<usize>(topology)];
    }

    [[nodiscard]] u32 epoch_value() const noexcept override {
      return epoch;
    }

    write_args allocate(usize vtx, usize idx, vertex_topology topology) override {
      inc_epoch();
      auto& vertices = vertex_buffer;
      auto& indices = index_buffers[static_cast<usize>(topology)];
      usize vertex_base = vertices.size();
      usize index_base = indices.size();
      vertices.resize(vertex_base + vtx);
      indices.resize(index_base + idx, static_cast<u32>(vertex_base));
      return {vtx == 0 ? nullptr : vertices.data() + vertex_base,
              idx == 0 ? nullptr : indices.data() + index_base};
    }

    void transform(const math::mat4x4& tf,
                   const std::optional<math::vec4>& tint = std::nullopt) override {
      inc_epoch();
      for (auto& v : vertex_buffer)
        v = tint ? v.transform(tf, *tint) : v.transform(tf);
    }

    void clear() override {
      vertex_buffer.clear();
      for (auto& indices : index_buffers)
        indices.clear();
      inc_epoch();
    }

    [[nodiscard]] command_buffer clone() const;
  };
} // namespace fxe
