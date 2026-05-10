#include <fxe/font.hpp>
#include <fxe/offscreen.hpp>
#include <fxe/spritesheet.hpp>

#include <fxe/window.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fxe/types.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if FXE_HAS_WGPU
#include "pipeline.hpp"
#include <fxe/fxe_shaders.hpp>
#include <webgpu/webgpu_cpp.h>

#endif

namespace fxe {
  namespace {
    class offscreen_stub_window final : public window {
    public:
      explicit offscreen_stub_window(math::uvec2 size) : size_(size) {}
      void poll() override {}
      void wait_events() override {}
      void wait_events_timeout(double) override {}
      void post_redraw() override {}
      bool take_redraw_request() override {
        return false;
      }
      void close() override {
        closed_ = true;
      }
      bool should_close() const override {
        return closed_;
      }
      math::uvec2 framebuffer_size() const override {
        return size_;
      }
      void set_vsync(bool) override {}
      void* native_handle() const override {
        return nullptr;
      }
      void resize(math::uvec2 size) {
        size_ = size;
      }

    private:
      math::uvec2 size_{};
      bool closed_ = false;
    };
  } // namespace

#if !FXE_HAS_WGPU

  namespace {
    class null_offscreen_renderer final : public offscreen_renderer {
    public:
      explicit null_offscreen_renderer(offscreen_options options)
          : options_(sanitize(options)), window_({options_.width, options_.height}) {
        if (!check_multisample_count(options_.multisample)) {
          std::fprintf(stderr,
                       "fxe.offscreen: unsupported multisample count %u; falling back to 1\n",
                       options_.multisample);
          options_.multisample = 1;
        }
        multisample_count_ = options_.multisample;
      }

      void begin_frame(const math::vec3& eye_pos, const math::vec3& eye_dir,
                       const math::mat4x4& world_view_proj) override {
        update_constants(eye_pos, eye_dir, world_view_proj, static_cast<float>(options_.width),
                         static_cast<float>(options_.height));
        clear();
      }

      void end_frame() override {
        pixels_.assign(pixel_count(), 0);
      }

      bool queue_dev(const command_view& src, const vshader_cbuf&, const render_config&) override {
        queue(src);
        return true;
      }

      std::vector<u8> read_rgba8() override {
        if (pixels_.size() != pixel_count())
          pixels_.assign(pixel_count(), 0);
        return pixels_;
      }

      capture_result capture_frame() override {
        capture_result result;
        result.ok = true;
        result.width = options_.width;
        result.height = options_.height;
        result.rgba = read_rgba8();
        return result;
      }

      window& get_window() override {
        return window_;
      }
      const window& get_window() const override {
        return window_;
      }

    private:
      static offscreen_options sanitize(offscreen_options options) {
        options.width = std::max<u32>(options.width, 1);
        options.height = std::max<u32>(options.height, 1);
        options.mip_levels = std::max<u32>(options.mip_levels, 1);
        return options;
      }

      usize pixel_count() const {
        return static_cast<usize>(options_.width) * options_.height * 4u;
      }

      offscreen_options options_{};
      offscreen_stub_window window_;
      std::vector<u8> pixels_;
    };
  } // namespace

  std::unique_ptr<offscreen_renderer> offscreen_renderer::create(const offscreen_options& options) {
    return std::make_unique<null_offscreen_renderer>(options);
  }

#else

  namespace {
    constexpr u64 kUboBytes = (sizeof(vshader_cbuf) + 255ull) & ~static_cast<u64>(255ull);
    constexpr u64 kInitialDynamicBytes = 1u << 20;

    [[nodiscard]] u64 hash_atlas_pixels(const texture_data& tex) noexcept {
      static_assert(sizeof(r8g8b8a8) == 4, "RGBA8 atlas pixel layout drift");
      const auto* bytes = reinterpret_cast<const u8*>(tex.pixels.data());
      const usize byte_count = tex.pixels.size() * sizeof(r8g8b8a8);
      u64 h = 1469598103934665603ull;
      for (usize i = 0; i != byte_count; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
      }
      return h;
    }

    [[nodiscard]] texture_id vertex_texture_id(const vertex& v) noexcept {
      return std::bit_cast<texture_id>(v.uv.z);
    }

    constexpr texture_id kFontColorFlag = 0x080000u;
    constexpr texture_id kFontMaskFlag = 0x040000u;

    [[nodiscard]] primitive_effect classify_triangle(const std::vector<vertex>& vertices,
                                                     const u32* indices) noexcept {
      primitive_effect alpha_effect = primitive_effect::color;
      for (u32 i = 0; i != 3; ++i) {
        if (indices[i] >= vertices.size())
          continue;
        const vertex& v = vertices[indices[i]];
        const texture_id tx = vertex_texture_id(v);
        if (tx == framebuffer_texture_id)
          return primitive_effect::framebuffer_sample;
        if ((tx & kFontMaskFlag) != 0u)
          return primitive_effect::text_mask;
        if ((tx & kFontColorFlag) != 0u)
          return primitive_effect::text_color;
        if (v.color.a < 255)
          alpha_effect = primitive_effect::transparent;
      }
      return alpha_effect;
    }

    template <typename Future> void wait_future(wgpu::Instance& instance, Future fut) {
      wgpu::FutureWaitInfo info{fut};
      auto status = instance.WaitAny(1, &info, UINT64_MAX);
      if (status != wgpu::WaitStatus::Success)
        throw std::runtime_error("wgpu::Instance::WaitAny failed");
    }

    wgpu::Adapter request_adapter(wgpu::Instance& instance) {
      wgpu::Adapter adapter;
      wgpu::RequestAdapterOptions opts{};
      opts.powerPreference = wgpu::PowerPreference::HighPerformance;
      auto fut = instance.RequestAdapter(
          &opts, wgpu::CallbackMode::WaitAnyOnly,
          [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
            if (status == wgpu::RequestAdapterStatus::Success) {
              adapter = std::move(a);
            } else {
              std::string m(msg.data, msg.length);
              std::fprintf(stderr, "fxe.offscreen: RequestAdapter failed: %s\n", m.c_str());
            }
          });
      wait_future(instance, fut);
      if (!adapter)
        throw std::runtime_error("RequestAdapter returned null");
      return adapter;
    }

