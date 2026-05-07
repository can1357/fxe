// =============================================================================
// renderer_dawn.cpp — v1 Dawn/WebGPU backend for fxe::renderer.
//
// Implements coloured triangle/line topology, queued device draws with their own
// uniform blocks, and the reserved framebuffer texture tag used by blur geometry.
// The architectural shape mirrors gfw/d3d11.cpp: persistent device,
// dynamic vertex/index buffers grown on demand via queue.WriteBuffer, single
// UBO updated each frame, surface acquired via wgpu::Surface::Configure and
// GetCurrentTexture (modern post-1.0 API).
//
// Built against Dawn revision pinned to ~/dawn (May 2026 main).
// =============================================================================

#if !FXE_HAS_WGPU

// Intentionally empty. null_renderer in renderer_wgpu.cpp owns create_renderer.

#else

#include <fxe/types.hpp>
#include <webgpu/webgpu_cpp.h>

#include <fxe/font.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/window.hpp>

#include "pipeline.hpp"
#include <fxe/fxe_shaders.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fxe {
  namespace {
    // UBO must be aligned to 256 bytes for Dawn's uniform binding rules.
    constexpr u64 kUboBytes = (sizeof(vshader_cbuf) + 255ull) & ~static_cast<u64>(255ull);

    // 1 MiB initial dynamic vertex / index buffer; grown on demand.
    constexpr u64 kInitialDynamicBytes = 1u << 20;

    // ---------------------------------------------------------------------
    // Synchronous helpers: pump the Future-based async API to completion.
    // ---------------------------------------------------------------------
    template <typename Future, typename Instance> void wait_future(Instance& inst, Future fut) {
      // WaitAny with UINT64_MAX yields a synchronous-style block.
      wgpu::FutureWaitInfo info{fut};
      auto status = inst.WaitAny(1, &info, UINT64_MAX);
      if (status != wgpu::WaitStatus::Success) {
        throw std::runtime_error("wgpu::Instance::WaitAny failed");
      }
    }

    wgpu::Adapter request_adapter(wgpu::Instance& instance, const wgpu::Surface& surface) {
      wgpu::Adapter adapter;
      wgpu::RequestAdapterOptions opts{};
      opts.compatibleSurface = surface;
      opts.powerPreference = wgpu::PowerPreference::HighPerformance;

      auto fut = instance.RequestAdapter(
          &opts, wgpu::CallbackMode::WaitAnyOnly,
          [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
            if (status == wgpu::RequestAdapterStatus::Success) {
              adapter = std::move(a);
            } else {
              std::string m(msg.data, msg.length);
              std::fprintf(stderr, "fxe: RequestAdapter failed: %s\n", m.c_str());
            }
          });
      wait_future(instance, fut);
      if (!adapter)
        throw std::runtime_error("RequestAdapter returned null");
      return adapter;
    }

#ifndef NDEBUG
    std::atomic<unsigned>& debug_device_request_count() {
      static std::atomic<unsigned> count{0};
      return count;
    }
#endif

    wgpu::Device request_device(wgpu::Instance& instance, wgpu::Adapter& adapter) {
      wgpu::Device device;
      wgpu::DeviceDescriptor desc{};
      desc.label = "fxe-device";
      desc.SetUncapturedErrorCallback(
          [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
            std::string m(message.data, message.length);
            std::fprintf(stderr, "fxe: device error (type=%u): %s\n", static_cast<unsigned>(type),
                         m.c_str());
          });
#ifndef NDEBUG
      const unsigned request_count = debug_device_request_count().fetch_add(1) + 1;
      std::fprintf(stderr, "fxe.wgpu: RequestDevice count=%u\n", request_count);
#endif

      auto fut = adapter.RequestDevice(
          &desc, wgpu::CallbackMode::WaitAnyOnly,
          [&device](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
            if (status == wgpu::RequestDeviceStatus::Success) {
              device = std::move(d);
            } else {
              std::string m(msg.data, msg.length);
              std::fprintf(stderr, "fxe: RequestDevice failed: %s\n", m.c_str());
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

    class gpu_runtime final {
    public:
      static gpu_runtime& get() {
        static gpu_runtime runtime;
        return runtime;
      }

      wgpu::Instance& instance() {
        return instance_;
      }

      void ensure_device(const wgpu::Surface& surface) {
        std::call_once(init_once_, [this, &surface] {
          adapter_ = request_adapter(instance_, surface);
          device_ = request_device(instance_, adapter_);
          queue_ = device_.GetQueue();
        });

        if (!adapter_)
          throw std::runtime_error("gpu_runtime adapter is not initialized");
        if (!device_)
          throw std::runtime_error("gpu_runtime device is not initialized");
      }

      wgpu::Adapter& adapter() {
        return adapter_;
      }
      wgpu::Device& device() {
        return device_;
      }
      wgpu::Queue& queue() {
        return queue_;
      }

    private:
      gpu_runtime() {
        wgpu::InstanceDescriptor inst_desc{};
        // TimedWaitAny is required for the WaitAny-based sync helpers above.
        static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
        inst_desc.requiredFeatureCount = 1;
        inst_desc.requiredFeatures = &kTimedWaitAny;
        instance_ = wgpu::CreateInstance(&inst_desc);
        if (!instance_)
          throw std::runtime_error("wgpu::CreateInstance failed");
      }

      std::once_flag init_once_;
      wgpu::Instance instance_;
      wgpu::Adapter adapter_;
      wgpu::Device device_;
      wgpu::Queue queue_;
    };

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

    [[nodiscard]] primitive_effect classify_triangle(const std::vector<vertex>& vertices,
                                                     const u32* indices) noexcept {
      for (u32 i = 0; i != 3; ++i) {
        if (indices[i] < vertices.size() &&
            vertex_texture_id(vertices[indices[i]]) == framebuffer_texture_id) {
          return primitive_effect::framebuffer_sample;
        }
      }
      return primitive_effect::color;
    }

    // ---------------------------------------------------------------------
    // The renderer.
    // ---------------------------------------------------------------------
    class dawn_renderer final : public renderer {
    public:
      dawn_renderer(window& w, const renderer_options& opts) : win_(w) {
        auto& runtime = gpu_runtime::get();
        instance_ = runtime.instance();

        surface_ = make_wgpu_surface(w, instance_);
        if (!surface_)
          throw std::runtime_error("make_wgpu_surface returned null");

        runtime.ensure_device(surface_);
        adapter_ = runtime.adapter();
        device_ = runtime.device();
        queue_ = runtime.queue();

        // Pick a surface format that the adapter supports.
        wgpu::SurfaceCapabilities caps{};
        surface_.GetCapabilities(adapter_, &caps);
        surface_format_ = caps.formats[0];

        // Pick alpha mode. Transparent windows want Premultiplied; opaque
        // windows keep Auto.
        alpha_mode_ = wgpu::CompositeAlphaMode::Auto;
        if (w.is_transparent()) {
          bool found = false;
          for (size_t i = 0; i < caps.alphaModeCount; ++i) {
            if (caps.alphaModes[i] == wgpu::CompositeAlphaMode::Premultiplied) {
              alpha_mode_ = wgpu::CompositeAlphaMode::Premultiplied;
              found = true;
              break;
            }
          }
          if (!found) {
            std::fprintf(stderr, "fxe.wgpu: surface does not advertise Premultiplied alpha; "
                                 "transparency may not work\n");
          }
          // Default to fully transparent clear unless the user overrides.
          if (!clear_color_set_)
            clear_color_ = math::vec4{0.0f, 0.0f, 0.0f, 0.0f};
        }

        multisample_count_ =
            check_multisample_count(opts.multisample_count) ? opts.multisample_count : 1;
        bloom_enabled_ = opts.enable_bloom;
        present_mode_ = choose_present_mode(caps, opts.vsync);
        if (!opts.vsync && present_mode_ == wgpu::PresentMode::Fifo) {
          std::fprintf(stderr,
                       "fxe.wgpu: --no-vsync requested, but surface only supports FIFO present; "
                       "presentation remains display-paced\n");
        }

        build_resources();
        configure_surface(w.framebuffer_size().x, w.framebuffer_size().y);
      }

      ~dawn_renderer() override = default;

      void begin_frame(const math::vec3& eye_pos, const math::vec3& eye_dir,
                       const math::mat4x4& world_view_proj) override {
        sync_default_atlas();
        sync_font_atlases();
        {
          const auto fb_now = win_.framebuffer_size();
          const auto cs_now = win_.content_size();
          const float dpr =
              (cs_now.x > 0 && fb_now.x > 0) ? float(fb_now.x) / float(cs_now.x) : 1.0f;
          font::set_device_pixel_ratio(dpr);
        }
        auto fb = win_.framebuffer_size();
        if (fb.x != fb_w_ || fb.y != fb_h_) {
          configure_surface(fb.x, fb.y);
        }
        refresh_atlas_bind_groups_if_dirty();
        if (recreate_buffers_)
          refresh_pipelines();
        update_constants(eye_pos, eye_dir, world_view_proj, fb.x ? float(fb.x) : 1.0f,
                         fb.y ? float(fb.y) : 1.0f);
        queue_.WriteBuffer(ubo_, 0, &cbuf_, sizeof(cbuf_));
        clear();
        queued_dev_draws_.clear();
        queued_dev_prepared_ = false;
      }

      void end_frame() override {
        // 1. Acquire the next surface texture.
        wgpu::SurfaceTexture surf_tex{};
        surface_.GetCurrentTexture(&surf_tex);
        static int frame_dbg = 0;
        if (frame_dbg < 3) {
          std::fprintf(
              stderr, "fxe.frame[%d]: surf_status=%u verts=%llu tris=%u\n", frame_dbg,
              static_cast<unsigned>(surf_tex.status),
              static_cast<unsigned long long>(vertex_buffer.size()),
              static_cast<unsigned>(index_buffers[usize(vertex_topology::triangle)].size()));
          ++frame_dbg;
        }
        const bool tex_ok =
            surf_tex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            surf_tex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
        if (!tex_ok) {
          // Skip the frame; recreate the surface configuration on next paint.
          fb_w_ = fb_h_ = 0;
          return;
        }

        wgpu::TextureView view = surf_tex.texture.CreateView();
        sync_default_atlas();
        sync_font_atlases();
        refresh_atlas_bind_groups_if_dirty();
        prepare_queued_dev_draws();

        // 2. Push the dynamic vertex / index data, including queued device draws.
        flush_dynamic();

        // 3. Encode the render pass.
        wgpu::CommandEncoderDescriptor enc_desc{};
        enc_desc.label = "fxe-frame";
        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&enc_desc);

        const bool has_blur = queued_frame_has_blur();

        auto encode_draw_pass = [&](wgpu::LoadOp color_load, wgpu::LoadOp depth_load,
                                    blur_draw_mode mode, bool* blur_seen) {
          wgpu::RenderPassColorAttachment color{};
          color.view = color_target_view_ ? color_target_view_ : view;
          if (multisample_count_ > 1) {
            color.resolveTarget = capture_resolve_view_;
          }
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
          if (depth_view_) {
            pass_desc.depthStencilAttachment = &depth;
          }
          wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
          pass.SetBindGroup(0, bind_group_);
          if (vbuf_size_used_ > 0) {
            pass.SetVertexBuffer(0, vbuf_, 0, vbuf_size_used_);
            draw_indexed_ranges(pass, bind_group_, 0, main_tri_index_count_, 0,
                                main_line_index_count_, render_config{}, mode, blur_seen);

            for (auto& draw : queued_dev_draws_) {
              draw_indexed_ranges(pass, draw.bind_group, draw.tri_index_offset,
                                  draw.tri_index_count, draw.line_index_offset,
                                  draw.line_index_count, draw.cfg, mode, blur_seen);
            }
          }
          pass.End();
        };

        if (has_blur) {
          bool blur_seen = false;
          encode_draw_pass(wgpu::LoadOp::Clear, wgpu::LoadOp::Clear, blur_draw_mode::pre_capture,
                           &blur_seen);
          capture_frame_for_blur(encoder, surf_tex.texture);
          blur_seen = false;
          encode_draw_pass(wgpu::LoadOp::Load, wgpu::LoadOp::Load, blur_draw_mode::post_capture,
                           &blur_seen);
        } else {
          encode_draw_pass(wgpu::LoadOp::Clear, wgpu::LoadOp::Clear, blur_draw_mode::all, nullptr);
        }

        // When MSAA is on we resolved into capture_resolve_texture_; blit it
        // into the surface so the user actually sees their frame.
        if (multisample_count_ > 1 && capture_resolve_texture_) {
          wgpu::TexelCopyTextureInfo blit_src{};
          blit_src.texture = capture_resolve_texture_;
          blit_src.mipLevel = 0;
          blit_src.origin = {0, 0, 0};
          blit_src.aspect = wgpu::TextureAspect::All;
          wgpu::TexelCopyTextureInfo blit_dst{};
          blit_dst.texture = surf_tex.texture;
          blit_dst.mipLevel = 0;
          blit_dst.origin = {0, 0, 0};
          blit_dst.aspect = wgpu::TextureAspect::All;
          wgpu::Extent3D blit_extent{fb_w_, fb_h_, 1};
          encoder.CopyTextureToTexture(&blit_src, &blit_dst, &blit_extent);
        }

        // Capture path: encode CopyTextureToBuffer into the SAME command
        // encoder so the GPU executes copy after the render pass's resolve
        // and store ops are committed. Then a single Submit dispatches both.
        bool need_readback = capture_armed_.load(std::memory_order_acquire);
        u64 readback_size = 0;
        u32 readback_padded_row = 0;
        if (need_readback && fb_w_ > 0 && fb_h_ > 0) {
          const u32 row_bytes = fb_w_ * 4u;
          readback_padded_row = (row_bytes + 255u) & ~static_cast<u32>(255u);
          readback_size = u64(readback_padded_row) * fb_h_;
          if (capture_staging_size_ < readback_size) {
            wgpu::BufferDescriptor bd{};
            bd.label = "fxe-capture-staging";
            bd.size = readback_size;
            bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            capture_staging_buf_ = device_.CreateBuffer(&bd);
            capture_staging_size_ = readback_size;
          }
          capture_padded_row_ = readback_padded_row;
          wgpu::TexelCopyTextureInfo src{};
          src.texture = (multisample_count_ > 1) ? capture_resolve_texture_ : surf_tex.texture;
          src.mipLevel = 0;
          src.origin = {0, 0, 0};
          src.aspect = wgpu::TextureAspect::All;
          wgpu::TexelCopyBufferInfo dst{};
          dst.buffer = capture_staging_buf_;
          dst.layout.offset = 0;
          dst.layout.bytesPerRow = readback_padded_row;
          dst.layout.rowsPerImage = fb_h_;
          wgpu::Extent3D extent{fb_w_, fb_h_, 1};
          encoder.CopyTextureToBuffer(&src, &dst, &extent);
        }

        wgpu::CommandBufferDescriptor cb_desc{};
        wgpu::CommandBuffer cmds = encoder.Finish(&cb_desc);
        queue_.Submit(1, &cmds);

        if (need_readback && readback_size > 0) {
          finish_readback(readback_size, readback_padded_row);
        }

        captured_frame_available_ = false;
        surface_.Present();
        instance_.ProcessEvents();
      }

      bool queue_dev(const command_buffer& src, const vshader_cbuf& cbuf,
                     const render_config& cfg) override {
        queued_dev_draw draw{};
        draw.tri_index_count =
            static_cast<u32>(src.index_buffers[usize(vertex_topology::triangle)].size());
        draw.line_index_count =
            static_cast<u32>(src.index_buffers[usize(vertex_topology::line)].size());
        draw.cfg = cfg;
        draw.src = src;
        draw.ubo = create_uniform_buffer(cbuf, "fxe-queued-ubo");
        draw.bind_group = create_bind_group(draw.ubo, "fxe-queued-bg");
        queued_dev_draws_.push_back(std::move(draw));
        queued_dev_prepared_ = false;
        return true;
      }

      void stage_captured_frame() override {
        captured_frame_available_ = true;
      }

      // Page.screenshot path. Arms the per-frame readback the first time it's
      // called; subsequent calls return the most-recently-captured pixels as a
      // PNG. If no frame has been captured yet, the call returns ok=false with
      // a clear error and the window is poked so the next loop iteration
      // produces a frame — the protocol client should retry.
      capture_result capture_frame() override {
        capture_result r;
        bool was_armed = capture_armed_.exchange(true, std::memory_order_acq_rel);
        if (capture_pixels_.empty() || capture_pixels_w_ == 0 || capture_pixels_h_ == 0) {
          r.ok = false;
          r.error = was_armed ? "no frame captured yet (try again after the next render)"
                              : "capture armed; retry after the next render";
          // Nudge the window so the next iteration produces a frame.
          win_.post_redraw();
          return r;
        }
        r.ok = true;
        r.width = capture_pixels_w_;
        r.height = capture_pixels_h_;
        // capture_pixels_ is already tightly packed (row stride = width*4).
        r.rgba = capture_pixels_;
        return r;
      }

    private:
      // Called from end_frame() after Submit. The CopyTextureToBuffer was
      // already encoded into the same command buffer; this just maps the
      // staging buffer (synchronous WaitAny on the future) and copies/swizzles
      // the bytes into capture_pixels_.
      void finish_readback(u64 needed, u32 padded_row) {
        struct map_state {
          bool ok = false;
        } state;
        auto fut = capture_staging_buf_.MapAsync(
            wgpu::MapMode::Read, 0, needed, wgpu::CallbackMode::WaitAnyOnly,
            [&state](wgpu::MapAsyncStatus status, wgpu::StringView /*msg*/) {
              state.ok = (status == wgpu::MapAsyncStatus::Success);
            });
        wgpu::FutureWaitInfo info{fut};
        instance_.WaitAny(1, &info, UINT64_MAX);
        if (!state.ok) {
          std::fprintf(stderr, "fxe.capture: MapAsync failed\n");
          return;
        }
        const auto* mapped =
            static_cast<const u8*>(capture_staging_buf_.GetConstMappedRange(0, needed));
        if (!mapped) {
          std::fprintf(stderr, "fxe.capture: GetConstMappedRange returned null\n");
          capture_staging_buf_.Unmap();
          return;
        }
        const u32 row_bytes = fb_w_ * 4u;
        capture_pixels_.assign(usize(row_bytes) * fb_h_, 0);
        const bool is_bgra = (surface_format_ == wgpu::TextureFormat::BGRA8Unorm ||
                              surface_format_ == wgpu::TextureFormat::BGRA8UnormSrgb);
        for (u32 y = 0; y < fb_h_; ++y) {
          const u8* in = mapped + usize(y) * padded_row;
          u8* out = capture_pixels_.data() + usize(y) * row_bytes;
          if (is_bgra) {
            for (u32 x = 0; x < fb_w_; ++x) {
              out[x * 4 + 0] = in[x * 4 + 2]; // R
              out[x * 4 + 1] = in[x * 4 + 1]; // G
              out[x * 4 + 2] = in[x * 4 + 0]; // B
              out[x * 4 + 3] = in[x * 4 + 3]; // A
            }
          } else {
            std::memcpy(out, in, row_bytes);
          }
        }
        capture_pixels_w_ = fb_w_;
        capture_pixels_h_ = fb_h_;
        ++capture_frame_seq_;
        capture_staging_buf_.Unmap();
      }

    public:
      void set_atlas(u32 w, u32 h, const u8* rgba) override {
        if (!w || !h || !rgba) {
          create_default_atlas();
          return;
        }
        upload_atlas_pixels(w, h, rgba);
      }

      window& get_window() override {
        return win_;
      }
      const window& get_window() const override {
        return win_;
      }

    private:
      // Build all device-lifetime resources: pipeline, bind group, buffers.
      void build_resources() {
        // Shader module.
        wgpu::ShaderSourceWGSL wgsl{};
        std::string code(reinterpret_cast<const char*>(shaders::main_wgsl),
                         shaders::main_wgsl_size);
        wgsl.code = code.c_str();
        wgpu::ShaderModuleDescriptor sm_desc{};
        sm_desc.nextInChain = &wgsl;
        sm_desc.label = "fxe-main.wgsl";
        shader_ = device_.CreateShaderModule(&sm_desc);

        // UBO.
        {
          wgpu::BufferDescriptor desc{};
          desc.label = "fxe-ubo";
          desc.size = kUboBytes;
          desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
          ubo_ = device_.CreateBuffer(&desc);
        }

        // Bind group layout matches main.wgsl:
        //   @binding(0) UBO (Constants)
        //   @binding(1) sampler        (atlas_sampler)
        //   @binding(2) texture_2d<f32>(atlas_tex)        — sprite/legacy
        //   @binding(3) texture_2d<f32>(font_mask_tex)    — font alpha mask
        //   @binding(4) texture_2d<f32>(font_color_tex)   — font color emoji
        //   @binding(5) sampler        (framebuffer_sampler)
        //   @binding(6) texture_2d<f32>(framebuffer_texture) — captured frame
        std::array<wgpu::BindGroupLayoutEntry, 8> bgl_entries{};
        bgl_entries[0].binding = 0;
        bgl_entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        bgl_entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        bgl_entries[0].buffer.minBindingSize = 0;
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
        // Nearest sampler dedicated to the font mask page. Glyphs are
        // already rasterised at framebuffer resolution; bilinear filtering
        // would only soften them.
        bgl_entries[7].binding = 7;
        bgl_entries[7].visibility = wgpu::ShaderStage::Fragment;
        bgl_entries[7].sampler.type = wgpu::SamplerBindingType::NonFiltering;
        wgpu::BindGroupLayoutDescriptor bgl_desc{};
        bgl_desc.label = "fxe-bgl";
        bgl_desc.entryCount = bgl_entries.size();
        bgl_desc.entries = bgl_entries.data();
        bgl_ = device_.CreateBindGroupLayout(&bgl_desc);

        // Linear sampler for glyph atlas sampling. clamp on edges so the
        // atlas's adjacent glyphs don't bleed across UV-overflow.
        wgpu::SamplerDescriptor sampler_desc{};
        sampler_desc.label = "fxe-atlas-sampler";
        sampler_desc.minFilter = wgpu::FilterMode::Linear;
        sampler_desc.magFilter = wgpu::FilterMode::Linear;
        sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        sampler_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
        sampler_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
        atlas_sampler_ = device_.CreateSampler(&sampler_desc);

        // Nearest sampler used by the font mask path.
        wgpu::SamplerDescriptor mask_desc{};
        mask_desc.label = "fxe-mask-sampler";
        mask_desc.minFilter = wgpu::FilterMode::Nearest;
        mask_desc.magFilter = wgpu::FilterMode::Nearest;
        mask_desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        mask_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
        mask_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
        mask_sampler_ = device_.CreateSampler(&mask_desc);

        // Default 1x1 white atlas. Lets primitives that DO route through the
        // atlas branch (uv.z != 0) but have no real font yet still produce
        // legible output (alpha=255 white).
        create_default_atlas();

        bind_group_ = create_bind_group(ubo_, "fxe-bg");

        // Pipeline layout.
        wgpu::PipelineLayoutDescriptor pl_desc{};
        pl_desc.label = "fxe-plo";
        pl_desc.bindGroupLayoutCount = 1;
        pl_desc.bindGroupLayouts = &bgl_;
        pipeline_layout_ = device_.CreatePipelineLayout(&pl_desc);

        pipeline_cache_.clear();
        refresh_pipelines();

        // Initial dynamic vertex / index buffers.
        ensure_buffer(vbuf_, vbuf_capacity_, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                      kInitialDynamicBytes, "fxe-vbuf");
        ensure_buffer(tri_ibuf_, tri_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, kInitialDynamicBytes,
                      "fxe-tri-ibuf");
        ensure_buffer(line_ibuf_, line_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, kInitialDynamicBytes,
                      "fxe-line-ibuf");
      }

      void refresh_pipelines() {
        pipeline_key key{};
        key.vs_entry = "vs_transform";
        key.fs_entry = "ps_opaque";
        key.color_format = surface_format_;
        key.depth_format = depth_format_;
        key.blend = current_blend_mode();
        key.topology = wgpu::PrimitiveTopology::TriangleList;
        key.sample_count = multisample_count_;
        triangle_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.topology = wgpu::PrimitiveTopology::LineList;
        line_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);

        key.vs_entry = "vs_transform_uv";
        key.topology = wgpu::PrimitiveTopology::TriangleList;
        key.fs_entry = "ps_sample";
        sample_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_vblur";
        vblur_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_hblur";
        hblur_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.fs_entry = "ps_lum_filter";
        lum_filter_pipeline_ = pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);

        recreate_buffers_ = false;
      }

      wgpu::Buffer create_uniform_buffer(const vshader_cbuf& cbuf, const char* label) {
        wgpu::BufferDescriptor desc{};
        desc.label = label;
        desc.size = kUboBytes;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        wgpu::Buffer buf = device_.CreateBuffer(&desc);
        queue_.WriteBuffer(buf, 0, &cbuf, sizeof(cbuf));
        return buf;
      }

      wgpu::BindGroup create_bind_group(const wgpu::Buffer& ubo, const char* label) {
        std::array<wgpu::BindGroupEntry, 8> entries{};
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
        entries[6].textureView = blur_capture_view_ ? blur_capture_view_ : atlas_view_;
        entries[7].binding = 7;
        entries[7].sampler = mask_sampler_;
        wgpu::BindGroupDescriptor bg_desc{};
        bg_desc.label = label;
        bg_desc.layout = bgl_;
        bg_desc.entryCount = entries.size();
        bg_desc.entries = entries.data();
        return device_.CreateBindGroup(&bg_desc);
      }

      // Build a 1x1 white atlas (RGBA = 255,255,255,255). Used until the host
      // pushes a real font/sprite atlas via set_atlas().
      void create_default_atlas() {
        const u8 white[4] = {255, 255, 255, 255};
        upload_atlas_pixels(1, 1, white);
      }

      // Replace the atlas texture + view with a freshly-uploaded RGBA8 image.
      // After upload the bind groups must be rebuilt so they point at the new
      // view; refresh_atlas_bind_groups_if_dirty() does that before drawing.
      void upload_atlas_pixels(u32 w, u32 h, const u8* rgba) {
        wgpu::TextureDescriptor td{};
        td.label = "fxe-atlas";
        td.dimension = wgpu::TextureDimension::e2D;
        td.size = {w, h, 1};
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        atlas_texture_ = device_.CreateTexture(&td);
        atlas_view_ = atlas_texture_.CreateView();
        atlas_w_ = w;
        atlas_h_ = h;

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
        queue_.WriteTexture(&dst, rgba, usize(w) * h * 4, &layout, &extent);
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

      // Upload the font module's mask + color atlas pages whenever their
      // generation counter advances. The pages are growable; we re-create
      // the wgpu::Texture if the size changed and queue.WriteTexture
      // otherwise.
      void sync_font_atlases() {
        auto& gc = font::shared_glyph_cache();
        sync_one_font_atlas(gc.mask_atlas(), font_mask_texture_, font_mask_view_, font_mask_w_,
                            font_mask_h_, font_mask_gen_, wgpu::TextureFormat::R8Unorm,
                            "fxe-font-mask");
        sync_one_font_atlas(gc.color_atlas(), font_color_texture_, font_color_view_, font_color_w_,
                            font_color_h_, font_color_gen_, wgpu::TextureFormat::BGRA8Unorm,
                            "fxe-font-color");
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

      void refresh_atlas_bind_groups_if_dirty() {
        if (!atlas_dirty_)
          return;
        bind_group_ = create_bind_group(ubo_, "fxe-bg");
        for (auto& draw : queued_dev_draws_)
          draw.bind_group = create_bind_group(draw.ubo, "fxe-queued-bg");
        atlas_dirty_ = false;
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

      void prepare_queued_dev_draws() {
        if (queued_dev_prepared_)
          return;
        main_tri_index_count_ =
            static_cast<u32>(index_buffers[usize(vertex_topology::triangle)].size());
        main_line_index_count_ =
            static_cast<u32>(index_buffers[usize(vertex_topology::line)].size());

        for (auto& draw : queued_dev_draws_) {
          draw.tri_index_offset =
              index_buffers[usize(vertex_topology::triangle)].size() * sizeof(u32);
          draw.line_index_offset = index_buffers[usize(vertex_topology::line)].size() * sizeof(u32);
          queue(draw.src);
        }
        queued_dev_prepared_ = true;
      }
      void flush_dynamic() {
        const u64 vbytes = vertex_buffer.size() * sizeof(vertex);
        const u64 tri_idx_bytes =
            index_buffers[usize(vertex_topology::triangle)].size() * sizeof(u32);
        const u64 line_idx_bytes = index_buffers[usize(vertex_topology::line)].size() * sizeof(u32);

        ensure_buffer(vbuf_, vbuf_capacity_, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                      vbytes ? vbytes : kInitialDynamicBytes, "fxe-vbuf");
        ensure_buffer(tri_ibuf_, tri_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
                      tri_idx_bytes ? tri_idx_bytes : kInitialDynamicBytes, "fxe-tri-ibuf");
        ensure_buffer(line_ibuf_, line_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
                      line_idx_bytes ? line_idx_bytes : kInitialDynamicBytes, "fxe-line-ibuf");

        if (vbytes) {
          queue_.WriteBuffer(vbuf_, 0, vertex_buffer.data(), vbytes);
        }
        if (tri_idx_bytes) {
          queue_.WriteBuffer(tri_ibuf_, 0, index_buffers[usize(vertex_topology::triangle)].data(),
                             tri_idx_bytes);
        }
        if (line_idx_bytes) {
          queue_.WriteBuffer(line_ibuf_, 0, index_buffers[usize(vertex_topology::line)].data(),
                             line_idx_bytes);
        }
        vbuf_size_used_ = vbytes;
      }

      void configure_surface(u32 w, u32 h) {
        if (w == 0 || h == 0)
          return;
        wgpu::SurfaceConfiguration cfg{};
        cfg.device = device_;
        cfg.format = surface_format_;
        cfg.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                    wgpu::TextureUsage::CopyDst;
        cfg.width = w;
        cfg.height = h;
        cfg.presentMode = present_mode_;
        cfg.alphaMode = alpha_mode_;
        surface_.Configure(&cfg);

        depth_view_ = {};
        destroy_texture(depth_texture_);
        color_target_view_ = {};
        destroy_texture(color_target_texture_);

        wgpu::TextureDescriptor depth_desc{};
        depth_desc.label = "fxe-depth";
        depth_desc.dimension = wgpu::TextureDimension::e2D;
        depth_desc.size = {w, h, 1};
        depth_desc.format = depth_format_;
        depth_desc.usage = wgpu::TextureUsage::RenderAttachment;
        depth_desc.sampleCount = multisample_count_;
        depth_texture_ = device_.CreateTexture(&depth_desc);
        depth_view_ = depth_texture_.CreateView();

        if (multisample_count_ > 1) {
          wgpu::TextureDescriptor color_desc{};
          color_desc.label = "fxe-msaa-color";
          color_desc.dimension = wgpu::TextureDimension::e2D;
          color_desc.size = {w, h, 1};
          color_desc.format = surface_format_;
          color_desc.usage = wgpu::TextureUsage::RenderAttachment;
          color_desc.sampleCount = multisample_count_;
          color_target_texture_ = device_.CreateTexture(&color_desc);
          color_target_view_ = color_target_texture_.CreateView();
        }

        // MSAA resolve proxy: on Metal the actual surface drawable can't be
        // CopySrc-readable, so when MSAA>1 we resolve into our own texture
        // (RenderAttachment | CopySrc | CopyDst) and then CopyTextureToTexture
        // it into the surface in end_frame. capture_frame() reads from this
        // proxy. For MSAA=1 we still write directly to the surface (whose
        // CopySrc usage Dawn's Metal backend honors via an internal shadow).
        capture_resolve_view_ = {};
        destroy_texture(capture_resolve_texture_);
        if (multisample_count_ > 1) {
          wgpu::TextureDescriptor rd{};
          rd.label = "fxe-capture-resolve";
          rd.dimension = wgpu::TextureDimension::e2D;
          rd.size = {w, h, 1};
          rd.format = surface_format_;
          rd.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                     wgpu::TextureUsage::CopyDst;
          rd.sampleCount = 1;
          capture_resolve_texture_ = device_.CreateTexture(&rd);
          capture_resolve_view_ = capture_resolve_texture_.CreateView();
        }

        blur_capture_view_ = {};
        destroy_texture(blur_capture_texture_);
        wgpu::TextureDescriptor blur_desc{};
        blur_desc.label = "fxe-blur-capture";
        blur_desc.dimension = wgpu::TextureDimension::e2D;
        blur_desc.size = {w, h, 1};
        blur_desc.format = surface_format_;
        blur_desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst |
                          wgpu::TextureUsage::CopySrc;
        blur_desc.sampleCount = 1;
        blur_capture_texture_ = device_.CreateTexture(&blur_desc);
        blur_capture_view_ = blur_capture_texture_.CreateView();
        if (bgl_) {
          bind_group_ = create_bind_group(ubo_, "fxe-bg");
          for (auto& draw : queued_dev_draws_)
            draw.bind_group = create_bind_group(draw.ubo, "fxe-queued-bg");
        }

        fb_w_ = w;
        fb_h_ = h;
      }

      enum class blur_draw_mode { all, pre_capture, post_capture };

      [[nodiscard]] bool range_has_framebuffer_samples(u64 tri_offset, u32 tri_count) const {
        const auto& tri = index_buffers[usize(vertex_topology::triangle)];
        const usize begin = static_cast<usize>(tri_offset / sizeof(u32));
        const usize end = std::min<usize>(begin + tri_count, tri.size());
        for (usize i = begin; i + 2 < end; i += 3) {
          if (classify_triangle(vertex_buffer, &tri[i]) == primitive_effect::framebuffer_sample)
            return true;
        }
        return false;
      }

      [[nodiscard]] wgpu::RenderPipeline pipeline_for_effect(primitive_effect effect) const {
        switch (effect) {
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

      void draw_triangle_batch(wgpu::RenderPassEncoder& pass, primitive_effect effect,
                               u64 base_offset, u32 first, u32 count) {
        if (count == 0)
          return;
        pass.SetPipeline(pipeline_for_effect(effect));
        pass.SetIndexBuffer(tri_ibuf_, wgpu::IndexFormat::Uint32,
                            base_offset + u64(first) * sizeof(u32), count * sizeof(u32));
        pass.DrawIndexed(count, 1, 0, 0, 0);
      }

      void draw_triangles_for_mode(wgpu::RenderPassEncoder& pass, u64 tri_offset, u32 tri_count,
                                   blur_draw_mode mode, bool& blur_seen) {
        const auto& tri = index_buffers[usize(vertex_topology::triangle)];
        const usize begin = static_cast<usize>(tri_offset / sizeof(u32));
        const usize end = std::min<usize>(begin + tri_count, tri.size());
        u32 batch_first = 0;
        u32 batch_count = 0;
        primitive_effect batch_effect = primitive_effect::color;
        auto flush = [&] {
          draw_triangle_batch(pass, batch_effect, tri_offset, batch_first, batch_count);
          batch_count = 0;
        };
        for (usize i = begin; i + 2 < end; i += 3) {
          primitive_effect effect = classify_triangle(vertex_buffer, &tri[i]);
          if (mode == blur_draw_mode::pre_capture &&
              effect == primitive_effect::framebuffer_sample) {
            flush();
            blur_seen = true;
            return;
          }
          if (mode == blur_draw_mode::post_capture && !blur_seen) {
            if (effect != primitive_effect::framebuffer_sample)
              continue;
            blur_seen = true;
          }
          const u32 rel = static_cast<u32>(i - begin);
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

      void draw_indexed_ranges(wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bind_group,
                               u64 tri_offset, u32 tri_count, u64 line_offset, u32 line_count,
                               const render_config& cfg, blur_draw_mode mode = blur_draw_mode::all,
                               bool* blur_seen_arg = nullptr) {
        pass.SetBindGroup(0, bind_group);
        if (cfg.viewport.size.x > 0.0f && cfg.viewport.size.y > 0.0f) {
          pass.SetViewport(cfg.viewport.at.x, cfg.viewport.at.y, cfg.viewport.size.x,
                           cfg.viewport.size.y, cfg.viewport.depth_range.x,
                           cfg.viewport.depth_range.y);
        }
        if (cfg.scissor.end.x > cfg.scissor.begin.x && cfg.scissor.end.y > cfg.scissor.begin.y) {
          pass.SetScissorRect(static_cast<u32>(cfg.scissor.begin.x),
                              static_cast<u32>(cfg.scissor.begin.y),
                              static_cast<u32>(cfg.scissor.end.x - cfg.scissor.begin.x),
                              static_cast<u32>(cfg.scissor.end.y - cfg.scissor.begin.y));
        }
        bool local_blur_seen = mode != blur_draw_mode::post_capture;
        bool& blur_seen = blur_seen_arg ? *blur_seen_arg : local_blur_seen;
        if (mode == blur_draw_mode::pre_capture && blur_seen)
          return;
        if (tri_count > 0)
          draw_triangles_for_mode(pass, tri_offset, tri_count, mode, blur_seen);
        if (line_count > 0 && (mode != blur_draw_mode::pre_capture || !blur_seen)) {
          pass.SetPipeline(line_pipeline_);
          pass.SetIndexBuffer(line_ibuf_, wgpu::IndexFormat::Uint32, line_offset,
                              line_count * sizeof(u32));
          pass.DrawIndexed(line_count, 1, 0, 0, 0);
        }
      }

      [[nodiscard]] bool queued_frame_has_blur() const {
        if (range_has_framebuffer_samples(0, main_tri_index_count_))
          return true;
        for (const auto& draw : queued_dev_draws_) {
          if (range_has_framebuffer_samples(draw.tri_index_offset, draw.tri_index_count))
            return true;
        }
        return false;
      }

      static wgpu::PresentMode choose_present_mode(const wgpu::SurfaceCapabilities& caps,
                                                   bool vsync) noexcept {
        auto supports = [&](wgpu::PresentMode mode) noexcept {
          for (size_t i = 0; i < caps.presentModeCount; ++i) {
            if (caps.presentModes[i] == mode)
              return true;
          }
          return false;
        };
        if (vsync)
          return supports(wgpu::PresentMode::Fifo) ? wgpu::PresentMode::Fifo
                                                   : wgpu::PresentMode::Undefined;
        if (supports(wgpu::PresentMode::Immediate))
          return wgpu::PresentMode::Immediate;
        if (supports(wgpu::PresentMode::Mailbox))
          return wgpu::PresentMode::Mailbox;
        if (supports(wgpu::PresentMode::FifoRelaxed))
          return wgpu::PresentMode::FifoRelaxed;
        return supports(wgpu::PresentMode::Fifo) ? wgpu::PresentMode::Fifo
                                                 : wgpu::PresentMode::Undefined;
      }

      void capture_frame_for_blur(wgpu::CommandEncoder& encoder,
                                  const wgpu::Texture& surface_texture) {
        if (!blur_capture_texture_)
          return;
        wgpu::TexelCopyTextureInfo src{};
        src.texture = (multisample_count_ > 1) ? capture_resolve_texture_ : surface_texture;
        src.mipLevel = 0;
        src.origin = {0, 0, 0};
        src.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = blur_capture_texture_;
        dst.mipLevel = 0;
        dst.origin = {0, 0, 0};
        dst.aspect = wgpu::TextureAspect::All;
        wgpu::Extent3D extent{fb_w_, fb_h_, 1};
        encoder.CopyTextureToTexture(&src, &dst, &extent);
      }

      window& win_;
      wgpu::Instance instance_;
      wgpu::Surface surface_;
      wgpu::Adapter adapter_;
      wgpu::Device device_;
      wgpu::Queue queue_;
      wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::BGRA8Unorm;
      wgpu::TextureFormat depth_format_ = wgpu::TextureFormat::Depth24Plus;
      wgpu::CompositeAlphaMode alpha_mode_ = wgpu::CompositeAlphaMode::Auto;
      wgpu::PresentMode present_mode_ = wgpu::PresentMode::Fifo;

      wgpu::Buffer ubo_;
      wgpu::Buffer vbuf_;
      wgpu::Buffer tri_ibuf_;
      wgpu::Buffer line_ibuf_;
      u64 vbuf_capacity_ = 0;
      u64 tri_ibuf_capacity_ = 0;
      u64 line_ibuf_capacity_ = 0;
      u64 vbuf_size_used_ = 0;
      u32 main_tri_index_count_ = 0;
      u32 main_line_index_count_ = 0;

      struct queued_dev_draw {
        u64 tri_index_offset = 0;
        u32 tri_index_count = 0;
        u64 line_index_offset = 0;
        u32 line_index_count = 0;
        render_config cfg{};
        command_buffer src{};
        wgpu::Buffer ubo;
        wgpu::BindGroup bind_group;
      };
      std::vector<queued_dev_draw> queued_dev_draws_;

      wgpu::BindGroupLayout bgl_;
      wgpu::BindGroup bind_group_;
      wgpu::PipelineLayout pipeline_layout_;
      wgpu::ShaderModule shader_;
      wgpu::RenderPipeline triangle_pipeline_;
      wgpu::RenderPipeline line_pipeline_;
      pipeline_cache pipeline_cache_;
      wgpu::RenderPipeline sample_pipeline_;
      wgpu::RenderPipeline vblur_pipeline_;
      wgpu::RenderPipeline hblur_pipeline_;
      wgpu::RenderPipeline lum_filter_pipeline_;
      wgpu::Texture depth_texture_;
      wgpu::TextureView depth_view_;
      wgpu::Texture color_target_texture_;
      wgpu::TextureView color_target_view_;
      // See configure_surface() for why this exists. Single-sample, BGRA, MSAA-only.
      wgpu::Texture capture_resolve_texture_;
      wgpu::TextureView capture_resolve_view_;
      wgpu::Texture blur_capture_texture_;
      wgpu::TextureView blur_capture_view_;
      wgpu::Texture atlas_texture_;
      wgpu::TextureView atlas_view_;
      wgpu::Sampler atlas_sampler_;
      wgpu::Sampler mask_sampler_;
      u32 atlas_w_ = 0;
      u32 atlas_h_ = 0;
      bool atlas_dirty_ = false;
      texture_id synced_default_atlas_id_ = null_texture;
      u32 synced_default_atlas_w_ = 0;
      u32 synced_default_atlas_h_ = 0;
      u64 synced_default_atlas_hash_ = 0;
      // Font module's mask + color atlases. Synced from font::GlyphCache
      // each frame; bumped to GPU when the cache's `generation()` advances.
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

      // Capture/screenshot state. capture_armed_ is set by capture_frame()
      // and remains true; end_frame() copies the surface texture into
      // capture_staging_buf_ each frame and swizzles into capture_pixels_.
      // capture_pixels_w_/h_ track the layout of the cached bytes (may differ
      // from current fb if a resize happened mid-capture).
      std::atomic<bool> capture_armed_{false};
      wgpu::Buffer capture_staging_buf_;
      u64 capture_staging_size_ = 0;
      u32 capture_padded_row_ = 0;
      std::vector<u8> capture_pixels_;
      u32 capture_pixels_w_ = 0;
      u32 capture_pixels_h_ = 0;
      u64 capture_frame_seq_ = 0;

      u32 fb_w_ = 0;
      u32 fb_h_ = 0;
      bool captured_frame_available_ = false;
      bool queued_dev_prepared_ = false;
    };
  } // namespace

  std::unique_ptr<renderer> create_renderer(window& w, const renderer_options& opts) {
    return std::make_unique<dawn_renderer>(w, opts);
  }
} // namespace fxe

#endif // FXE_HAS_WGPU
