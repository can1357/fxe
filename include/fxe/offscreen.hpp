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
#endif
  };

  class offscreen_renderer : public renderer {
  public:
    static std::unique_ptr<offscreen_renderer> create(const offscreen_options&);
    virtual std::vector<u8> read_rgba8() = 0;
    ~offscreen_renderer() override = default;
  };
} // namespace fxe