    wgpu::Device request_device(wgpu::Instance& instance, wgpu::Adapter& adapter) {
      wgpu::Device device;
      wgpu::DeviceDescriptor desc{};
      desc.label = "fxe-offscreen-device";
      desc.SetUncapturedErrorCallback(
          [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
            std::string m(message.data, message.length);
            std::fprintf(stderr, "fxe.offscreen: device error (type=%u): %s\n",
                         static_cast<unsigned>(type), m.c_str());
          });
      auto fut = adapter.RequestDevice(
          &desc, wgpu::CallbackMode::WaitAnyOnly,
          [&device](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
            if (status == wgpu::RequestDeviceStatus::Success) {
              device = std::move(d);
            } else {
              std::string m(msg.data, msg.length);
              std::fprintf(stderr, "fxe.offscreen: RequestDevice failed: %s\n", m.c_str());
            }
          });
      wait_future(instance, fut);
      if (!device)
        throw std::runtime_error("RequestDevice returned null");
      return device;
    }

    void destroy_texture(wgpu::Texture& texture) {
      if (!texture)
        return;
      texture.Destroy();
      texture = {};
    }

    u32 max_mip_levels(u32 width, u32 height) {
      u32 levels = 1;
      u32 size = std::max(width, height);
      while (size > 1) {
        size >>= 1;
        ++levels;
      }
      return levels;
    }

