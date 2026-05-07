#pragma once

#include <fxe/pipeline.hpp>

#if FXE_HAS_WGPU
#include <webgpu/webgpu_cpp.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fxe {
  class pipeline_cache;
  struct custom_pipeline_draw {
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer vertex_buffer;
    wgpu::Buffer index_buffer;
    wgpu::BindGroup renderer_bind_group;
    wgpu::BindGroup user_bind_group;
    uint64_t vertex_bytes = 0;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    bool uses_renderer_bind_group = false;
    bool uses_user_bind_group = false;
  };

  class dawn_pipeline_device_access {
  public:
    virtual wgpu::Device& device() = 0;
    virtual wgpu::Queue& queue() = 0;
    virtual wgpu::TextureFormat color_format() const = 0;
    virtual wgpu::TextureFormat depth_format() const = 0;
    virtual uint32_t sample_count() const = 0;
    virtual pipeline_cache& cache() = 0;
    virtual wgpu::BindGroupLayout renderer_bind_group_layout() const = 0;
    virtual wgpu::BindGroup renderer_bind_group() const = 0;
    virtual wgpu::TextureView texture_view(texture_id id) const = 0;
    virtual wgpu::Sampler texture_sampler() const = 0;
    virtual void enqueue_custom_draw(custom_pipeline_draw draw) = 0;
    virtual ~dawn_pipeline_device_access() = default;
  };

  struct pipeline_key {
    std::string vs_entry = "vs_transform";
    std::string fs_entry = "ps_opaque";
    wgpu::TextureFormat color_format = wgpu::TextureFormat::Undefined;
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined;
    blend_mode blend = blend_mode::alpha;
    wgpu::PrimitiveTopology topology = wgpu::PrimitiveTopology::TriangleList;
    u32 sample_count = 1;

    [[nodiscard]] bool operator==(const pipeline_key& other) const noexcept {
      return vs_entry == other.vs_entry && fs_entry == other.fs_entry &&
             color_format == other.color_format && depth_format == other.depth_format &&
             blend == other.blend && topology == other.topology &&
             sample_count == other.sample_count;
    }
  };

  struct pipeline_key_hash {
    [[nodiscard]] size_t operator()(const pipeline_key& key) const noexcept;
  };

  struct custom_pipeline_key {
    uint64_t wgsl_hash = 0;
    uint64_t layout_hash = 0;
    uint32_t vertex_stride = 0;
    wgpu::TextureFormat color_format = wgpu::TextureFormat::Undefined;
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined;
    blend_mode blend = blend_mode::alpha;
    wgpu::PrimitiveTopology topology = wgpu::PrimitiveTopology::TriangleList;
    u32 sample_count = 1;

    [[nodiscard]] bool operator==(const custom_pipeline_key& other) const noexcept {
      return wgsl_hash == other.wgsl_hash && layout_hash == other.layout_hash &&
             vertex_stride == other.vertex_stride && color_format == other.color_format &&
             depth_format == other.depth_format && blend == other.blend &&
             topology == other.topology && sample_count == other.sample_count;
    }
  };

  struct custom_pipeline_key_hash {
    [[nodiscard]] size_t operator()(const custom_pipeline_key& key) const noexcept;
  };

  class pipeline_cache {
  public:
    [[nodiscard]] wgpu::RenderPipeline acquire(const pipeline_key& key, wgpu::Device device,
                                               wgpu::PipelineLayout layout,
                                               wgpu::ShaderModule shader);
    [[nodiscard]] wgpu::RenderPipeline
    acquire_custom(const custom_pipeline_key& key, wgpu::Device device, wgpu::PipelineLayout layout,
                   wgpu::ShaderModule shader, const std::string& vs_entry,
                   const std::string& fs_entry, const std::vector<wgpu::VertexAttribute>& attrs,
                   uint64_t vertex_stride);
    void clear() noexcept {
      pipelines_.clear();
      custom_pipelines_.clear();
      miss_count_ = 0;
    }
    [[nodiscard]] size_t miss_count() const noexcept {
      return miss_count_;
    }

  private:
    std::unordered_map<pipeline_key, wgpu::RenderPipeline, pipeline_key_hash> pipelines_;
    std::unordered_map<custom_pipeline_key, wgpu::RenderPipeline, custom_pipeline_key_hash>
        custom_pipelines_;
    size_t miss_count_ = 0;
  };
} // namespace fxe
#endif
