#include <fxe/types.hpp>
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

#include <webgpu/webgpu_cpp.h>

#include <fxe/font.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/texture_registry.hpp>
#include <fxe/window.hpp>

#include "pipeline.hpp"
#include <fxe/fxe_shaders.hpp>
#include <fxe/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fxe {
  namespace {

    // UBO must be aligned to 256 bytes for Dawn's uniform binding rules.
    constexpr u64 kUboBytes = (sizeof(vshader_cbuf) + 255ull) & ~static_cast<u64>(255ull);

    // 1 MiB initial dynamic vertex / index buffer; grown on demand.
    constexpr u64 kInitialDynamicBytes = 1u << 20;

    constexpr float kBlurKernelRadiusPx = 3.25f;

    [[nodiscard]] u32 blur_pass_count_for_radius(float radius_px) noexcept {
      if (!(radius_px > 0.0f))
        return 0;
      const float passes = std::ceil(radius_px / kBlurKernelRadiusPx);
      return std::max<u32>(1u, static_cast<u32>(passes));
    }

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
              FXE_ERROR("wgpu.renderer", "RequestAdapter failed: {}", m);
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
      desc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type,
                                         wgpu::StringView message) {
        std::string m(message.data, message.length);
        FXE_ERROR("wgpu.renderer", "device error (type={}): {}", static_cast<unsigned>(type), m);
      });
#ifndef NDEBUG
      const unsigned request_count = debug_device_request_count().fetch_add(1) + 1;
      FXE_DEBUG("wgpu.renderer", "RequestDevice count={}", request_count);
