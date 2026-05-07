#include "pipeline.hpp"

#if FXE_HAS_WGPU

#include <fxe/vertex.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace fxe {
  namespace {
    template <typename T> void hash_combine(size_t& seed, const T& value) noexcept {
      seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    template <typename E> [[nodiscard]] auto enum_value(E value) noexcept {
      return static_cast<std::underlying_type_t<E>>(value);
    }

    [[nodiscard]] bool configure_blend(blend_mode mode, wgpu::BlendState& out) noexcept {
      out = {};
      out.color.operation = wgpu::BlendOperation::Add;
      out.alpha.operation = wgpu::BlendOperation::Add;

      switch (mode) {
      case blend_mode::none:
        return false;
      case blend_mode::alpha:
        out.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
        out.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        out.alpha.srcFactor = wgpu::BlendFactor::One;
        out.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        return true;
      case blend_mode::premultiplied:
        out.color.srcFactor = wgpu::BlendFactor::One;
        out.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        out.alpha.srcFactor = wgpu::BlendFactor::One;
        out.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        return true;
      case blend_mode::additive:
        out.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
        out.color.dstFactor = wgpu::BlendFactor::One;
        out.alpha.srcFactor = wgpu::BlendFactor::One;
        out.alpha.dstFactor = wgpu::BlendFactor::One;
        return true;
      case blend_mode::multiply:
        out.color.srcFactor = wgpu::BlendFactor::Dst;
        out.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        out.alpha.srcFactor = wgpu::BlendFactor::One;
        out.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        return true;
      }
      return false;
    }
  } // namespace

  size_t pipeline_key_hash::operator()(const pipeline_key& key) const noexcept {
    size_t seed = 0;
    hash_combine(seed, key.vs_entry);
    hash_combine(seed, key.fs_entry);
    hash_combine(seed, enum_value(key.color_format));
    hash_combine(seed, enum_value(key.depth_format));
    hash_combine(seed, enum_value(key.blend));
    hash_combine(seed, enum_value(key.topology));
    hash_combine(seed, key.sample_count);
    return seed;
  }

  wgpu::RenderPipeline pipeline_cache::acquire(const pipeline_key& key, wgpu::Device device,
                                               wgpu::PipelineLayout layout,
                                               wgpu::ShaderModule shader) {
    if (auto it = pipelines_.find(key); it != pipelines_.end())
      return it->second;

    static_assert(sizeof(vertex) == 32, "vertex layout drift");
    std::array<wgpu::VertexAttribute, 5> attrs{};
    attrs[0].format = wgpu::VertexFormat::Float32x3;
    attrs[0].offset = 0;
    attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32;
    attrs[1].offset = 12;
    attrs[1].shaderLocation = 1;
    attrs[2].format = wgpu::VertexFormat::Unorm8x4;
    attrs[2].offset = 16;
    attrs[2].shaderLocation = 2;
    attrs[3].format = wgpu::VertexFormat::Float32x2;
    attrs[3].offset = 20;
    attrs[3].shaderLocation = 3;
    attrs[4].format = wgpu::VertexFormat::Uint32;
    attrs[4].offset = 28;
    attrs[4].shaderLocation = 4;

    wgpu::VertexBufferLayout vb_layout{};
    vb_layout.arrayStride = sizeof(vertex);
    vb_layout.stepMode = wgpu::VertexStepMode::Vertex;
    vb_layout.attributeCount = attrs.size();
    vb_layout.attributes = attrs.data();

    wgpu::BlendState blend{};
    const bool has_blend = configure_blend(key.blend, blend);

    wgpu::ColorTargetState color_target{};
    color_target.format = key.color_format;
    color_target.blend = has_blend ? &blend : nullptr;
    color_target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag_state{};
    frag_state.module = shader;
    frag_state.entryPoint = key.fs_entry.c_str();
    frag_state.targetCount = 1;
    frag_state.targets = &color_target;

    wgpu::DepthStencilState depth_state{};
    const bool has_depth = key.depth_format != wgpu::TextureFormat::Undefined;
    if (has_depth) {
      depth_state.format = key.depth_format;
      depth_state.depthWriteEnabled = true;
      depth_state.depthCompare = wgpu::CompareFunction::LessEqual;
    }

    const std::string label = "fxe-pipe-" + key.vs_entry + "-" + key.fs_entry;
    wgpu::RenderPipelineDescriptor desc{};
    desc.label = label.c_str();
    desc.layout = layout;
    desc.vertex.module = shader;
    desc.vertex.entryPoint = key.vs_entry.c_str();
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vb_layout;
    desc.primitive.topology = key.topology;
    desc.primitive.frontFace = wgpu::FrontFace::CCW;
    desc.primitive.cullMode = wgpu::CullMode::None;
    desc.multisample.count = key.sample_count;
    desc.multisample.mask = 0xffffffff;
    desc.fragment = &frag_state;
    desc.depthStencil = has_depth ? &depth_state : nullptr;

    auto pipeline = device.CreateRenderPipeline(&desc);
    auto [it, inserted] = pipelines_.emplace(key, pipeline);
    (void)inserted;
    return it->second;
  }

  std::unique_ptr<pipeline> pipeline::create(renderer&, const pipeline_desc& desc) {
    return std::make_unique<pipeline_detail::recording_pipeline>(desc);
  }
} // namespace fxe
#endif