#pragma once

#include <fxe/pipeline.hpp>

#if FXE_HAS_WGPU
#include <webgpu/webgpu_cpp.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fxe {
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

  class pipeline_cache {
  public:
    [[nodiscard]] wgpu::RenderPipeline acquire(const pipeline_key& key, wgpu::Device device,
                                               wgpu::PipelineLayout layout,
                                               wgpu::ShaderModule shader);
    void clear() noexcept {
      pipelines_.clear();
    }

  private:
    std::unordered_map<pipeline_key, wgpu::RenderPipeline, pipeline_key_hash> pipelines_;
  };
} // namespace fxe
#endif