#endif

      auto fut = adapter.RequestDevice(
          &desc, wgpu::CallbackMode::WaitAnyOnly,
          [&device](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
            if (status == wgpu::RequestDeviceStatus::Success) {
              device = std::move(d);
            } else {
              std::string m(msg.data, msg.length);
              FXE_ERROR("wgpu.renderer", "RequestDevice failed: {}", m);
            }
          });
      wait_future(instance, fut);
      if (!device)
        throw std::runtime_error("RequestDevice returned null");
      return device;
    }
    void destroy_texture(wgpu::Texture& texture);

    [[nodiscard]] bool probe_render_attachment_sample_count(const wgpu::Device& device,
                                                            wgpu::TextureFormat color_format,
                                                            wgpu::TextureFormat depth_format,
                                                            u32 sample_count) {
      if (sample_count <= 1)
        return true;
      auto instance = device.GetAdapter().GetInstance();
      if (!instance)
        return false;

      wgpu::TextureDescriptor color_desc{};
      color_desc.label = "fxe-renderer-msaa-probe-color";
      color_desc.dimension = wgpu::TextureDimension::e2D;
      color_desc.size = {4, 4, 1};
      color_desc.format = color_format;
      color_desc.mipLevelCount = 1;
      color_desc.sampleCount = sample_count;
      color_desc.usage = wgpu::TextureUsage::RenderAttachment;

      device.PushErrorScope(wgpu::ErrorFilter::Validation);
      auto color = device.CreateTexture(&color_desc);
      wgpu::Texture depth;
      if (depth_format != wgpu::TextureFormat::Undefined) {
        wgpu::TextureDescriptor depth_desc = color_desc;
        depth_desc.label = "fxe-renderer-msaa-probe-depth";
        depth_desc.format = depth_format;
        depth = device.CreateTexture(&depth_desc);
      }

      wgpu::PopErrorScopeStatus pop_status = wgpu::PopErrorScopeStatus::Error;
      wgpu::ErrorType error_type = wgpu::ErrorType::NoError;
      auto fut = device.PopErrorScope(
          wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
            pop_status = status;
            error_type = type;
          });
      wait_future(instance, fut);

      destroy_texture(depth);
      destroy_texture(color);
      return pop_status == wgpu::PopErrorScopeStatus::Success &&
             error_type == wgpu::ErrorType::NoError;
    }

    [[nodiscard]] std::vector<u32>
    probe_supported_multisample_counts(const wgpu::Device& device, wgpu::TextureFormat color_format,
                                       wgpu::TextureFormat depth_format) {
      std::vector<u32> supported;
      supported.push_back(1);
      for (u32 sample_count : std::array<u32, 4>{2, 4, 8, 16}) {
        if (probe_render_attachment_sample_count(device, color_format, depth_format, sample_count))
          supported.push_back(sample_count);
      }
      return supported;
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

    constexpr texture_id kFontColorFlag = font_color_flag;
    constexpr texture_id kFontMaskFlag = font_mask_flag;
    constexpr texture_id kExternalTextureFlag = external_texture_flag;

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
        if ((tx & kExternalTextureFlag) != 0u)
          alpha_effect = primitive_effect::alpha_blend;
        if (v.color.a < 255)
          alpha_effect = primitive_effect::alpha_blend;
      }
      return alpha_effect;
    }

    [[nodiscard]] texture_id triangle_external_texture_id(const std::vector<vertex>& vertices,
                                                          const u32* indices) noexcept {
      texture_id external = null_texture;
      for (u32 i = 0; i != 3; ++i) {
        if (indices[i] >= vertices.size())
          continue;
        const texture_id tx = vertex_texture_id(vertices[indices[i]]);
        if ((tx & kExternalTextureFlag) == 0u)
          continue;
        if (external == null_texture) {
          external = tx;
        } else if (external != tx) {
          return null_texture;
        }
      }
      return external;
    }

    // ---------------------------------------------------------------------
    // The renderer.
    // ---------------------------------------------------------------------
    class dawn_renderer final : public renderer, public dawn_pipeline_device_access {
    private:
      enum class pending_capture { idle, copy_encoded, map_pending, ready, failed };

    public:
      using renderer::queue;

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
          for (usize i = 0; i < caps.alphaModeCount; ++i) {
            if (caps.alphaModes[i] == wgpu::CompositeAlphaMode::Premultiplied) {
              alpha_mode_ = wgpu::CompositeAlphaMode::Premultiplied;
              found = true;
              break;
            }
          }
          if (!found) {
            FXE_WARN("wgpu.renderer",
                     "surface does not advertise Premultiplied alpha; transparency may not work");
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
          FXE_WARN("wgpu.renderer", "--no-vsync requested, but surface only supports FIFO present; "
                                    "falling back to platform vsync override (e.g. macOS "
                                    "CAMetalLayer.displaySyncEnabled)");
        }
        // Even when Dawn maps to Fifo, the platform window may have a direct
        // way to disable display-paced presentation (e.g. macOS Metal layer
        // displaySyncEnabled). configure_surface() re-applies want_vsync_
        // after every Surface.Configure (which itself resets the layer state).
        want_vsync_ = opts.vsync;

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
        {
          auto& gc = font::shared_glyph_cache();
          FXE_TRACE("wgpu.renderer", "frame_begin mask_gen={} color_gen={} evicted={}/{}",
                    gc.generation(font::Format::grayscale), gc.generation(font::Format::bgra),
                    gc.eviction_count(font::Format::grayscale),
                    gc.eviction_count(font::Format::bgra));
        }
        queue_.WriteBuffer(ubo_, 0, &cbuf_, sizeof(cbuf_));
        clear();
        queued_dev_draws_.clear();
        queued_custom_draws_.clear();
        queued_dev_prepared_ = false;
      }

      void end_frame() override {
        {
          auto& gc = font::shared_glyph_cache();
          FXE_TRACE("wgpu.renderer", "frame_end mask_gen={} color_gen={}",
                    gc.generation(font::Format::grayscale), gc.generation(font::Format::bgra));
        }
        instance_.ProcessEvents();
        // 1. Acquire the next surface texture. On macOS Dawn may reset
        // CAMetalLayer.displaySyncEnabled while acquiring a FIFO drawable, so
        // the platform vsync override is re-applied immediately after
        // GetCurrentTexture rather than before it.
        wgpu::SurfaceTexture surf_tex{};
        surface_.GetCurrentTexture(&surf_tex);
        win_.set_vsync(want_vsync_);
        const bool tex_ok =
            surf_tex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            surf_tex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
        if (!tex_ok) {
          // Skip the frame; recreate the surface configuration on next paint.
          fb_w_ = fb_h_ = 0;
          instance_.ProcessEvents();
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
                                    blur_draw_mode mode,
                                    const wgpu::TextureView& framebuffer_view = {}) {
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
          const wgpu::BindGroup frame_bind_group =
              framebuffer_view ? create_bind_group(ubo_, "fxe-bg-framebuffer", framebuffer_view)
                               : bind_group_;
          pass.SetBindGroup(0, frame_bind_group);
          if (vbuf_size_used_ > 0) {
            pass.SetVertexBuffer(0, vbuf_, 0, vbuf_size_used_);
            draw_indexed_ranges(pass, frame_bind_group, ubo_, framebuffer_view, 0,
                                main_tri_index_count_, 0, main_line_index_count_, render_config{},
                                mode);

            for (auto& draw : queued_dev_draws_) {
              const wgpu::BindGroup draw_bind_group =
                  framebuffer_view
                      ? create_bind_group(draw.ubo, "fxe-queued-bg-framebuffer", framebuffer_view)
                      : draw.bind_group;
              draw_indexed_ranges(pass, draw_bind_group, draw.ubo, framebuffer_view,
                                  draw.tri_index_offset, draw.tri_index_count,
                                  draw.line_index_offset, draw.line_index_count, draw.cfg, mode);
            }
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
        const bool self_blur_active = self_backdrop_blur_active();
        const u32 self_blur_passes = blur_pass_count_for_radius(self_backdrop_blur_radius_px());
        if ((has_blur || self_blur_active) && !blur_ready && !blur_texture_failure_logged_) {
          FXE_WARN("wgpu.renderer",
                   "blur intermediate texture allocation failed; falling back to unblurred base");
          blur_texture_failure_logged_ = true;
        }

        auto run_blur_chain = [&](const wgpu::TextureView& src_view, u32 pass_count,
                                  const char* vlabel, const char* hlabel) -> wgpu::TextureView {
          wgpu::TextureView cur = src_view;
          for (u32 i = 0; i < pass_count; ++i) {
            encode_post_process_pass(vblur_pipeline_, cur, blur_ping_view_, vlabel);
            encode_post_process_pass(hblur_pipeline_, blur_ping_view_, blur_pong_view_, hlabel);
            cur = blur_pong_view_;
          }
          return cur;
        };
        auto encode_fullscreen_sample_pass = [&](const wgpu::TextureView& src_view,
                                                 wgpu::LoadOp load_op, const char* label) {
          wgpu::RenderPassColorAttachment color{};
          color.view = color_target_view_ ? color_target_view_ : view;
          if (multisample_count_ > 1)
            color.resolveTarget = capture_resolve_view_;
          color.loadOp = load_op;
          color.storeOp = wgpu::StoreOp::Store;
          auto cv = clear_color();
          color.clearValue = {static_cast<double>(cv.x), static_cast<double>(cv.y),
                              static_cast<double>(cv.z), static_cast<double>(cv.w)};
          color.depthSlice = wgpu::kDepthSliceUndefined;
          wgpu::RenderPassDescriptor pass_desc{};
          pass_desc.label = label;
          pass_desc.colorAttachmentCount = 1;
          pass_desc.colorAttachments = &color;
          wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
          pass.SetPipeline(sample_fullscreen_pipeline_);
          pass.SetBindGroup(0, create_bind_group(ubo_, label, src_view));
          pass.Draw(3, 1, 0, 0);
          pass.End();
        };

        const bool can_self_blur = self_blur_active && self_blur_passes > 0 && blur_ready &&
                                   self_blur_history_valid_ && self_blur_history_view_;
        if (can_self_blur) {
          const wgpu::TextureView blurred_history =
              run_blur_chain(self_blur_history_view_, self_blur_passes, "fxe-self-vblur-pass",
                             "fxe-self-hblur-pass");
          encode_fullscreen_sample_pass(blurred_history, wgpu::LoadOp::Clear,
                                        "fxe-self-sample-pass");
        }

        if (has_blur && blur_ready) {
          encode_draw_pass(can_self_blur ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear,
                           wgpu::LoadOp::Clear, blur_draw_mode::base);
          capture_frame_for_blur(encoder, surf_tex.texture);
          const wgpu::TextureView blurred_frame =
              run_blur_chain(blur_capture_view_, 1, "fxe-vblur-pass", "fxe-hblur-pass");
          encode_draw_pass(wgpu::LoadOp::Load, wgpu::LoadOp::Load, blur_draw_mode::composite,
                           blurred_frame);
        } else {
          encode_draw_pass(can_self_blur ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear,
                           wgpu::LoadOp::Clear,
                           has_blur ? blur_draw_mode::base : blur_draw_mode::all);
        }

        if (self_blur_history_texture_) {
          copy_full_frame_texture(encoder, current_frame_texture(surf_tex.texture),
                                  self_blur_history_texture_);
          self_blur_history_valid_ = true;
        }

        // When MSAA is on we resolved into capture_resolve_texture_; blit it
        // into the surface so the user actually sees their frame.
        if (multisample_count_ > 1 && capture_resolve_texture_) {
          copy_full_frame_texture(encoder, capture_resolve_texture_, surf_tex.texture);
        }

        // Capture path: encode CopyTextureToBuffer into the SAME command
        // encoder so the GPU executes copy after the render pass's resolve
        // and store ops are committed. Mapping is requested asynchronously
        // after submit; capture_frame() only polls the state machine.
        //
        // If JS called requestRedraw during this frame's render (typically
        // because fxe-ui's first-frame layout was unresolved and dirtied
        // ancestors for a follow-up rebuild), defer the readback: this
        // frame's framebuffer doesn't reflect the settled layout yet.
        // Stay armed; the next end_frame after the redraw cycle finishes
        // will encode the copy from a stable frame.
        const bool frame_unsettled = win_.peek_redraw_request();
        bool need_readback = false;
        u64 readback_size = 0;
        u32 readback_padded_row = 0;
        {
          std::lock_guard<std::mutex> lock(capture_mutex_);
          if (capture_state_ == pending_capture::idle && capture_requested_ && !frame_unsettled) {
            capture_requested_ = false;
            capture_state_ = pending_capture::copy_encoded;
            need_readback = true;
          }
        }
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
          {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            capture_pending_size_ = readback_size;
            capture_pending_padded_row_ = readback_padded_row;
            capture_pending_w_ = fb_w_;
            capture_pending_h_ = fb_h_;
            capture_pending_is_bgra_ = (surface_format_ == wgpu::TextureFormat::BGRA8Unorm ||
                                        surface_format_ == wgpu::TextureFormat::BGRA8UnormSrgb);
          }
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
        } else if (need_readback) {
          std::lock_guard<std::mutex> lock(capture_mutex_);
          capture_state_ = pending_capture::failed;
          capture_error_ = "capture failed: framebuffer has zero size";
        }

        wgpu::CommandBufferDescriptor cb_desc{};
        wgpu::CommandBuffer cmds = encoder.Finish(&cb_desc);
        queue_.Submit(1, &cmds);

        if (need_readback && readback_size > 0) {
          begin_capture_map(readback_size);
        }

        captured_frame_available_ = false;
        surface_.Present();
        instance_.ProcessEvents();
      }

      bool queue_dev(const command_view& src, const vshader_cbuf& cbuf,
                     const render_config& cfg) override {
        queued_dev_draw draw{};
        draw.tri_index_count = static_cast<u32>(src.indices(vertex_topology::triangle).size());
        draw.line_index_count = static_cast<u32>(src.indices(vertex_topology::line).size());
        draw.cfg = cfg;
        const auto src_vertices = src.vertices();
        draw.src.vertex_buffer.assign(src_vertices.begin(), src_vertices.end());
        for (usize i = 0; i != static_cast<usize>(vertex_topology::max); ++i) {
          const auto topology = static_cast<vertex_topology>(i);
          const auto src_indices = src.indices(topology);
          draw.src.index_buffers[i].assign(src_indices.begin(), src_indices.end());
        }
        draw.src.epoch = src.epoch_value();
        draw.ubo = create_uniform_buffer(cbuf, "fxe-queued-ubo");
        draw.bind_group = create_bind_group(draw.ubo, "fxe-queued-bg");
        queued_dev_draws_.push_back(std::move(draw));
        queued_dev_prepared_ = false;
        return true;
      }

      void stage_captured_frame() override {
        captured_frame_available_ = true;
      }

      // Page.screenshot path. The first call arms one readback for the next
      // submitted frame and returns a retryable error. Later calls poll the
      // asynchronous map state until the cached RGBA pixels are ready or the
      // readback fails. Repeated calls while pending never arm another copy.
      capture_result capture_frame() override {
        instance_.ProcessEvents();

        capture_result r;
        bool should_post_redraw = false;
        {
          std::lock_guard<std::mutex> lock(capture_mutex_);
          switch (capture_state_) {
          case pending_capture::ready:
            r.ok = true;
            r.width = capture_pixels_w_;
            r.height = capture_pixels_h_;
            r.rgba = capture_pixels_;
            return r;
          case pending_capture::failed:
            r.ok = false;
            r.error = capture_error_.empty() ? "capture failed" : capture_error_;
            capture_error_.clear();
            capture_requested_ = false;
            capture_state_ = pending_capture::idle;
            return r;
          case pending_capture::copy_encoded:
          case pending_capture::map_pending:
            r.ok = false;
            r.error = "capture in progress; retry shortly";
            return r;
          case pending_capture::idle:
            capture_requested_ = true;
            should_post_redraw = true;
            r.ok = false;
            r.error = "capture armed; retry after the next render";
            break;
          }
        }

        if (should_post_redraw)
          win_.post_redraw();
        return r;
      }

    private:
      // Called from end_frame() after Submit. The copy has already been encoded
      // into the same command buffer; MapAsync is registered with a non-blocking
      // callback mode so the render thread never waits for GPU readback.
      void begin_capture_map(u64 needed) {
        {
          std::lock_guard<std::mutex> lock(capture_mutex_);
          capture_pending_size_ = needed;
          capture_state_ = pending_capture::map_pending;
        }
        auto fut = capture_staging_buf_.MapAsync(
            wgpu::MapMode::Read, 0, needed, wgpu::CallbackMode::AllowSpontaneous,
            [this, needed](wgpu::MapAsyncStatus status, wgpu::StringView msg) {
              complete_capture_map(status, msg, needed);
            });
        (void)fut;
      }

      void complete_capture_map(wgpu::MapAsyncStatus status, wgpu::StringView msg, u64 needed) {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (capture_state_ != pending_capture::map_pending)
          return;

        auto fail = [&](std::string message) {
          capture_error_ = std::move(message);
          capture_state_ = pending_capture::failed;
        };

        if (status != wgpu::MapAsyncStatus::Success) {
          std::string message = "capture MapAsync failed";
          if (msg.data && msg.length > 0) {
            message += ": ";
            message.append(msg.data, msg.length);
          }
          fail(std::move(message));
          return;
        }

        const u64 mapped_size = std::min<u64>(needed, capture_pending_size_);
        const auto* mapped =
            static_cast<const u8*>(capture_staging_buf_.GetConstMappedRange(0, mapped_size));
        if (!mapped) {
          capture_staging_buf_.Unmap();
          fail("capture GetConstMappedRange returned null");
          return;
        }

        const u32 row_bytes = capture_pending_w_ * 4u;
        std::vector<u8> pixels(usize(row_bytes) * capture_pending_h_, 0);
        for (u32 y = 0; y < capture_pending_h_; ++y) {
          const u8* in = mapped + usize(y) * capture_pending_padded_row_;
          u8* out = pixels.data() + usize(y) * row_bytes;
          if (capture_pending_is_bgra_) {
            for (u32 x = 0; x < capture_pending_w_; ++x) {
              out[x * 4 + 0] = in[x * 4 + 2]; // R
              out[x * 4 + 1] = in[x * 4 + 1]; // G
              out[x * 4 + 2] = in[x * 4 + 0]; // B
              out[x * 4 + 3] = in[x * 4 + 3]; // A
            }
          } else {
            std::memcpy(out, in, row_bytes);
          }
        }
        capture_staging_buf_.Unmap();
        capture_pixels_ = std::move(pixels);
        capture_pixels_w_ = capture_pending_w_;
        capture_pixels_h_ = capture_pending_h_;
        ++capture_frame_seq_;
        capture_error_.clear();
        capture_state_ = pending_capture::ready;
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

      wgpu::Device& device() override {
        return device_;
      }
      wgpu::Queue& queue() override {
        return queue_;
      }
      wgpu::TextureFormat color_format() const override {
        return surface_format_;
      }
      wgpu::TextureFormat depth_format() const override {
        return depth_format_;
      }
      std::vector<u32> supported_multisample_counts() const override {
        if (supported_msaa_cache_.empty())
          supported_msaa_cache_ =
              probe_supported_multisample_counts(device_, surface_format_, depth_format_);
        return supported_msaa_cache_;
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

      // Bind / unbind a user texture slot (0..3). View can be a sampleable
      // wgpu::TextureView; passing an empty view clears the slot back to
      // the atlas placeholder. Marks the bind group as needing a rebuild
      // so the next draw picks up the change.
      void bind_user_texture(u32 slot, wgpu::TextureView view) override {
        if (slot >= user_tex_views_.size())
          return;
        user_tex_views_[slot] = std::move(view);
        atlas_dirty_ = true;
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
        std::array<wgpu::BindGroupLayoutEntry, 12> bgl_entries{};
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
        // Slots 8..11 are user-texture slots used by drawTextureQuad /
        // surface caching. Default-bound to atlas_view_ as a placeholder
        // until a binding call swaps them in.
        for (u32 i = 8; i < 12; ++i) {
          bgl_entries[i].binding = i;
          bgl_entries[i].visibility = wgpu::ShaderStage::Fragment;
          bgl_entries[i].texture.sampleType = wgpu::TextureSampleType::Float;
          bgl_entries[i].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        }
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
        key.sample_count = multisample_count_;
        key.fs_entry = "ps_sample";
        sample_fullscreen_pipeline_ =
            pipeline_cache_.acquire(key, device_, pipeline_layout_, shader_);
        key.sample_count = 1;
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

      wgpu::BindGroup create_bind_group(const wgpu::Buffer& ubo, const char* label,
                                        const wgpu::TextureView& framebuffer_view = {},
                                        const wgpu::TextureView& external_view = {}) {
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
        entries[8].binding = 8;
        entries[8].textureView =
            external_view ? external_view : (user_tex_views_[0] ? user_tex_views_[0] : atlas_view_);
        for (u32 i = 1; i < 4; ++i) {
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

      void refresh_atlas_bind_groups_if_dirty() {
        if (!atlas_dirty_)
          return;
        bind_group_ = create_bind_group(ubo_, "fxe-bg");
        for (auto& draw : queued_dev_draws_)
          draw.bind_group = create_bind_group(draw.ubo, "fxe-queued-bg");
        atlas_dirty_ = false;
      }
      wgpu::TextureView resolve_external_texture_view(texture_id id) {
        if ((id & kExternalTextureFlag) == 0u)
          return {};
        auto it = external_textures_.find(id);
        auto tex = find_external_texture(id);
        if (!tex) {
          return it != external_textures_.end() ? it->second.view : wgpu::TextureView{};
        }
        if (tex->size.x == 0 || tex->size.y == 0 || tex->pixels.empty())
          return {};
        const u64 pixel_hash = hash_atlas_pixels(*tex);
        auto& cached = external_textures_[id];
        if (!cached.texture || cached.width != tex->size.x || cached.height != tex->size.y ||
            cached.hash != pixel_hash) {
          destroy_texture(cached.texture);
          cached.view = {};
          wgpu::TextureDescriptor td{};
          td.label = "fxe-external-texture";
          td.dimension = wgpu::TextureDimension::e2D;
          td.size = {tex->size.x, tex->size.y, 1};
          td.format = wgpu::TextureFormat::RGBA8Unorm;
          td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
          td.mipLevelCount = 1;
          td.sampleCount = 1;
          cached.texture = device_.CreateTexture(&td);
          cached.view = cached.texture.CreateView();
          cached.width = tex->size.x;
          cached.height = tex->size.y;
          cached.hash = pixel_hash;
          wgpu::TexelCopyTextureInfo dst{};
          dst.texture = cached.texture;
          dst.mipLevel = 0;
          dst.origin = {0, 0, 0};
          dst.aspect = wgpu::TextureAspect::All;
          wgpu::TexelCopyBufferLayout layout{};
          layout.offset = 0;
          layout.bytesPerRow = tex->size.x * 4;
          layout.rowsPerImage = tex->size.y;
          wgpu::Extent3D extent{tex->size.x, tex->size.y, 1};
          queue_.WriteTexture(&dst, tex->pixels.data(), static_cast<usize>(tex->pixels.size()) * 4,
                              &layout, &extent);
        }
        return cached.view;
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
            static_cast<u32>(upload_buf_.index_buffers[usize(vertex_topology::triangle)].size());
        main_line_index_count_ =
            static_cast<u32>(upload_buf_.index_buffers[usize(vertex_topology::line)].size());

        for (auto& draw : queued_dev_draws_) {
          draw.tri_index_offset =
              upload_buf_.index_buffers[usize(vertex_topology::triangle)].size() * sizeof(u32);
          draw.line_index_offset =
              upload_buf_.index_buffers[usize(vertex_topology::line)].size() * sizeof(u32);
          upload_buf_.queue(draw.src);
        }
        queued_dev_prepared_ = true;
      }
      void flush_dynamic() {
        const u64 vbytes = upload_buf_.vertex_buffer.size() * sizeof(vertex);
        const u64 tri_idx_bytes =
            upload_buf_.index_buffers[usize(vertex_topology::triangle)].size() * sizeof(u32);
        const u64 line_idx_bytes =
            upload_buf_.index_buffers[usize(vertex_topology::line)].size() * sizeof(u32);

        ensure_buffer(vbuf_, vbuf_capacity_, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                      vbytes ? vbytes : kInitialDynamicBytes, "fxe-vbuf");
        ensure_buffer(tri_ibuf_, tri_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
                      tri_idx_bytes ? tri_idx_bytes : kInitialDynamicBytes, "fxe-tri-ibuf");
        ensure_buffer(line_ibuf_, line_ibuf_capacity_,
                      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
                      line_idx_bytes ? line_idx_bytes : kInitialDynamicBytes, "fxe-line-ibuf");

        if (vbytes) {
          upload_gpu_vertices_ = upload_buf_.vertex_buffer;
          for (auto& v : upload_gpu_vertices_) {
            const texture_id tx = vertex_texture_id(v);
            if ((tx & kExternalTextureFlag) != 0u)
              v.uv.z = std::bit_cast<float>(user_tex_flag);
          }
          queue_.WriteBuffer(vbuf_, 0, upload_gpu_vertices_.data(), vbytes);
        } else {
          upload_gpu_vertices_.clear();
        }
        if (tri_idx_bytes) {
          queue_.WriteBuffer(tri_ibuf_, 0,
                             upload_buf_.index_buffers[usize(vertex_topology::triangle)].data(),
                             tri_idx_bytes);
        }
        if (line_idx_bytes) {
          queue_.WriteBuffer(line_ibuf_, 0,
                             upload_buf_.index_buffers[usize(vertex_topology::line)].data(),
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
        // Dawn's Surface.Configure resets CAMetalLayer.displaySyncEnabled to
        // match the requested present mode. On macOS Fifo is the only mode
        // Dawn exposes, so even with --no-vsync we'd be re-pinned to display
        // refresh after every reconfigure (resize, dpr change, …) unless we
        // re-assert the user's vsync intent here. The window backend honours
        // it via CAMetalLayer.displaySyncEnabled.
        win_.set_vsync(want_vsync_);

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
        blur_ping_view_ = {};
        blur_pong_view_ = {};
        self_blur_history_view_ = {};
        destroy_texture(blur_capture_texture_);
        destroy_texture(blur_ping_texture_);
        destroy_texture(blur_pong_texture_);
        destroy_texture(self_blur_history_texture_);
        self_blur_history_valid_ = false;
        wgpu::TextureDescriptor blur_desc{};
        blur_desc.dimension = wgpu::TextureDimension::e2D;
        blur_desc.size = {w, h, 1};
        blur_desc.format = surface_format_;
        blur_desc.usage = wgpu::TextureUsage::RenderAttachment |
                          wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst |
                          wgpu::TextureUsage::CopySrc;
        blur_desc.sampleCount = 1;
        blur_desc.label = "fxe-blur-capture";
        blur_capture_texture_ = device_.CreateTexture(&blur_desc);
        blur_capture_view_ = blur_capture_texture_.CreateView();
        blur_desc.label = "fxe-blur-ping";
        blur_ping_texture_ = device_.CreateTexture(&blur_desc);
        blur_ping_view_ = blur_ping_texture_.CreateView();
        blur_desc.label = "fxe-blur-pong";
        blur_pong_texture_ = device_.CreateTexture(&blur_desc);
        blur_pong_view_ = blur_pong_texture_.CreateView();
        blur_desc.label = "fxe-self-blur-history";
        self_blur_history_texture_ = device_.CreateTexture(&blur_desc);
        self_blur_history_view_ = self_blur_history_texture_.CreateView();
        if (bgl_) {
          bind_group_ = create_bind_group(ubo_, "fxe-bg");
          for (auto& draw : queued_dev_draws_)
            draw.bind_group = create_bind_group(draw.ubo, "fxe-queued-bg");
        }

        fb_w_ = w;
        fb_h_ = h;
      }

      enum class blur_draw_mode { all, base, composite };

      [[nodiscard]] bool range_has_framebuffer_samples(u64 tri_offset, u32 tri_count) const {
        const auto& tri = upload_buf_.index_buffers[usize(vertex_topology::triangle)];
        const usize begin = static_cast<usize>(tri_offset / sizeof(u32));
        const usize end = std::min<usize>(begin + tri_count, tri.size());
        for (usize i = begin; i + 2 < end; i += 3) {
          if (classify_triangle(upload_buf_.vertex_buffer, &tri[i]) ==
              primitive_effect::framebuffer_sample)
            return true;
        }
        return false;
      }

      [[nodiscard]] wgpu::RenderPipeline pipeline_for_effect(primitive_effect effect) const {
        switch (effect) {
        case primitive_effect::alpha_blend:
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

      void draw_triangle_batch(wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bind_group,
                               const wgpu::Buffer& ubo, const wgpu::TextureView& framebuffer_view,
                               primitive_effect effect, texture_id external_texture,
                               u64 base_offset, u32 first, u32 count) {
        if (count == 0)
          return;
        if (external_texture != null_texture) {
          const wgpu::TextureView view = resolve_external_texture_view(external_texture);
          if (!view)
            return;
          pass.SetBindGroup(0, create_bind_group(ubo, "fxe-bg-external", framebuffer_view, view));
        } else {
          pass.SetBindGroup(0, bind_group);
        }
        pass.SetPipeline(pipeline_for_effect(effect));
        pass.SetIndexBuffer(tri_ibuf_, wgpu::IndexFormat::Uint32,
                            base_offset + u64(first) * sizeof(u32), count * sizeof(u32));
        pass.DrawIndexed(count, 1, 0, 0, 0);
      }

      void draw_triangles_for_mode(wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bind_group,
                                   const wgpu::Buffer& ubo,
                                   const wgpu::TextureView& framebuffer_view, u64 tri_offset,
                                   u32 tri_count, blur_draw_mode mode) {
        const auto& tri = upload_buf_.index_buffers[usize(vertex_topology::triangle)];
        const usize begin = static_cast<usize>(tri_offset / sizeof(u32));
        const usize end = std::min<usize>(begin + tri_count, tri.size());
        u32 batch_first = 0;
        u32 batch_count = 0;
        primitive_effect batch_effect = primitive_effect::color;
        texture_id batch_external_texture = null_texture;
        auto flush = [&] {
          draw_triangle_batch(pass, bind_group, ubo, framebuffer_view, batch_effect,
                              batch_external_texture, tri_offset, batch_first, batch_count);
          batch_count = 0;
        };
        for (usize i = begin; i + 2 < end; i += 3) {
          const primitive_effect effect = classify_triangle(upload_buf_.vertex_buffer, &tri[i]);
          const texture_id external_texture =
              triangle_external_texture_id(upload_buf_.vertex_buffer, &tri[i]);
          const bool composite_effect =
              effect == primitive_effect::framebuffer_sample ||
              (effect == primitive_effect::alpha_blend && external_texture == null_texture);
          if (mode == blur_draw_mode::base && composite_effect)
            continue;
          if (mode == blur_draw_mode::composite && !composite_effect)
            continue;
          const u32 rel = static_cast<u32>(i - begin);
          if (batch_count == 0) {
            batch_first = rel;
            batch_count = 3;
            batch_effect = effect;
            batch_external_texture = external_texture;
          } else if (effect == batch_effect && batch_external_texture == external_texture &&
                     batch_first + batch_count == rel) {
            batch_count += 3;
          } else {
            flush();
            batch_first = rel;
            batch_count = 3;
            batch_effect = effect;
            batch_external_texture = external_texture;
          }
        }
        flush();
      }

      void draw_indexed_ranges(wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bind_group,
                               const wgpu::Buffer& ubo, const wgpu::TextureView& framebuffer_view,
                               u64 tri_offset, u32 tri_count, u64 line_offset, u32 line_count,
                               const render_config& cfg,
                               blur_draw_mode mode = blur_draw_mode::all) {
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
        if (tri_count > 0)
          draw_triangles_for_mode(pass, bind_group, ubo, framebuffer_view, tri_offset, tri_count,
                                  mode);
        if (line_count > 0 && mode != blur_draw_mode::composite) {
          pass.SetPipeline(line_pipeline_);
          pass.SetIndexBuffer(line_ibuf_, wgpu::IndexFormat::Uint32, line_offset,
                              line_count * sizeof(u32));
          pass.DrawIndexed(line_count, 1, 0, 0, 0);
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
          for (usize i = 0; i < caps.presentModeCount; ++i) {
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

      [[nodiscard]] bool self_backdrop_blur_active() const {
        return self_backdrop_blur_enabled() && win_.is_transparent();
      }

      void copy_full_frame_texture(wgpu::CommandEncoder& encoder, const wgpu::Texture& src_texture,
                                   const wgpu::Texture& dst_texture) {
        if (!src_texture || !dst_texture || fb_w_ == 0 || fb_h_ == 0)
          return;
        wgpu::TexelCopyTextureInfo src{};
        src.texture = src_texture;
        src.mipLevel = 0;
        src.origin = {0, 0, 0};
        src.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = dst_texture;
        dst.mipLevel = 0;
        dst.origin = {0, 0, 0};
        dst.aspect = wgpu::TextureAspect::All;
        wgpu::Extent3D extent{fb_w_, fb_h_, 1};
        encoder.CopyTextureToTexture(&src, &dst, &extent);
      }

      [[nodiscard]] const wgpu::Texture&
      current_frame_texture(const wgpu::Texture& surface_texture) const {
        return (multisample_count_ > 1) ? capture_resolve_texture_ : surface_texture;
      }
      void capture_frame_for_blur(wgpu::CommandEncoder& encoder,
                                  const wgpu::Texture& surface_texture) {
        if (!blur_capture_texture_)
          return;
        copy_full_frame_texture(encoder, current_frame_texture(surface_texture),
                                blur_capture_texture_);
      }

      window& win_;
      wgpu::Instance instance_;
      wgpu::Surface surface_;
      wgpu::Adapter adapter_;
      wgpu::Device device_;
      wgpu::Queue queue_;
      wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::BGRA8Unorm;
      wgpu::TextureFormat depth_format_ = wgpu::TextureFormat::Depth24Plus;
      mutable std::vector<u32> supported_msaa_cache_;

      wgpu::CompositeAlphaMode alpha_mode_ = wgpu::CompositeAlphaMode::Auto;
      wgpu::PresentMode present_mode_ = wgpu::PresentMode::Fifo;
      bool want_vsync_ = true;

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
      wgpu::RenderPipeline sample_fullscreen_pipeline_;
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
      wgpu::Texture blur_ping_texture_;
      wgpu::TextureView blur_ping_view_;
      wgpu::Texture blur_pong_texture_;
      wgpu::TextureView blur_pong_view_;
      wgpu::Texture self_blur_history_texture_;
      wgpu::TextureView self_blur_history_view_;
      bool self_blur_history_valid_ = false;
      bool blur_texture_failure_logged_ = false;
      wgpu::Texture atlas_texture_;
      wgpu::TextureView atlas_view_;
      // User texture slots used by the WGSL `user_tex_0..3` bindings.
      // Default-empty; falls back to atlas_view_ in the bind group when
      // unbound. Indexed via `bind_user_texture(slot, view)`.
      std::array<wgpu::TextureView, 4> user_tex_views_{};
      struct external_texture_gpu {
        wgpu::Texture texture;
        wgpu::TextureView view;
        u32 width = 0;
        u32 height = 0;
        u64 hash = 0;
      };
      std::unordered_map<texture_id, external_texture_gpu> external_textures_;

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

      // Capture/screenshot state. capture_frame() arms one pending copy; end_frame()
      // encodes CopyTextureToBuffer and registers a non-blocking MapAsync callback.
      // The callback may run off-thread, so every field below is protected by
      // capture_mutex_. capture_pixels_w_/h_ describe the cached byte layout
      // and may differ from the current framebuffer after resize.
      std::mutex capture_mutex_;
      pending_capture capture_state_ = pending_capture::idle;
      bool capture_requested_ = false;
      std::string capture_error_;
      wgpu::Buffer capture_staging_buf_;
      u64 capture_staging_size_ = 0;
      u64 capture_pending_size_ = 0;
      u32 capture_pending_padded_row_ = 0;
      u32 capture_pending_w_ = 0;
      u32 capture_pending_h_ = 0;
      bool capture_pending_is_bgra_ = true;
      std::vector<u8> capture_pixels_;
      u32 capture_pixels_w_ = 0;
      u32 capture_pixels_h_ = 0;
      u64 capture_frame_seq_ = 0;

      u32 fb_w_ = 0;
      u32 fb_h_ = 0;
      bool captured_frame_available_ = false;
      bool queued_dev_prepared_ = false;
      std::vector<vertex> upload_gpu_vertices_;
    };
  } // namespace

  std::unique_ptr<renderer> create_renderer(window& w, const renderer_options& opts) {
    return std::make_unique<dawn_renderer>(w, opts);
  }
} // namespace fxe

#endif // FXE_HAS_WGPU
