#pragma once

#include <fxe/command_buffer.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef FXE_HAS_WGPU
#define FXE_HAS_WGPU 0
#endif

namespace fxe {
  struct vertex_attribute {
    uint32_t shader_location = 0;
    uint32_t offset = 0;
    enum class format { f32, f32x2, f32x3, f32x4, u32, u8x4_norm } fmt = format::f32;
  };

  struct pipeline_desc {
    std::string wgsl;
    std::string vs_entry = "vs_main";
    std::string fs_entry = "fs_main";
    uint32_t vertex_stride = 0;
    std::vector<vertex_attribute> attrs;
    // bind groups: 0 = per-frame UBO (provided by renderer), 1 = per-pipeline
    // (textures/samplers/UBOs)
    uint32_t color_format_hint = 0;
    bool depth_test = true;
    bool blend = true;
  };

  class pipeline {
  public:
    static std::unique_ptr<pipeline> create(renderer&, const pipeline_desc&);
    virtual ~pipeline() = default;
    virtual void update_uniforms(const void* data, size_t bytes) = 0;
    virtual void bind_texture(uint32_t binding, texture_id tex) = 0;
    virtual void draw(command_buffer& cb, const float* vertices, size_t vertex_count,
                      const uint32_t* indices, size_t index_count, const float matrix[16]) = 0;
  };

  namespace pipeline_detail {
    [[nodiscard]] inline size_t vertex_format_size(vertex_attribute::format fmt) noexcept {
      switch (fmt) {
      case vertex_attribute::format::f32:
      case vertex_attribute::format::u32:
      case vertex_attribute::format::u8x4_norm:
        return 4;
      case vertex_attribute::format::f32x2:
        return 8;
      case vertex_attribute::format::f32x3:
        return 12;
      case vertex_attribute::format::f32x4:
        return 16;
      }
      return 0;
    }

    inline void validate_pipeline_desc(const pipeline_desc& desc) {
      if (desc.wgsl.empty())
        throw std::invalid_argument("WGSL validation failed: source is empty");
      if (desc.vs_entry.empty() || desc.fs_entry.empty())
        throw std::invalid_argument("Pipeline entry points must be non-empty");
      if (desc.wgsl.find("@vertex") == std::string::npos ||
          desc.wgsl.find(desc.vs_entry) == std::string::npos) {
        throw std::invalid_argument("WGSL validation failed: missing @vertex entry point '" +
                                    desc.vs_entry + "'");
      }
      if (desc.wgsl.find("@fragment") == std::string::npos ||
          desc.wgsl.find(desc.fs_entry) == std::string::npos) {
        throw std::invalid_argument("WGSL validation failed: missing @fragment entry point '" +
                                    desc.fs_entry + "'");
      }
      if (desc.vertex_stride == 0)
        throw std::invalid_argument("Pipeline vertexStride must be greater than zero");
      if ((desc.vertex_stride % sizeof(float)) != 0)
        throw std::invalid_argument("Pipeline vertexStride must be a multiple of 4 bytes");
      for (const auto& attr : desc.attrs) {
        const size_t end = static_cast<size_t>(attr.offset) + vertex_format_size(attr.fmt);
        if (end > desc.vertex_stride) {
          throw std::invalid_argument("Pipeline vertex attribute exceeds vertexStride");
        }
      }
    }

#if !FXE_HAS_WGPU
    class recording_pipeline final : public pipeline {
    public:
      explicit recording_pipeline(pipeline_desc desc) : desc_(std::move(desc)) {
        validate_pipeline_desc(desc_);
      }

      void update_uniforms(const void* data, size_t bytes) override {
        uniforms_.resize(bytes);
        if (bytes && data)
          std::memcpy(uniforms_.data(), data, bytes);
      }

      void bind_texture(uint32_t binding, texture_id tex) override {
        if (binding >= bound_textures_.size())
          bound_textures_.resize(static_cast<size_t>(binding) + 1, null_texture);
        bound_textures_[binding] = tex;
      }

      void draw(command_buffer& cb, const float* vertices, size_t vertex_count,
                const uint32_t* indices, size_t index_count, const float matrix[16]) override {
        (void)matrix;
        auto [out_vertices, out_indices] =
            cb.allocate(vertex_count, index_count, vertex_topology::triangle);
        const size_t floats_per_vertex = desc_.vertex_stride / sizeof(float);
        for (size_t i = 0; i < vertex_count; ++i) {
          vertex v{};
          v.color = r8g8b8a8{255, 255, 255, 255};
          if (vertices && floats_per_vertex >= 2) {
            const float* src = vertices + i * floats_per_vertex;
            v.pos = {src[0], src[1], floats_per_vertex >= 3 ? src[2] : 0.0f};
            v.is_world = 1.0f;
            if (floats_per_vertex >= 5)
              v.uv = {src[3], src[4], 0.0f};
          }
          out_vertices[i] = v;
        }
        for (size_t i = 0; i < index_count; ++i)
          out_indices[i] = indices ? indices[i] : static_cast<uint32_t>(i);
      }

    private:
      pipeline_desc desc_;
      std::vector<uint8_t> uniforms_;
      std::vector<texture_id> bound_textures_;
    };
#endif
  } // namespace pipeline_detail

#if !FXE_HAS_WGPU
  inline std::unique_ptr<pipeline> pipeline::create(renderer&, const pipeline_desc& desc) {
    return std::make_unique<pipeline_detail::recording_pipeline>(desc);
  }
#endif
} // namespace fxe
