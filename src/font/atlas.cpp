// Page atlas with a shelf packer. One instance per pixel format (R8 alpha
// mask, BGRA8 color emoji). The packer is intentionally simple: a single
// shelf advances left-to-right; if the row fills, we wrap to a new shelf at
// `cursor_y + row_h + padding`. When the atlas can't accommodate a glyph we
// double the shorter axis (up to `max_size_`) and re-blit the existing
// pixels into the grown buffer.

#include <fxe/font/atlas.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace fxe::font {

  Atlas::Atlas(Format f, std::uint32_t initial_size, std::uint32_t max_size)
      : format_(f), width_(initial_size), height_(initial_size), max_size_(max_size) {
    pixels_.assign(static_cast<std::size_t>(width_) * height_ * bytes_per_pixel(), 0);
    ++generation_;
  }

  std::uint32_t Atlas::bytes_per_pixel() const noexcept {
    switch (format_) {
    case Format::grayscale:
      return 1;
    case Format::bgra:
      return 4;
    }
    return 1;
  }

  void Atlas::clear() {
    cursor_x_ = padding_;
    cursor_y_ = padding_;
    row_h_ = 0;
    std::fill(pixels_.begin(), pixels_.end(), std::uint8_t{0});
    ++generation_;
  }

  std::uint8_t* Atlas::mutable_pixels() noexcept {
    ++generation_;
    return pixels_.data();
  }

  bool Atlas::grow_(std::uint32_t min_w, std::uint32_t min_h) noexcept {
    std::uint32_t new_w = std::max<std::uint32_t>(width_, 1);
    std::uint32_t new_h = std::max<std::uint32_t>(height_, 1);
    while (new_w < min_w && new_w < max_size_)
      new_w *= 2;
    while (new_h < min_h && new_h < max_size_)
      new_h *= 2;
    if (new_w == width_ && new_h == height_)
      return false;
    if (new_w > max_size_ || new_h > max_size_)
      return false;

    const auto bpp = static_cast<std::size_t>(bytes_per_pixel());
    std::vector<std::uint8_t> grown(static_cast<std::size_t>(new_w) * new_h * bpp, 0);
    for (std::uint32_t y = 0; y < height_; ++y) {
      const std::uint8_t* src = pixels_.data() + static_cast<std::size_t>(y) * width_ * bpp;
      std::uint8_t* dst = grown.data() + static_cast<std::size_t>(y) * new_w * bpp;
      std::memcpy(dst, src, static_cast<std::size_t>(width_) * bpp);
    }
    width_ = new_w;
    height_ = new_h;
    pixels_ = std::move(grown);
    ++generation_;
    return true;
  }

  void Atlas::copy_into_(std::uint32_t dst_x, std::uint32_t dst_y, std::uint32_t w, std::uint32_t h,
                         const std::uint8_t* src) noexcept {
    const auto bpp = static_cast<std::size_t>(bytes_per_pixel());
    for (std::uint32_t y = 0; y < h; ++y) {
      std::uint8_t* dst =
          pixels_.data() + (static_cast<std::size_t>(dst_y + y) * width_ + dst_x) * bpp;
      std::memcpy(dst, src + static_cast<std::size_t>(y) * w * bpp,
                  static_cast<std::size_t>(w) * bpp);
    }
    ++generation_;
  }

  AtlasRegion Atlas::reserve(std::uint32_t w, std::uint32_t h) noexcept {
    if (w == 0 || h == 0)
      return {true, 0, 0};
    if (cursor_x_ < padding_)
      cursor_x_ = padding_;
    if (cursor_y_ < padding_)
      cursor_y_ = padding_;
    for (;;) {
      // Reserve glyph + 1px padding on each side so neighbours don't bleed.
      const std::uint32_t need_w = w + padding_ * 2;
      const std::uint32_t need_h = h + padding_ * 2;
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

  AtlasRegion Atlas::pack(std::uint32_t w, std::uint32_t h, const std::uint8_t* bytes) noexcept {
    AtlasRegion r = reserve(w, h);
    if (!r.ok || w == 0 || h == 0 || !bytes)
      return r;
    copy_into_(r.x, r.y, w, h, bytes);
    return r;
  }

} // namespace fxe::font