    class dawn_offscreen_renderer final : public offscreen_renderer,
                                          public dawn_pipeline_device_access {
    public:
      using renderer::queue;

      explicit dawn_offscreen_renderer(offscreen_options options)
          : options_(sanitize(options)), window_({options_.width, options_.height}) {
        if (options_.parent_device) {
          // Borrow the caller's device + queue. No private instance/adapter
          // created — texture views from this offscreen will be valid for
          // the parent's bind groups.
          device_ = options_.parent_device;
          queue_ = options_.parent_queue ? options_.parent_queue : device_.GetQueue();
          if (options_.parent_adapter)
            adapter_ = options_.parent_adapter;
        } else {
          wgpu::InstanceDescriptor inst_desc{};
          static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
          inst_desc.requiredFeatureCount = 1;
          inst_desc.requiredFeatures = &kTimedWaitAny;
          instance_ = wgpu::CreateInstance(&inst_desc);
          if (!instance_)
            throw std::runtime_error("wgpu::CreateInstance failed");
          adapter_ = request_adapter(instance_);
          device_ = request_device(instance_, adapter_);
          queue_ = device_.GetQueue();
        }
        if (!check_multisample_count(options_.multisample)) {
          std::fprintf(stderr,
                       "fxe.offscreen: unsupported multisample count %u; falling back to 1\n",
                       options_.multisample);
          options_.multisample = 1;
        }
        multisample_count_ = options_.multisample;
        target_format_ = options_.color_format;
        depth_format_ = options_.depth_format;
        build_resources();
        resize_targets();
      }

      void begin_frame(const math::vec3& eye_pos, const math::vec3& eye_dir,
                       const math::mat4x4& world_view_proj) override {
        sync_default_atlas();
        sync_font_atlases();
        refresh_atlas_bind_group_if_dirty();
        if (recreate_buffers_)
          refresh_pipelines();
        update_constants(eye_pos, eye_dir, world_view_proj, static_cast<float>(options_.width),
                         static_cast<float>(options_.height));
        queue_.WriteBuffer(ubo_, 0, &cbuf_, sizeof(cbuf_));
        clear();
        queued_custom_draws_.clear();
      }

      void end_frame() override {
        sync_default_atlas();
        sync_font_atlases();
        refresh_atlas_bind_group_if_dirty();
        flush_dynamic();

        wgpu::CommandEncoderDescriptor enc_desc{};
        enc_desc.label = "fxe-offscreen-frame";
        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&enc_desc);

        const bool has_blur = has_framebuffer_samples();
        auto encode_draw_pass = [&](wgpu::LoadOp color_load, wgpu::LoadOp depth_load,
                                    blur_draw_mode mode,
                                    const wgpu::TextureView& framebuffer_view = {}) {
          wgpu::RenderPassColorAttachment color{};
          color.view = multisample_count_ > 1 ? msaa_view_ : color_view_;
          if (multisample_count_ > 1)
            color.resolveTarget = color_view_;
          color.loadOp = color_load;
          color.storeOp = wgpu::StoreOp::Store;
          auto cv = clear_color();
          color.clearValue = {static_cast<double>(cv.x), static_cast<double>(cv.y),
                              static_cast<double>(cv.z), static_cast<double>(cv.w)};
          color.depthSlice = wgpu::kDepthSliceUndefined;

          wgpu::RenderPassDepthStencilAttachment depth{};
          depth.view = depth_view_;
          depth.depthLoadOp = depth_load;
          depth.depthStoreOp = wgpu::StoreOp::Store;
          depth.depthClearValue = 1.0f;

          wgpu::RenderPassDescriptor pass_desc{};
          pass_desc.colorAttachmentCount = 1;
          pass_desc.colorAttachments = &color;
          if (depth_view_)
            pass_desc.depthStencilAttachment = &depth;
          wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
          const wgpu::BindGroup frame_bind_group =
              framebuffer_view
                  ? create_bind_group(ubo_, "fxe-offscreen-bg-framebuffer", framebuffer_view)
                  : bind_group_;
          pass.SetBindGroup(0, frame_bind_group);
          if (vbuf_size_used_ > 0) {
            pass.SetVertexBuffer(0, vbuf_, 0, vbuf_size_used_);
            draw_indexed(pass, mode);
          }
          if (mode != blur_draw_mode::composite)
            encode_custom_draws(pass);
          pass.End();
        };

        auto encode_post_process_pass = [&](wgpu::RenderPipeline pipeline,
                                            const wgpu::TextureView& src_view,
                                            const wgpu::TextureView& dst_view, const char* label) {
          wgpu::RenderPassColorAttachment color{};
          color.view = dst_view;
          color.loadOp = wgpu::LoadOp::Clear;
          color.storeOp = wgpu::StoreOp::Store;
          color.clearValue = {0.0, 0.0, 0.0, 0.0};
          color.depthSlice = wgpu::kDepthSliceUndefined;
          wgpu::RenderPassDescriptor pass_desc{};
          pass_desc.label = label;
          pass_desc.colorAttachmentCount = 1;
          pass_desc.colorAttachments = &color;
          wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
          pass.SetPipeline(pipeline);
          pass.SetBindGroup(0, create_bind_group(ubo_, label, src_view));
          if (vbuf_size_used_ > 0)
            pass.SetVertexBuffer(0, vbuf_, 0, vbuf_size_used_);
          pass.Draw(3, 1, 0, 0);
          pass.End();
        };

        const bool blur_ready = blur_capture_texture_ && blur_capture_view_ && blur_ping_texture_ &&
                                blur_ping_view_ && blur_pong_texture_ && blur_pong_view_;
        if (has_blur && !blur_ready && !blur_texture_failure_logged_) {
          std::fprintf(stderr, "fxe.offscreen: blur intermediate texture allocation failed; "
                               "falling back to unblurred base\n");
          blur_texture_failure_logged_ = true;
        }

        if (has_blur && blur_ready) {
          encode_draw_pass(wgpu::LoadOp::Clear, wgpu::LoadOp::Clear, blur_draw_mode::base);
          wgpu::TexelCopyTextureInfo blur_src{};
          blur_src.texture = color_texture_;
          blur_src.mipLevel = 0;
          blur_src.origin = {0, 0, 0};
          blur_src.aspect = wgpu::TextureAspect::All;
          wgpu::TexelCopyTextureInfo blur_dst{};
          blur_dst.texture = blur_capture_texture_;
          blur_dst.mipLevel = 0;
          blur_dst.origin = {0, 0, 0};
          blur_dst.aspect = wgpu::TextureAspect::All;
          wgpu::Extent3D blur_extent{options_.width, options_.height, 1};
          encoder.CopyTextureToTexture(&blur_src, &blur_dst, &blur_extent);
          encode_post_process_pass(vblur_pipeline_, blur_capture_view_, blur_ping_view_,
                                   "fxe-offscreen-vblur-pass");
          encode_post_process_pass(hblur_pipeline_, blur_ping_view_, blur_pong_view_,
                                   "fxe-offscreen-hblur-pass");
          encode_draw_pass(wgpu::LoadOp::Load, wgpu::LoadOp::Load, blur_draw_mode::composite,
                           blur_pong_view_);
        } else {
          const blur_draw_mode mode = has_blur ? blur_draw_mode::base : blur_draw_mode::all;
          encode_draw_pass(wgpu::LoadOp::Clear, wgpu::LoadOp::Clear, mode);
        }

        const u32 row_bytes = options_.width * 4u;
        readback_padded_row_ = (row_bytes + 255u) & ~static_cast<u32>(255u);
        const u64 readback_size = static_cast<u64>(readback_padded_row_) * options_.height;
        ensure_readback_buffer(readback_size);

        wgpu::TexelCopyTextureInfo src{};
        src.texture = color_texture_;
        src.mipLevel = 0;
        src.origin = {0, 0, 0};
        src.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback_buf_;
        dst.layout.offset = 0;
        dst.layout.bytesPerRow = readback_padded_row_;
        dst.layout.rowsPerImage = options_.height;
        wgpu::Extent3D extent{options_.width, options_.height, 1};
        encoder.CopyTextureToBuffer(&src, &dst, &extent);

        wgpu::CommandBufferDescriptor cb_desc{};
        wgpu::CommandBuffer cmds = encoder.Finish(&cb_desc);
        queue_.Submit(1, &cmds);
        finish_readback(readback_size);
        if (instance_)
          instance_.ProcessEvents();
      }

      bool queue_dev(const command_view& src, const vshader_cbuf&, const render_config&) override {
        upload_buf_.queue(src);
        return true;
      }

      void set_atlas(u32 w, u32 h, const u8* rgba) override {
        if (!w || !h || !rgba) {
          create_default_atlas();
          return;
        }
        upload_atlas_pixels(w, h, rgba);
      }

      std::vector<u8> read_rgba8() override {
        if (pixels_.size() != pixel_count())
          pixels_.assign(pixel_count(), 0);
        return pixels_;
      }

      capture_result capture_frame() override {
        capture_result result;
        result.ok = true;
        result.width = options_.width;
        result.height = options_.height;
        result.rgba = read_rgba8();
        return result;
      }

      wgpu::TextureView color_texture_view() const override {
        return color_view_;
      }

      window& get_window() override {
        return window_;
      }
      const window& get_window() const override {
        return window_;
      }

      wgpu::Device& device() override {
        return device_;
      }
      wgpu::Queue& queue() override {
        return queue_;
      }
      wgpu::TextureFormat color_format() const override {
        return target_format_;
      }
      wgpu::TextureFormat depth_format() const override {
        return options_.enable_depth ? depth_format_ : wgpu::TextureFormat::Undefined;
      }
      u32 sample_count() const override {
        return multisample_count_;
      }
      pipeline_cache& cache() override {
        return pipeline_cache_;
      }
      wgpu::BindGroupLayout renderer_bind_group_layout() const override {
        return bgl_;
      }
      wgpu::BindGroup renderer_bind_group() const override {
        return bind_group_;
      }
      wgpu::TextureView texture_view(texture_id id) const override {
        if (id == framebuffer_texture_id && blur_capture_view_)
          return blur_capture_view_;
        return atlas_view_;
      }
      wgpu::Sampler texture_sampler() const override {
        return atlas_sampler_;
      }
      void enqueue_custom_draw(custom_pipeline_draw draw) override {
        queued_custom_draws_.push_back(std::move(draw));
      }

