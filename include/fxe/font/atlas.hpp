#pragma once

// Bounded, rebuildable page atlas with a shelf packer. Two variants live in
// any process:
//   - a grayscale (R8) page for FT/CT alpha bitmaps.
//   - a BGRA8 page for color emoji bitmaps.
// The renderer uploads the two pages as separate textures; the shader picks
// one via a flag bit on the texture id (see src/wgpu/shaders/main.wgsl).

#include <cstdint>
#include <span>
#include <vector>

#include <fxe/font/glyph.hpp>
#include <fxe/math.hpp>
#include <fxe/types.hpp>

namespace fxe::font {

  // Result of `Atlas::pack`. `ok=false` means the atlas grew but still couldn't
  // accommodate the glyph (e.g. it would exceed `max_size`). Callers should
  // discard the glyph in that case.
  struct AtlasRegion {
    bool ok = false;
    u32 x = 0;
    u32 y = 0;
  };

  struct AtlasRepackItem {
    Glyph* glyph = nullptr;
    std::span<const u8> pixels{};
    u32 width = 0;
    u32 height = 0;
  };

  class Atlas {
  public:
    Atlas() = default;
    explicit Atlas(Format f, u32 initial_size = 256, u32 max_size = 8192);

    [[nodiscard]] Format format() const noexcept {
      return format_;
    }
    [[nodiscard]] math::uvec2 size() const noexcept {
      return {width_, height_};
    }
    [[nodiscard]] const std::vector<u8>& pixels() const noexcept {
      return pixels_;
    }
    [[nodiscard]] u32 bytes_per_pixel() const noexcept;
    // Atlas pages are bounded and rebuildable under cache pressure; callers
    // observe `generation()` to know when to re-upload pixel contents to the GPU.
    [[nodiscard]] u64 generation() const noexcept {
      return generation_;
    }
    // UV-layout generation. Bumps only when previously-emitted glyph UVs may
    // have become stale (grow/shrink/repack), so higher layers can keep caches
    // hot across pure append-only glyph writes.
    [[nodiscard]] u64 layout_generation() const noexcept {
      return layout_generation_;
    }

    // Resets the atlas to an empty initial-size page. Used by tests/repack.
    void clear();

    // Packs an opaque rectangle into the atlas. `bytes` must be either
    // `width*height` (grayscale) or `width*height*4` (BGRA). Returns the
    // top-left position of the packed region.
    [[nodiscard]] AtlasRegion pack(u32 w, u32 h, const u8* bytes) noexcept;

    // Same as `pack`, but writes zeros. Used to reserve space ahead of an
    // out-of-band upload (e.g. a rasterizer that wants to write its bitmap
    // directly into the atlas memory).
    [[nodiscard]] AtlasRegion reserve(u32 w, u32 h) noexcept;

    // Direct mutable access to the pixel buffer. Callers must clamp writes to
    // `size()`. Bumps `generation()` because the page contents change.
    [[nodiscard]] u8* mutable_pixels() noexcept;

    // Clears the page and repacks the supplied live glyph bitmaps in order.
    // Updates each glyph's atlas coordinates. Returns false if any bitmap
    // cannot fit even after growth to `max_size`.
    [[nodiscard]] bool rebuild_from_live(std::span<AtlasRepackItem> live) noexcept;

  private:
    bool grow_(u32 min_w, u32 min_h) noexcept;
    void copy_into_(u32 dst_x, u32 dst_y, u32 w, u32 h, const u8* src) noexcept;
    void reset_empty_();

    Format format_ = Format::grayscale;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 max_size_ = 8192;
    u32 initial_size_ = 0;
    u32 cursor_x_ = 1;
    u32 cursor_y_ = 1;
    u32 row_h_ = 0;
    u32 padding_ = 1;
    u64 generation_ = 0;
    u64 layout_generation_ = 0;
    std::vector<u8> pixels_;
  };

} // namespace fxe::font
