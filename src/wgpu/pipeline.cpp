#include "pipeline.hpp"
#include <fxe/types.hpp>

#if FXE_HAS_WGPU

#include <fxe/renderer.hpp>
#include <fxe/vertex.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fxe {
  namespace {
    constexpr u64 kInitialCustomBufferBytes = 4096;
    constexpr u64 kInitialUniformBytes = 256;

    template <typename T> void hash_combine(usize& seed, const T& value) noexcept {
      seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    template <typename E> [[nodiscard]] auto enum_value(E value) noexcept {
      return static_cast<std::underlying_type_t<E>>(value);
    }

    [[nodiscard]] u64 fnv1a(const void* data, usize bytes) noexcept {
      const auto* p = static_cast<const u8*>(data);
      u64 h = 1469598103934665603ull;
      for (usize i = 0; i != bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
      }
      return h;
    }

    [[nodiscard]] u64 hash_string(const std::string& s) noexcept {
      return fnv1a(s.data(), s.size());
    }

    template <typename T> void hash_value(u64& h, const T& value) noexcept {
      const u64 part = fnv1a(&value, sizeof(value));
      h ^= part;
      h *= 1099511628211ull;
    }

    [[nodiscard]] u64 align_to(u64 value, u64 alignment) noexcept {
      return (value + alignment - 1) & ~(alignment - 1);
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

    [[nodiscard]] wgpu::VertexFormat to_wgpu_format(vertex_attribute::format fmt) {
      switch (fmt) {
      case vertex_attribute::format::f32:
        return wgpu::VertexFormat::Float32;
      case vertex_attribute::format::f32x2:
        return wgpu::VertexFormat::Float32x2;
      case vertex_attribute::format::f32x3:
        return wgpu::VertexFormat::Float32x3;
      case vertex_attribute::format::f32x4:
        return wgpu::VertexFormat::Float32x4;
      case vertex_attribute::format::u32:
        return wgpu::VertexFormat::Uint32;
      case vertex_attribute::format::u8x4_norm:
        return wgpu::VertexFormat::Unorm8x4;
      }
      throw std::invalid_argument("Pipeline vertex attribute format is unsupported");
    }

    [[nodiscard]] std::vector<wgpu::VertexAttribute> make_vertex_attrs(const pipeline_desc& desc) {
      std::vector<wgpu::VertexAttribute> out;
      out.reserve(desc.attrs.size());
      for (const auto& attr : desc.attrs) {
        wgpu::VertexAttribute gpu{};
        gpu.format = to_wgpu_format(attr.fmt);
        gpu.offset = attr.offset;
        gpu.shaderLocation = attr.shader_location;
        out.push_back(gpu);
      }
      return out;
    }

    struct wgsl_resource {
      enum class kind { uniform_buffer, texture, sampler } type = kind::uniform_buffer;
      u32 group = 0;
      u32 binding = 0;
    };

    [[nodiscard]] bool parse_uint_after(const std::string& s, usize pos, u32& out) noexcept {
      while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
        ++pos;
      if (pos >= s.size() || s[pos] != '(')
        return false;
      ++pos;
      while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
        ++pos;
      u32 value = 0;
      bool any = false;
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        any = true;
        value = value * 10u + static_cast<u32>(s[pos] - '0');
        ++pos;
      }
      out = value;
      return any;
    }

    [[nodiscard]] std::vector<wgsl_resource> reflect_wgsl_resources(const std::string& wgsl) {
      std::vector<wgsl_resource> resources;
      usize pos = 0;
      while ((pos = wgsl.find("@group", pos)) != std::string::npos) {
        u32 group = 0;
        if (!parse_uint_after(wgsl, pos + 6, group)) {
          pos += 6;
          continue;
        }
        const usize stmt_end = wgsl.find(';', pos);
        const usize limit = stmt_end == std::string::npos ? wgsl.size() : stmt_end;
        const usize binding_pos = wgsl.find("@binding", pos);
        if (binding_pos == std::string::npos || binding_pos > limit) {
          pos += 6;
          continue;
        }
        u32 binding = 0;
        if (!parse_uint_after(wgsl, binding_pos + 8, binding)) {
          pos = binding_pos + 8;
          continue;
        }
        const std::string decl = wgsl.substr(pos, limit - pos);
        wgsl_resource res{};
        res.group = group;
        res.binding = binding;
        if (decl.find("texture_") != std::string::npos) {
          res.type = wgsl_resource::kind::texture;
        } else if (decl.find("sampler") != std::string::npos) {
          res.type = wgsl_resource::kind::sampler;
        } else {
          res.type = wgsl_resource::kind::uniform_buffer;
        }
        resources.push_back(res);
        pos = limit;
      }
      std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
        if (a.group != b.group)
          return a.group < b.group;
        return a.binding < b.binding;
      });
      resources.erase(std::unique(resources.begin(), resources.end(),
                                  [](const auto& a, const auto& b) {
                                    return a.group == b.group && a.binding == b.binding;
                                  }),
                      resources.end());
      return resources;
    }

    [[nodiscard]] bool uses_group(const std::vector<wgsl_resource>& resources, u32 group) noexcept {
      return std::any_of(resources.begin(), resources.end(),
                         [group](const wgsl_resource& res) { return res.group == group; });
    }

    [[nodiscard]] u64 layout_hash_for(const pipeline_desc& desc,
                                      const std::vector<wgsl_resource>& resources) noexcept {
      u64 h = hash_string(desc.vs_entry);
      h ^= hash_string(desc.fs_entry) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      hash_value(h, desc.vertex_stride);
      hash_value(h, desc.depth_test);
      hash_value(h, desc.blend);
      for (const auto& attr : desc.attrs) {
        hash_value(h, attr.shader_location);
        hash_value(h, attr.offset);
        const auto fmt = enum_value(attr.fmt);
        hash_value(h, fmt);
      }
      for (const auto& res : resources) {
        hash_value(h, res.group);
        hash_value(h, res.binding);
        hash_value(h, enum_value(res.type));
      }
      return h;
    }

    [[nodiscard]] wgpu::Buffer create_buffer(wgpu::Device& device, u64 size,
                                             wgpu::BufferUsage usage, const char* label) {
      wgpu::BufferDescriptor desc{};
      desc.label = label;
      desc.size = size;
      desc.usage = usage;
      return device.CreateBuffer(&desc);
    }

    class dawn_pipeline final : public pipeline {
    public:
      dawn_pipeline(renderer& renderer_ref, pipeline_desc desc)
          : owner_(&renderer_ref),
            access_(dynamic_cast<dawn_pipeline_device_access*>(&renderer_ref)),
            desc_(std::move(desc)), resources_(reflect_wgsl_resources(desc_.wgsl)),
            uses_renderer_bind_group_(uses_group(resources_, 0)),
            uses_user_bind_group_(uses_group(resources_, 1)) {
        if (!access_) {
          throw std::runtime_error(
              "Pipeline requires a Dawn-backed Renderer; this renderer is unsupported");
        }
        pipeline_detail::validate_pipeline_desc(desc_);
        build_shader();
        build_layouts();
        build_pipeline();
        rebuild_user_bind_group();
      }

      void update_uniforms(const void* data, usize bytes) override {
        uniform_bytes_.resize(bytes);
        if (bytes && data)
          std::memcpy(uniform_bytes_.data(), data, bytes);
        ensure_uniform_buffer(bytes);
        if (bytes)
          access_->queue().WriteBuffer(uniform_buffer_, 0, uniform_bytes_.data(), bytes);
        rebuild_user_bind_group();
      }

      void bind_texture(u32 binding, texture_id tex) override {
        if (binding >= bound_textures_.size())
          bound_textures_.resize(static_cast<usize>(binding) + 1, null_texture);
        bound_textures_[binding] = tex;
        rebuild_user_bind_group();
      }

      void draw(command_buffer& cb, const float* vertices, usize vertex_count, const u32* indices,
                usize index_count, const float matrix[16]) override {
        (void)matrix;
        if (dynamic_cast<renderer*>(&cb) != owner_) {
          throw std::runtime_error("Pipeline.draw requires the Dawn renderer used at construction");
        }
        const u64 vertex_bytes = static_cast<u64>(vertex_count) * desc_.vertex_stride;
        ensure_gpu_buffer(vertex_buffer_, vertex_capacity_, vertex_bytes,
                          wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                          "fxe-custom-vbuf");
        if (vertex_bytes && vertices)
          access_->queue().WriteBuffer(vertex_buffer_, 0, vertices, vertex_bytes);

        const u64 index_bytes = static_cast<u64>(index_count) * sizeof(u32);
        ensure_gpu_buffer(index_buffer_, index_capacity_, index_bytes,
                          wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, "fxe-custom-ibuf");
        if (index_bytes && indices)
          access_->queue().WriteBuffer(index_buffer_, 0, indices, index_bytes);

        custom_pipeline_draw draw{};
        draw.pipeline = render_pipeline_;
        draw.vertex_buffer = vertex_buffer_;
        draw.index_buffer = index_buffer_;
        draw.renderer_bind_group =
            uses_renderer_bind_group_ ? access_->renderer_bind_group() : wgpu::BindGroup{};
        draw.user_bind_group = uses_user_bind_group_ ? user_bind_group_ : wgpu::BindGroup{};
        draw.vertex_bytes = vertex_bytes;
        draw.vertex_count = static_cast<u32>(vertex_count);
        draw.index_count = static_cast<u32>(index_count);
        draw.uses_renderer_bind_group = uses_renderer_bind_group_;
        draw.uses_user_bind_group = uses_user_bind_group_ && static_cast<bool>(user_bind_group_);
        access_->enqueue_custom_draw(std::move(draw));
      }

    private:
      void build_shader() {
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = desc_.wgsl.c_str();
        wgpu::ShaderModuleDescriptor sm_desc{};
        sm_desc.nextInChain = &wgsl;
        sm_desc.label = "fxe-custom-pipeline.wgsl";
        shader_ = access_->device().CreateShaderModule(&sm_desc);
        if (!shader_)
          throw std::runtime_error("Pipeline WGSL shader module creation failed");
      }

      void build_layouts() {
        std::vector<wgpu::BindGroupLayout> layouts;
        layouts.reserve(2);
        if (uses_renderer_bind_group_) {
          layouts.push_back(access_->renderer_bind_group_layout());
        } else if (uses_user_bind_group_) {
          wgpu::BindGroupLayoutDescriptor empty_desc{};
          empty_desc.label = "fxe-custom-empty-group0-bgl";
          empty_group0_layout_ = access_->device().CreateBindGroupLayout(&empty_desc);
          layouts.push_back(empty_group0_layout_);
        }

        if (uses_user_bind_group_) {
          build_user_bind_group_layout();
          layouts.push_back(user_bind_group_layout_);
        }

        wgpu::PipelineLayoutDescriptor pl_desc{};
        pl_desc.label = "fxe-custom-pipeline-layout";
        pl_desc.bindGroupLayoutCount = layouts.size();
        pl_desc.bindGroupLayouts = layouts.empty() ? nullptr : layouts.data();
        pipeline_layout_ = access_->device().CreatePipelineLayout(&pl_desc);
      }

      void build_user_bind_group_layout() {
        std::vector<wgpu::BindGroupLayoutEntry> entries;
        for (const auto& res : resources_) {
          if (res.group != 1)
            continue;
          wgpu::BindGroupLayoutEntry entry{};
          entry.binding = res.binding;
          entry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
          switch (res.type) {
          case wgsl_resource::kind::uniform_buffer:
            entry.buffer.type = wgpu::BufferBindingType::Uniform;
            entry.buffer.minBindingSize = 0;
            break;
          case wgsl_resource::kind::texture:
            entry.visibility = wgpu::ShaderStage::Fragment;
            entry.texture.sampleType = wgpu::TextureSampleType::Float;
            entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
            break;
          case wgsl_resource::kind::sampler:
            entry.visibility = wgpu::ShaderStage::Fragment;
            entry.sampler.type = wgpu::SamplerBindingType::Filtering;
            break;
          }
          entries.push_back(entry);
        }
        wgpu::BindGroupLayoutDescriptor desc{};
        desc.label = "fxe-custom-user-bgl";
        desc.entryCount = entries.size();
        desc.entries = entries.empty() ? nullptr : entries.data();
        user_bind_group_layout_ = access_->device().CreateBindGroupLayout(&desc);
      }

      void build_pipeline() {
        auto attrs = make_vertex_attrs(desc_);
        custom_pipeline_key key{};
        key.wgsl_hash = hash_string(desc_.wgsl);
        key.layout_hash = layout_hash_for(desc_, resources_);
        key.vertex_stride = desc_.vertex_stride;
        key.color_format = access_->color_format();
        key.depth_format =
            desc_.depth_test ? access_->depth_format() : wgpu::TextureFormat::Undefined;
        key.blend = desc_.blend ? blend_mode::alpha : blend_mode::none;
        key.topology = wgpu::PrimitiveTopology::TriangleList;
        key.sample_count = access_->sample_count();
        render_pipeline_ = access_->cache().acquire_custom(key, access_->device(), pipeline_layout_,
                                                           shader_, desc_.vs_entry, desc_.fs_entry,
                                                           attrs, desc_.vertex_stride);
      }

      void ensure_uniform_buffer(usize required) {
        const u64 needed = std::max<u64>(kInitialUniformBytes, align_to(required, 16));
        if (uniform_buffer_ && uniform_capacity_ >= needed)
          return;
        uniform_buffer_ = create_buffer(access_->device(), needed,
                                        wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                        "fxe-custom-uniforms");
        uniform_capacity_ = needed;
      }

      void rebuild_user_bind_group() {
        if (!uses_user_bind_group_)
          return;
        std::vector<wgpu::BindGroupEntry> entries;
        for (const auto& res : resources_) {
          if (res.group != 1)
            continue;
          wgpu::BindGroupEntry entry{};
          entry.binding = res.binding;
          switch (res.type) {
          case wgsl_resource::kind::uniform_buffer:
            ensure_uniform_buffer(uniform_bytes_.size());
            entry.buffer = uniform_buffer_;
            entry.size = uniform_capacity_;
            break;
          case wgsl_resource::kind::texture: {
            const texture_id id =
                res.binding < bound_textures_.size() ? bound_textures_[res.binding] : null_texture;
            entry.textureView = access_->texture_view(id);
            break;
          }
          case wgsl_resource::kind::sampler:
            entry.sampler = access_->texture_sampler();
            break;
          }
          entries.push_back(entry);
        }
        wgpu::BindGroupDescriptor desc{};
        desc.label = "fxe-custom-user-bg";
        desc.layout = user_bind_group_layout_;
        desc.entryCount = entries.size();
        desc.entries = entries.empty() ? nullptr : entries.data();
        user_bind_group_ = access_->device().CreateBindGroup(&desc);
      }

      void ensure_gpu_buffer(wgpu::Buffer& buffer, u64& capacity, u64 required,
                             wgpu::BufferUsage usage, const char* label) {
        const u64 needed = std::max<u64>(required, kInitialCustomBufferBytes);
        if (buffer && capacity >= needed)
          return;
        u64 next = capacity ? capacity : kInitialCustomBufferBytes;
        while (next < needed)
          next *= 2;
        buffer = create_buffer(access_->device(), next, usage, label);
        capacity = next;
      }

      renderer* owner_ = nullptr;
      dawn_pipeline_device_access* access_ = nullptr;
      pipeline_desc desc_;
      std::vector<wgsl_resource> resources_;
      bool uses_renderer_bind_group_ = false;
      bool uses_user_bind_group_ = false;
      wgpu::ShaderModule shader_;
      wgpu::PipelineLayout pipeline_layout_;
      wgpu::BindGroupLayout empty_group0_layout_;
      wgpu::BindGroupLayout user_bind_group_layout_;
      wgpu::BindGroup user_bind_group_;
      wgpu::RenderPipeline render_pipeline_;
      wgpu::Buffer uniform_buffer_;
      u64 uniform_capacity_ = 0;
      std::vector<u8> uniform_bytes_;
      std::vector<texture_id> bound_textures_;
      wgpu::Buffer vertex_buffer_;
      wgpu::Buffer index_buffer_;
      u64 vertex_capacity_ = 0;
      u64 index_capacity_ = 0;
    };
  } // namespace

  usize pipeline_key_hash::operator()(const pipeline_key& key) const noexcept {
    usize seed = 0;
    hash_combine(seed, key.vs_entry);
    hash_combine(seed, key.fs_entry);
    hash_combine(seed, enum_value(key.color_format));
    hash_combine(seed, enum_value(key.depth_format));
    hash_combine(seed, enum_value(key.blend));
    hash_combine(seed, enum_value(key.topology));
    hash_combine(seed, key.sample_count);
    return seed;
  }

  usize custom_pipeline_key_hash::operator()(const custom_pipeline_key& key) const noexcept {
    usize seed = 0;
    hash_combine(seed, key.wgsl_hash);
    hash_combine(seed, key.layout_hash);
    hash_combine(seed, key.vertex_stride);
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
    ++miss_count_;
    auto [it, inserted] = pipelines_.emplace(key, pipeline);
    (void)inserted;
    return it->second;
  }

  wgpu::RenderPipeline pipeline_cache::acquire_custom(
      const custom_pipeline_key& key, wgpu::Device device, wgpu::PipelineLayout layout,
      wgpu::ShaderModule shader, const std::string& vs_entry, const std::string& fs_entry,
      const std::vector<wgpu::VertexAttribute>& attrs, u64 vertex_stride) {
    if (auto it = custom_pipelines_.find(key); it != custom_pipelines_.end())
      return it->second;

    wgpu::VertexBufferLayout vb_layout{};
    vb_layout.arrayStride = vertex_stride;
    vb_layout.stepMode = wgpu::VertexStepMode::Vertex;
    vb_layout.attributeCount = attrs.size();
    vb_layout.attributes = attrs.empty() ? nullptr : attrs.data();

    wgpu::BlendState blend{};
    const bool has_blend = configure_blend(key.blend, blend);

    wgpu::ColorTargetState color_target{};
    color_target.format = key.color_format;
    color_target.blend = has_blend ? &blend : nullptr;
    color_target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag_state{};
    frag_state.module = shader;
    frag_state.entryPoint = fs_entry.c_str();
    frag_state.targetCount = 1;
    frag_state.targets = &color_target;

    wgpu::DepthStencilState depth_state{};
    const bool has_depth = key.depth_format != wgpu::TextureFormat::Undefined;
    if (has_depth) {
      depth_state.format = key.depth_format;
      depth_state.depthWriteEnabled = true;
      depth_state.depthCompare = wgpu::CompareFunction::LessEqual;
    }

    const std::string label = "fxe-custom-pipe-" + vs_entry + "-" + fs_entry;
    wgpu::RenderPipelineDescriptor desc{};
    desc.label = label.c_str();
    desc.layout = layout;
    desc.vertex.module = shader;
    desc.vertex.entryPoint = vs_entry.c_str();
    desc.vertex.bufferCount = attrs.empty() ? 0 : 1;
    desc.vertex.buffers = attrs.empty() ? nullptr : &vb_layout;
    desc.primitive.topology = key.topology;
    desc.primitive.frontFace = wgpu::FrontFace::CCW;
    desc.primitive.cullMode = wgpu::CullMode::None;
    desc.multisample.count = key.sample_count;
    desc.multisample.mask = 0xffffffff;
    desc.fragment = &frag_state;
    desc.depthStencil = has_depth ? &depth_state : nullptr;

    auto pipeline = device.CreateRenderPipeline(&desc);
    ++miss_count_;
    auto [it, inserted] = custom_pipelines_.emplace(key, pipeline);
    (void)inserted;
    return it->second;
  }

  std::unique_ptr<pipeline> pipeline::create(renderer& renderer_ref, const pipeline_desc& desc) {
    return std::make_unique<dawn_pipeline>(renderer_ref, desc);
  }
} // namespace fxe
#endif