      // Bind / unbind a user texture slot (0..3) on the offscreen, mirroring
      // `renderer_dawn::bind_user_texture`. Surface-cache bakes copy the
      // parent renderer's slot bindings into their offscreen before queueing
      // the cached buffer so nested `drawTextureQuad(slot=k, ...)` references
      // sample the actual bound texture instead of the offscreen's
      // placeholder atlas. Empty view reverts the slot to atlas_view_.
      void bind_user_texture(u32 slot, wgpu::TextureView view) override {
        if (slot >= user_tex_views_.size())
          return;
        user_tex_views_[slot] = std::move(view);
        atlas_dirty_ = true;
      }

    private:
      static offscreen_options sanitize(offscreen_options options) {
        options.width = std::max<u32>(options.width, 1);
        options.height = std::max<u32>(options.height, 1);
        options.mip_levels = std::max<u32>(options.mip_levels, 1);
        return options;
      }

      usize pixel_count() const {
        return static_cast<usize>(options_.width) * options_.height * 4u;
      }

      void build_resources() {
        wgpu::ShaderSourceWGSL wgsl{};
        std::string code(reinterpret_cast<const char*>(shaders::main_wgsl),
                         shaders::main_wgsl_size);
        wgsl.code = code.c_str();
        wgpu::ShaderModuleDescriptor sm_desc{};
        sm_desc.nextInChain = &wgsl;
        sm_desc.label = "fxe-main.wgsl";
        shader_ = device_.CreateShaderModule(&sm_desc);

        wgpu::BufferDescriptor ubo_desc{};
        ubo_desc.label = "fxe-offscreen-ubo";
        ubo_desc.size = kUboBytes;
        ubo_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        ubo_ = device_.CreateBuffer(&ubo_desc);

        std::array<wgpu::BindGroupLayoutEntry, 12> bgl_entries{};
        bgl_entries[0].binding = 0;
        bgl_entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        bgl_entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        bgl_entries[1].binding = 1;
        bgl_entries[1].visibility = wgpu::ShaderStage::Fragment;
        bgl_entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
        for (u32 i = 2; i < 5; ++i) {
          bgl_entries[i].binding = i;
          bgl_entries[i].visibility = wgpu::ShaderStage::Fragment;
          bgl_entries[i].texture.sampleType = wgpu::TextureSampleType::Float;
          bgl_entries[i].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        }
        bgl_entries[5].binding = 5;
        bgl_entries[5].visibility = wgpu::ShaderStage::Fragment;
        bgl_entries[5].sampler.type = wgpu::SamplerBindingType::Filtering;
        bgl_entries[6].binding = 6;
        bgl_entries[6].visibility = wgpu::ShaderStage::Fragment;
        bgl_entries[6].texture.sampleType = wgpu::TextureSampleType::Float;
        bgl_entries[6].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        // Nearest sampler dedicated to the font mask page (matches main.wgsl).
        bgl_entries[7].binding = 7;
        bgl_entries[7].visibility = wgpu::ShaderStage::Fragment;
        bgl_entries[7].sampler.type = wgpu::SamplerBindingType::NonFiltering;
        // User texture slots (binding 8..11). Mirrors the main renderer
        // layout so a shared shader compiles unchanged.
        for (u32 i = 8; i < 12; ++i) {
          bgl_entries[i].binding = i;
          bgl_entries[i].visibility = wgpu::ShaderStage::Fragment;
          bgl_entries[i].texture.sampleType = wgpu::TextureSampleType::Float;
          bgl_entries[i].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        }
        wgpu::BindGroupLayoutDescriptor bgl_desc{};
        bgl_desc.label = "fxe-offscreen-bgl";
        bgl_desc.entryCount = bgl_entries.size();
        bgl_desc.entries = bgl_entries.data();
        bgl_ = device_.CreateBindGroupLayout(&bgl_desc);

        wgpu::SamplerDescriptor sampler_desc{};
        sampler_desc.label = "fxe-offscreen-atlas-sampler";
        sampler_desc.minFilter = wgpu::FilterMode::Linear;
        sampler_desc.magFilter = wgpu::FilterMode::Linear;
        sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        sampler_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
        sampler_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
        atlas_sampler_ = device_.CreateSampler(&sampler_desc);
        // Nearest sampler dedicated to the font mask page (matches main.wgsl @binding(7)).
        wgpu::SamplerDescriptor mask_desc{};
        mask_desc.label = "fxe-offscreen-mask-sampler";
        mask_desc.minFilter = wgpu::FilterMode::Nearest;
        mask_desc.magFilter = wgpu::FilterMode::Nearest;
        mask_desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        mask_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
        mask_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
        mask_sampler_ = device_.CreateSampler(&mask_desc);
        create_default_atlas();
        bind_group_ = create_bind_group(ubo_, "fxe-offscreen-bg");

        wgpu::PipelineLayoutDescriptor pl_desc{};
        pl_desc.label = "fxe-offscreen-plo";
        pl_desc.bindGroupLayoutCount = 1;
        pl_desc.bindGroupLayouts = &bgl_;
        pipeline_layout_ = device_.CreatePipelineLayout(&pl_desc);

        pipeline_cache_.clear();
        refresh_pipelines();

        ensure_buffer(vbuf_, vbuf_capacity_, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                      kInitialDynamicBytes, "fxe-offscreen-vbuf");
        ensure_buffer(tri_ibuf_, tri_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, kInitialDynamicBytes,
                      "fxe-offscreen-tri-ibuf");
        ensure_buffer(line_ibuf_, line_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, kInitialDynamicBytes,
                      "fxe-offscreen-line-ibuf");
      }

