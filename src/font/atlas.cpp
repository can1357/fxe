// Page atlas with a shelf packer. One instance per pixel format (R8 alpha
// mask, BGRA8 color emoji). The packer is intentionally simple: a single
// shelf advances left-to-right; if the row fills, we wrap to a new shelf at
// `cursor_y + row_h + padding`. When the atlas can't accommodate a glyph we
// double the shorter axis (up to `max_size_`) and re-blit existing pixels.
// Under cache pressure callers can rebuild the page from retained glyph
// bitmaps, shrinking back to the initial size when possible.

#include <fxe/font/atlas.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fxe/log.hpp>

namespace fxe::font {

  Atlas::Atlas(Format f, u32 initial_size, u32 max_size)
      : format_(f), width_(initial_size), height_(initial_size), max_size_(max_size),
        initial_size_(initial_size) {
    pixels_.assign(static_cast<usize>(width_) * height_ * bytes_per_pixel(), 0);
    ++generation_;
  }

  void Atlas::reset_empty_() {
    width_ = std::max<u32>(initial_size_, 1);
    height_ = std::max<u32>(initial_size_, 1);
    cursor_x_ = padding_;
    cursor_y_ = padding_;
    row_h_ = 0;
    pixels_.assign(static_cast<usize>(width_) * height_ * bytes_per_pixel(), 0);
  }

  u32 Atlas::bytes_per_pixel() const noexcept {
    switch (format_) {
    case Format::grayscale:
      return 1;
    case Format::bgra:
      return 4;
    }
    return 1;
  }

  void Atlas::clear() {
    reset_empty_();
    ++generation_;
  }

  u8* Atlas::mutable_pixels() noexcept {
    ++generation_;
    return pixels_.data();
  }

  bool Atlas::grow_(u32 min_w, u32 min_h) noexcept {
    u32 new_w = std::max<u32>(width_, 1);
    u32 new_h = std::max<u32>(height_, 1);
    while (new_w < min_w && new_w < max_size_)
      new_w *= 2;
    while (new_h < min_h && new_h < max_size_)
      new_h *= 2;
    if (new_w == width_ && new_h == height_)
      return false;
    if (new_w > max_size_ || new_h > max_size_)
      return false;

    const auto bpp = static_cast<usize>(bytes_per_pixel());
    std::vector<u8> grown(static_cast<usize>(new_w) * new_h * bpp, 0);
    for (u32 y = 0; y < height_; ++y) {
      const u8* src = pixels_.data() + static_cast<usize>(y) * width_ * bpp;
      u8* dst = grown.data() + static_cast<usize>(y) * new_w * bpp;
      std::memcpy(dst, src, static_cast<usize>(width_) * bpp);
    }
    width_ = new_w;
    height_ = new_h;
    pixels_ = std::move(grown);
    ++generation_;
    FXE_DEBUG("font.atlas", "atlas_grow fmt={} w={} h={} gen={}",
              format_ == Format::bgra ? "color" : "mask", new_w, new_h, generation_);
    return true;
  }

  void Atlas::copy_into_(u32 dst_x, u32 dst_y, u32 w, u32 h, const u8* src) noexcept {
    const auto bpp = static_cast<usize>(bytes_per_pixel());
    for (u32 y = 0; y < h; ++y) {
      u8* dst = pixels_.data() + (static_cast<usize>(dst_y + y) * width_ + dst_x) * bpp;
      std::memcpy(dst, src + static_cast<usize>(y) * w * bpp, static_cast<usize>(w) * bpp);
    }
    ++generation_;
  }

  AtlasRegion Atlas::reserve(u32 w, u32 h) noexcept {
    if (w == 0 || h == 0)
      return {true, 0, 0};
    if (cursor_x_ < padding_)
      cursor_x_ = padding_;
    if (cursor_y_ < padding_)
      cursor_y_ = padding_;
    for (;;) {
      // Reserve glyph + 1px padding on each side so neighbours don't bleed.
      const u32 need_w = w + padding_ * 2;
      const u32 need_h = h + padding_ * 2;
      if (need_w > width_ || need_h > height_) {
        if (!grow_(std::max(width_ * 2, need_w), std::max(height_ * 2, need_h)))
          return {false, 0, 0};
        continue;
      }
      if (cursor_x_ + w + padding_ > width_) {
        cursor_x_ = padding_;
        cursor_y_ += row_h_ + padding_;
        row_h_ = 0;
      }
      if (cursor_y_ + h + padding_ > height_) {
        if (!grow_(width_, height_ * 2))
          return {false, 0, 0};
        continue;
      }
      AtlasRegion out{true, cursor_x_, cursor_y_};
      cursor_x_ += w + padding_;
      row_h_ = std::max(row_h_, h);
      return out;
    }
  }

  AtlasRegion Atlas::pack(u32 w, u32 h, const u8* bytes) noexcept {
    AtlasRegion r = reserve(w, h);
    if (!r.ok || w == 0 || h == 0 || !bytes)
      return r;
    copy_into_(r.x, r.y, w, h, bytes);
    return r;
  }

  bool Atlas::rebuild_from_live(std::span<AtlasRepackItem> live) noexcept {
    const u64 prev_gen = generation_;
    reset_empty_();
    ++generation_;
    FXE_WARN("font.atlas", "atlas_rebuild fmt={} items={} gen={}->{}",
             format_ == Format::bgra ? "color" : "mask", live.size(), prev_gen, generation_);
    for (auto& item : live) {
      if (!item.glyph)
        return false;
      if (item.width == 0 || item.height == 0) {
        item.glyph->atlas_x = 0;
        item.glyph->atlas_y = 0;
        continue;
      }
      if (item.pixels.size() != static_cast<usize>(item.width) * item.height * bytes_per_pixel()) {
        return false;
      }
      AtlasRegion r = pack(item.width, item.height, item.pixels.data());
      if (!r.ok)
        return false;
      item.glyph->atlas_x = r.x;
      item.glyph->atlas_y = r.y;
    }
    return true;
  }

} // namespace fxe::font
