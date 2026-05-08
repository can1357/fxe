#pragma once

#include <fxe/renderer.hpp>
#include <fxe/types.hpp>

#ifndef FXE_HAS_WGPU
#define FXE_HAS_WGPU 0
#endif
#if FXE_HAS_WGPU
#include <webgpu/webgpu_cpp.h>
#endif

#include <memory>
#include <vector>

namespace fxe {
  struct offscreen_options {
    u32 width = 1;
    u32 height = 1;
    u32 multisample = 1;
    bool enable_depth = true;
    u32 mip_levels = 1;
#if FXE_HAS_WGPU
    wgpu::TextureFormat color_format = wgpu::TextureFormat::RGBA8Unorm;
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Depth24Plus;
    // Optional parent: when set, the offscreen reuses this device + queue
    // instead of creating a private one. Required when the offscreen's
    // color attachment will be sampled by another renderer (cross-device
    // texture sharing is not supported in WebGPU/Dawn).
    wgpu::Device parent_device{};
    wgpu::Queue parent_queue{};
    wgpu::Adapter parent_adapter{};
#endif
  };

  class offscreen_renderer : public renderer {
  public:
    static std::unique_ptr<offscreen_renderer> create(const offscreen_options&);
    virtual std::vector<u8> read_rgba8() = 0;
    // Sampleable view of the offscreen color attachment. Returned view is
    // valid until the offscreen is resized or destroyed; callers binding it
    // into another renderer should rebind after a resize. Empty when the
    // null backend is used.
#if FXE_HAS_WGPU
    virtual wgpu::TextureView color_texture_view() const {
      return {};
    }
#endif
    ~offscreen_renderer() override = default;
  };
} // namespace fxe