      void refresh_pipelines() {
        pipeline_key key{};
        key.vs_entry = "vs_transform";
        key.fs_entry = "ps_opaque";
        key.color_format = target_format_;
        key.depth_format = options_.enable_depth ? depth_format_ : wgpu::TextureFormat::Undefined;
        key.blend = blend_mode::none;
        key.topology = wgpu::PrimitiveTopology::TriangleList;
        key.sample_count = multisample_count_;
        triangle_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);

        key.blend = blend_mode::alpha;
        key.fs_entry = "ps_transparent";
        transparent_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_text_mask";
        text_mask_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_text_color";
        text_color_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_framebuffer_sample";
        sample_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);

        key.fs_entry = "ps_opaque";
        key.topology = wgpu::PrimitiveTopology::LineList;
        key.blend = current_blend_mode();
        line_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);

        key.vs_entry = "vs_fullscreen";
        key.topology = wgpu::PrimitiveTopology::TriangleList;
        key.depth_format = wgpu::TextureFormat::Undefined;
        key.blend = blend_mode::none;
        key.sample_count = 1;
        key.fs_entry = "ps_vblur";
        vblur_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_hblur";
        hblur_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_lum_filter";
        lum_filter_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        recreate_buffers_ = false;
      }

      wgpu::BindGroup create_bind_group(const wgpu::Buffer& ubo, const char* label,
                                        const wgpu::TextureView& framebuffer_view = {}) {
        std::array<wgpu::BindGroupEntry, 12> entries{};
        entries[0].binding = 0;
        entries[0].buffer = ubo;
        entries[0].size = kUboBytes;
        entries[1].binding = 1;
        entries[1].sampler = atlas_sampler_;
        entries[2].binding = 2;
        entries[2].textureView = atlas_view_;
        entries[3].binding = 3;
        entries[3].textureView = font_mask_view_ ? font_mask_view_ : atlas_view_;
        entries[4].binding = 4;
        entries[4].textureView = font_color_view_ ? font_color_view_ : atlas_view_;
        entries[5].binding = 5;
        entries[5].sampler = atlas_sampler_;
        entries[6].binding = 6;
        entries[6].textureView = framebuffer_view
                                     ? framebuffer_view
                                     : (blur_capture_view_ ? blur_capture_view_ : atlas_view_);
        entries[7].binding = 7;
        entries[7].sampler = mask_sampler_;
        // User texture slots. By default these point at the offscreen's own
        // atlas placeholder, but surface-cache bakes mirror the parent
        // renderer's bindings via `bind_user_texture` so cached subtrees that
        // sample nested surfaces (`drawTextureQuad(slot=k, …)`) hit the
        // intended views instead of the placeholder.
        for (u32 i = 0; i < 4; ++i) {
          entries[8 + i].binding = 8 + i;
          entries[8 + i].textureView = user_tex_views_[i] ? user_tex_views_[i] : atlas_view_;
        }
        wgpu::BindGroupDescriptor bg_desc{};
        bg_desc.label = label;
        bg_desc.layout = bgl_;
        bg_desc.entryCount = entries.size();
        bg_desc.entries = entries.data();
        return device_.CreateBindGroup(&bg_desc);
      }

      void create_default_atlas() {
        const u8 white[4] = {255, 255, 255, 255};
        upload_atlas_pixels(1, 1, white);
      }

      void upload_atlas_pixels(u32 w, u32 h, const u8* rgba) {
        const u32 max_levels = max_mip_levels(w, h);
        const u32 mip_levels = std::min(options_.mip_levels, max_levels);
        if (mip_levels != options_.mip_levels) {
          std::fprintf(stderr, "fxe.offscreen: mipLevels %u exceeds atlas maximum %u; clamping\n",
                       options_.mip_levels, max_levels);
        }
        wgpu::TextureDescriptor td{};
        td.label = "fxe-offscreen-atlas";
        td.dimension = wgpu::TextureDimension::e2D;
        td.size = {w, h, 1};
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        td.mipLevelCount = mip_levels;
        td.sampleCount = 1;
        atlas_view_ = {};
        destroy_texture(atlas_texture_);
        atlas_texture_ = device_.CreateTexture(&td);
        atlas_view_ = atlas_texture_.CreateView();

        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = atlas_texture_;
        dst.mipLevel = 0;
        dst.origin = {0, 0, 0};
        dst.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyBufferLayout layout{};
        layout.offset = 0;
        layout.bytesPerRow = w * 4;
        layout.rowsPerImage = h;
        wgpu::Extent3D extent{w, h, 1};
        queue_.WriteTexture(&dst, rgba, static_cast<usize>(w) * h * 4u, &layout, &extent);
        atlas_dirty_ = true;
      }

      void sync_default_atlas() {
        auto& sheet = get_default_spritesheet();
        const texture_id atlas_id = sheet.default_font.texture & sprite_mask;
        if (atlas_id == null_texture || atlas_id > sheet.textures.size())
          return;

        const auto& atlas = sheet.textures[atlas_id - 1];
        if (atlas.size.x == 0 || atlas.size.y == 0 || atlas.pixels.empty())
          return;

        const u64 pixel_hash = hash_atlas_pixels(atlas);
        if (synced_default_atlas_id_ == atlas_id && synced_default_atlas_w_ == atlas.size.x &&
            synced_default_atlas_h_ == atlas.size.y && synced_default_atlas_hash_ == pixel_hash) {
          return;
        }

        upload_atlas_pixels(atlas.size.x, atlas.size.y,
                            reinterpret_cast<const u8*>(atlas.pixels.data()));
        synced_default_atlas_id_ = atlas_id;
        synced_default_atlas_w_ = atlas.size.x;
        synced_default_atlas_h_ = atlas.size.y;
        synced_default_atlas_hash_ = pixel_hash;
      }

      void sync_font_atlases() {
        auto& gc = font::shared_glyph_cache();
        sync_one_font_atlas(gc.mask_atlas(), font_mask_texture_, font_mask_view_, font_mask_w_,
                            font_mask_h_, font_mask_gen_, wgpu::TextureFormat::R8Unorm,
                            "fxe-offscreen-font-mask");
        sync_one_font_atlas(gc.color_atlas(), font_color_texture_, font_color_view_, font_color_w_,
                            font_color_h_, font_color_gen_, wgpu::TextureFormat::BGRA8Unorm,
                            "fxe-offscreen-font-color");
      }

      void sync_one_font_atlas(const font::Atlas& src, wgpu::Texture& tex, wgpu::TextureView& view,
                               u32& cur_w, u32& cur_h, u64& cur_gen, wgpu::TextureFormat fmt,
                               const char* label) {
        const auto sz = src.size();
        const u32 w = sz.x;
        const u32 h = sz.y;
        if (w == 0 || h == 0)
          return;
        const u64 gen = src.generation();
        if (cur_gen == gen && cur_w == w && cur_h == h)
          return;

        if (cur_w != w || cur_h != h || !tex) {
          view = {};
          destroy_texture(tex);
          wgpu::TextureDescriptor td{};
          td.label = label;
          td.dimension = wgpu::TextureDimension::e2D;
          td.size = {w, h, 1};
          td.format = fmt;
          td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
          td.mipLevelCount = 1;
          td.sampleCount = 1;
          tex = device_.CreateTexture(&td);
          view = tex.CreateView();
          cur_w = w;
          cur_h = h;
          atlas_dirty_ = true;
        }

        const u32 bpp = src.bytes_per_pixel();
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = tex;
        dst.mipLevel = 0;
        dst.origin = {0, 0, 0};
        dst.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyBufferLayout layout{};
        layout.offset = 0;
        layout.bytesPerRow = w * bpp;
        layout.rowsPerImage = h;
        wgpu::Extent3D extent{w, h, 1};
        queue_.WriteTexture(&dst, src.pixels().data(), static_cast<usize>(w) * h * bpp, &layout,
                            &extent);
        cur_gen = gen;
      }

      void refresh_atlas_bind_group_if_dirty() {
        if (!atlas_dirty_)
          return;
        bind_group_ = create_bind_group(ubo_, "fxe-offscreen-bg");
        atlas_dirty_ = false;
      }

      void resize_targets() {
        color_view_ = {};
        destroy_texture(color_texture_);
        msaa_view_ = {};
        destroy_texture(msaa_texture_);
        depth_view_ = {};
        destroy_texture(depth_texture_);
        wgpu::TextureDescriptor color_desc{};
        color_desc.label = "fxe-offscreen-color";
        color_desc.dimension = wgpu::TextureDimension::e2D;
        color_desc.size = {options_.width, options_.height, 1};
        color_desc.format = target_format_;
        // TextureBinding so the rendered color image can be sampled in the
        // main pass via the user-texture slots (surface caching path).
        color_desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                           wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding;
        color_desc.sampleCount = 1;
        color_texture_ = device_.CreateTexture(&color_desc);
        color_view_ = color_texture_.CreateView();

        if (multisample_count_ > 1) {
          wgpu::TextureDescriptor msaa_desc = color_desc;
          msaa_desc.label = "fxe-offscreen-msaa-color";
          msaa_desc.usage = wgpu::TextureUsage::RenderAttachment;
          msaa_desc.sampleCount = multisample_count_;
          msaa_texture_ = device_.CreateTexture(&msaa_desc);
          msaa_view_ = msaa_texture_.CreateView();
        }

        blur_capture_view_ = {};
        blur_ping_view_ = {};
        blur_pong_view_ = {};
        destroy_texture(blur_capture_texture_);
        destroy_texture(blur_ping_texture_);
        destroy_texture(blur_pong_texture_);
        wgpu::TextureDescriptor blur_desc = color_desc;
        blur_desc.usage = wgpu::TextureUsage::RenderAttachment |
                          wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst |
                          wgpu::TextureUsage::CopySrc;
        blur_desc.sampleCount = 1;
        blur_desc.label = "fxe-offscreen-blur-capture";
        blur_capture_texture_ = device_.CreateTexture(&blur_desc);
        blur_capture_view_ = blur_capture_texture_.CreateView();
        blur_desc.label = "fxe-offscreen-blur-ping";
        blur_ping_texture_ = device_.CreateTexture(&blur_desc);
        blur_ping_view_ = blur_ping_texture_.CreateView();
        blur_desc.label = "fxe-offscreen-blur-pong";
        blur_pong_texture_ = device_.CreateTexture(&blur_desc);
        blur_pong_view_ = blur_pong_texture_.CreateView();
        if (bgl_)
          bind_group_ = create_bind_group(ubo_, "fxe-offscreen-bg");

        if (options_.enable_depth) {
          wgpu::TextureDescriptor depth_desc{};
          depth_desc.label = "fxe-offscreen-depth";
          depth_desc.dimension = wgpu::TextureDimension::e2D;
          depth_desc.size = {options_.width, options_.height, 1};
          depth_desc.format = depth_format_;
          depth_desc.usage = wgpu::TextureUsage::RenderAttachment;
          depth_desc.sampleCount = multisample_count_;
          depth_texture_ = device_.CreateTexture(&depth_desc);
          depth_view_ = depth_texture_.CreateView();
        }
      }

      void ensure_buffer(wgpu::Buffer& buf, u64& cap, wgpu::BufferUsage usage, u64 needed,
                         const char* label) {
        if (cap >= needed)
          return;
        u64 new_cap = cap ? cap : kInitialDynamicBytes;
        while (new_cap < needed)
          new_cap *= 2;
        wgpu::BufferDescriptor desc{};
        desc.label = label;
        desc.size = new_cap;
        desc.usage = usage;
        buf = device_.CreateBuffer(&desc);
        cap = new_cap;
      }

      void flush_dynamic() {
        const u64 vbytes = upload_buf_.vertex_buffer.size() * sizeof(vertex);
        const u64 tri_idx_bytes =
            upload_buf_.index_buffers[static_cast<usize>(vertex_topology::triangle)].size() *
            sizeof(u32);
        const u64 line_idx_bytes =
            upload_buf_.index_buffers[static_cast<usize>(vertex_topology::line)].size() *
            sizeof(u32);
        ensure_buffer(vbuf_, vbuf_capacity_, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                      vbytes ? vbytes : kInitialDynamicBytes, "fxe-offscreen-vbuf");
        ensure_buffer(
            tri_ibuf_, tri_ibuf_capacity_, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
            tri_idx_bytes ? tri_idx_bytes : kInitialDynamicBytes, "fxe-offscreen-tri-ibuf");
        ensure_buffer(
            line_ibuf_, line_ibuf_capacity_, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
            line_idx_bytes ? line_idx_bytes : kInitialDynamicBytes, "fxe-offscreen-line-ibuf");
        if (vbytes)
          queue_.WriteBuffer(vbuf_, 0, upload_buf_.vertex_buffer.data(), vbytes);
        if (tri_idx_bytes)
          queue_.WriteBuffer(
              tri_ibuf_, 0,
              upload_buf_.index_buffers[static_cast<usize>(vertex_topology::triangle)].data(),
              tri_idx_bytes);
        if (line_idx_bytes)
          queue_.WriteBuffer(
              line_ibuf_, 0,
              upload_buf_.index_buffers[static_cast<usize>(vertex_topology::line)].data(),
              line_idx_bytes);
        vbuf_size_used_ = vbytes;
      }

      enum class blur_draw_mode { all, base, composite };

      [[nodiscard]] bool has_framebuffer_samples() const {
        const auto& tri = upload_buf_.index_buffers[usize(vertex_topology::triangle)];
        for (usize i = 0; i + 2 < tri.size(); i += 3) {
          if (classify_triangle(upload_buf_.vertex_buffer, &tri[i]) ==
              primitive_effect::framebuffer_sample)
            return true;
        }
        return false;
      }

      [[nodiscard]] wgpu::RenderPipeline pipeline_for_effect(primitive_effect effect) const {
        switch (effect) {
        case primitive_effect::transparent:
          return transparent_pipeline_;
        case primitive_effect::text_mask:
          return text_mask_pipeline_;
        case primitive_effect::text_color:
          return text_color_pipeline_;
        case primitive_effect::framebuffer_sample:
          return sample_pipeline_;
        case primitive_effect::vertical_blur:
          return vblur_pipeline_;
        case primitive_effect::horizontal_blur:
          return hblur_pipeline_;
        case primitive_effect::luminance_filter:
          return lum_filter_pipeline_;
        case primitive_effect::color:
        default:
          return triangle_pipeline_;
        }
      }

      void draw_triangle_batch(wgpu::RenderPassEncoder& pass, primitive_effect effect, u32 first,
                               u32 count) {
        if (count == 0)
          return;
        pass.SetPipeline(pipeline_for_effect(effect));
        pass.SetIndexBuffer(tri_ibuf_, wgpu::IndexFormat::Uint32, u64(first) * sizeof(u32),
                            count * sizeof(u32));
        pass.DrawIndexed(count, 1, 0, 0, 0);
      }

      void draw_triangles(wgpu::RenderPassEncoder& pass, blur_draw_mode mode) {
        const auto& tri = upload_buf_.index_buffers[usize(vertex_topology::triangle)];
        u32 batch_first = 0;
        u32 batch_count = 0;
        primitive_effect batch_effect = primitive_effect::color;
        auto flush = [&] {
          draw_triangle_batch(pass, batch_effect, batch_first, batch_count);
          batch_count = 0;
        };
        for (usize i = 0; i + 2 < tri.size(); i += 3) {
          const primitive_effect effect = classify_triangle(upload_buf_.vertex_buffer, &tri[i]);
          const bool composite_effect = effect == primitive_effect::framebuffer_sample ||
                                        effect == primitive_effect::transparent;
          if (mode == blur_draw_mode::base && composite_effect)
            continue;
          if (mode == blur_draw_mode::composite && !composite_effect)
            continue;
          const u32 rel = static_cast<u32>(i);
          if (batch_count == 0) {
            batch_first = rel;
            batch_count = 3;
            batch_effect = effect;
          } else if (effect == batch_effect && batch_first + batch_count == rel) {
            batch_count += 3;
          } else {
            flush();
            batch_first = rel;
            batch_count = 3;
            batch_effect = effect;
          }
        }
        flush();
      }

      void draw_indexed(wgpu::RenderPassEncoder& pass, blur_draw_mode mode = blur_draw_mode::all) {
        draw_triangles(pass, mode);
        const usize top = static_cast<usize>(vertex_topology::line);
        const u32 count = static_cast<u32>(upload_buf_.index_buffers[top].size());
        if (count > 0 && mode != blur_draw_mode::composite) {
          pass.SetPipeline(line_pipeline_);
          pass.SetIndexBuffer(line_ibuf_, wgpu::IndexFormat::Uint32, 0, count * sizeof(u32));
          pass.DrawIndexed(count, 1, 0, 0, 0);
        }
      }

      void encode_custom_draws(wgpu::RenderPassEncoder& pass) {
        for (const auto& draw : queued_custom_draws_) {
          if (!draw.pipeline || !draw.vertex_buffer || draw.vertex_count == 0)
            continue;
          pass.SetPipeline(draw.pipeline);
          if (draw.uses_renderer_bind_group && draw.renderer_bind_group)
            pass.SetBindGroup(0, draw.renderer_bind_group);
          if (draw.uses_user_bind_group && draw.user_bind_group)
            pass.SetBindGroup(1, draw.user_bind_group);
          pass.SetVertexBuffer(0, draw.vertex_buffer, 0, draw.vertex_bytes);
          if (draw.index_count > 0 && draw.index_buffer) {
            pass.SetIndexBuffer(draw.index_buffer, wgpu::IndexFormat::Uint32, 0,
                                u64(draw.index_count) * sizeof(u32));
            pass.DrawIndexed(draw.index_count, 1, 0, 0, 0);
          } else {
            pass.Draw(draw.vertex_count, 1, 0, 0);
          }
        }
      }

      void ensure_readback_buffer(u64 needed) {
        if (readback_size_ >= needed)
          return;
        wgpu::BufferDescriptor bd{};
        bd.label = "fxe-offscreen-readback";
        bd.size = needed;
        bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        readback_buf_ = device_.CreateBuffer(&bd);
        readback_size_ = needed;
      }

      void finish_readback(u64 needed) {
        // Offscreen read_rgba8() is a test-only synchronous API. Keep it bounded:
        // never use an infinite WaitAny; instead pump Dawn events until the
        // non-blocking map callback fires or a finite timeout expires.
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        auto fut = readback_buf_.MapAsync(
            wgpu::MapMode::Read, 0, needed, wgpu::CallbackMode::AllowSpontaneous,
            [&done, &ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
              ok.store(status == wgpu::MapAsyncStatus::Success, std::memory_order_release);
              done.store(true, std::memory_order_release);
            });
        (void)fut;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
          if (instance_)
            instance_.ProcessEvents();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (instance_)
          instance_.ProcessEvents();
        if (!done.load(std::memory_order_acquire) || !ok.load(std::memory_order_acquire))
          return;
        const auto* mapped = static_cast<const u8*>(readback_buf_.GetConstMappedRange(0, needed));
        if (!mapped) {
          readback_buf_.Unmap();
          return;
        }
        const u32 row_bytes = options_.width * 4u;
        pixels_.assign(pixel_count(), 0);
        for (u32 y = 0; y != options_.height; ++y) {
          const u8* in = mapped + static_cast<usize>(y) * readback_padded_row_;
          u8* out = pixels_.data() + static_cast<usize>(y) * row_bytes;
          std::memcpy(out, in, row_bytes);
        }
        readback_buf_.Unmap();
      }

      offscreen_options options_{};
      offscreen_stub_window window_;
      wgpu::Instance instance_;
      wgpu::Adapter adapter_;
      wgpu::Device device_;
      wgpu::Queue queue_;
      wgpu::TextureFormat target_format_ = wgpu::TextureFormat::RGBA8Unorm;
      wgpu::TextureFormat depth_format_ = wgpu::TextureFormat::Depth24Plus;
      wgpu::Buffer ubo_;
      wgpu::Buffer vbuf_;
      wgpu::Buffer tri_ibuf_;
      wgpu::Buffer line_ibuf_;
      u64 vbuf_capacity_ = 0;
      u64 tri_ibuf_capacity_ = 0;
      u64 line_ibuf_capacity_ = 0;
      u64 vbuf_size_used_ = 0;
      std::vector<custom_pipeline_draw> queued_custom_draws_;
      wgpu::BindGroupLayout bgl_;
      wgpu::BindGroup bind_group_;
      wgpu::PipelineLayout pipeline_layout_;
      wgpu::ShaderModule shader_;
      wgpu::RenderPipeline triangle_pipeline_;
      wgpu::RenderPipeline transparent_pipeline_;
      wgpu::RenderPipeline line_pipeline_;
      pipeline_cache pipeline_cache_;
      wgpu::RenderPipeline text_mask_pipeline_;
      wgpu::RenderPipeline text_color_pipeline_;
      wgpu::RenderPipeline sample_pipeline_;
      wgpu::RenderPipeline vblur_pipeline_;
      wgpu::RenderPipeline hblur_pipeline_;
      wgpu::RenderPipeline lum_filter_pipeline_;
      wgpu::Texture color_texture_;
      wgpu::TextureView color_view_;
      wgpu::Texture msaa_texture_;
      wgpu::TextureView msaa_view_;
      wgpu::Texture depth_texture_;
      wgpu::TextureView depth_view_;
      wgpu::Texture blur_capture_texture_;
      wgpu::TextureView blur_capture_view_;
      wgpu::Texture blur_ping_texture_;
      wgpu::TextureView blur_ping_view_;
      wgpu::Texture blur_pong_texture_;
      wgpu::TextureView blur_pong_view_;
      bool blur_texture_failure_logged_ = false;
      wgpu::Texture atlas_texture_;
      wgpu::TextureView atlas_view_;
      // Optional user-texture slot bindings. Empty entries fall back to
      // atlas_view_ in the bind group. Surface-cache bakes populate these
      // via bind_user_texture before queueing a parent's cached buffer.
      std::array<wgpu::TextureView, 4> user_tex_views_{};
      wgpu::Sampler atlas_sampler_;
      wgpu::Sampler mask_sampler_;
      bool atlas_dirty_ = false;
      texture_id synced_default_atlas_id_ = null_texture;
      u32 synced_default_atlas_w_ = 0;
      u32 synced_default_atlas_h_ = 0;
      u64 synced_default_atlas_hash_ = 0;
      wgpu::Texture font_mask_texture_;
      wgpu::TextureView font_mask_view_;
      wgpu::Texture font_color_texture_;
      wgpu::TextureView font_color_view_;
      u32 font_mask_w_ = 0;
      u32 font_mask_h_ = 0;
      u32 font_color_w_ = 0;
      u32 font_color_h_ = 0;
      u64 font_mask_gen_ = 0;
      u64 font_color_gen_ = 0;
      wgpu::Buffer readback_buf_;
      u64 readback_size_ = 0;
      u32 readback_padded_row_ = 0;
      std::vector<u8> pixels_;
    };
  } // namespace

  std::unique_ptr<offscreen_renderer> offscreen_renderer::create(const offscreen_options& options) {
    return std::make_unique<dawn_offscreen_renderer>(options);
  }

#endif // FXE_HAS_WGPU
} // namespace fxe
