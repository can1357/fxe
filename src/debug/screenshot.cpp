#include "screenshot.hpp"
#include <fxe/types.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

#ifndef FXE_HAS_STB
#define FXE_HAS_STB 0
#endif

#if FXE_HAS_STB
// stb_image_write / stb_image_resize2 are implemented in src/core/stb_impl.cpp.
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#endif

namespace fxe::debug {
  namespace {
    void set_error(std::string* err_out, std::string_view message) {
      if (err_out)
        *err_out = std::string(message);
    }
  } // namespace

#if FXE_HAS_STB
  namespace {
    void byte_writer(void* ctx, void* data, int size) {
      auto* out = static_cast<std::string*>(ctx);
      out->append(static_cast<const char*>(data), static_cast<usize>(size));
    }

    // Copy a sub-rect of an RGBA8 image into a tightly-packed buffer.
    void copy_rect(const u8* src, u32 src_stride, u32 cx, u32 cy, u32 cw, u32 ch,
                   std::vector<u8>& out) {
      const u32 row = cw * 4u;
      out.resize(static_cast<usize>(row) * ch);
      for (u32 y = 0; y < ch; ++y) {
        const u8* row_src = src + static_cast<usize>(cy + y) * src_stride + cx * 4u;
        std::memcpy(out.data() + static_cast<usize>(y) * row, row_src, row);
      }
    }
  } // namespace

  std::string encode_png_rgba8(const u8* pixels, u32 width, u32 height, u32 row_stride_bytes,
                               std::string* err_out) {
    if (width == 0 || height == 0) {
      set_error(err_out, "screenshot: zero-dim source");
      return {};
    }
    if (!pixels) {
      set_error(err_out, "screenshot: null pixel buffer");
      return {};
    }

    const u32 tight = width * 4u;
    const u8* src = pixels;
    std::vector<u8> packed;
    if (row_stride_bytes != tight) {
      packed.resize(static_cast<usize>(tight) * height);
      for (u32 y = 0; y < height; ++y) {
        std::memcpy(packed.data() + static_cast<usize>(y) * tight,
                    pixels + static_cast<usize>(y) * row_stride_bytes, tight);
      }
      src = packed.data();
    }

    std::string out;
    out.reserve(static_cast<usize>(tight) * height / 4);
    int rc = stbi_write_png_to_func(&byte_writer, &out, static_cast<int>(width),
                                    static_cast<int>(height), 4, src, static_cast<int>(tight));
    if (rc == 0) {
      set_error(err_out, "stb encode failed");
      return {};
    }
    return out;
  }

  std::string encode_jpeg_rgba8(const u8* pixels, u32 width, u32 height, u32 row_stride_bytes,
                                int quality, std::string* err_out) {
    if (width == 0 || height == 0) {
      set_error(err_out, "screenshot: zero-dim source");
      return {};
    }
    if (!pixels) {
      set_error(err_out, "screenshot: null pixel buffer");
      return {};
    }
    quality = std::clamp(quality, 1, 100);

    // stb's JPEG writer wants 3-channel input. Pack RGB tightly, dropping A.
    const u32 tight = width * 3u;
    std::vector<u8> rgb(static_cast<usize>(tight) * height);
    for (u32 y = 0; y < height; ++y) {
      const u8* sr = pixels + static_cast<usize>(y) * row_stride_bytes;
      u8* dr = rgb.data() + static_cast<usize>(y) * tight;
      for (u32 x = 0; x < width; ++x) {
        dr[x * 3 + 0] = sr[x * 4 + 0];
        dr[x * 3 + 1] = sr[x * 4 + 1];
        dr[x * 3 + 2] = sr[x * 4 + 2];
      }
    }

    std::string out;
    int rc = stbi_write_jpg_to_func(&byte_writer, &out, static_cast<int>(width),
                                    static_cast<int>(height), 3, rgb.data(), quality);
    if (rc == 0) {
      set_error(err_out, "stb encode failed");
      return {};
    }
    return out;
  }

  bool crop_resize_rgba8(const u8* pixels, u32 src_width, u32 src_height, u32 src_stride_bytes,
                         u32 cx, u32 cy, u32 cw, u32 ch, u32 out_w, u32 out_h,
                         std::vector<u8>& out_pixels, std::string* err_out) {
    if (src_width == 0 || src_height == 0) {
      set_error(err_out, "screenshot: zero-dim source");
      return false;
    }
    if (!pixels) {
      set_error(err_out, "screenshot: null pixel buffer");
      return false;
    }

    // Clamp the crop rect to the source.
    if (cx >= src_width || cy >= src_height) {
      set_error(err_out, "screenshot: empty source rect");
      return false;
    }
    if (cw == 0)
      cw = src_width - cx;
    if (ch == 0)
      ch = src_height - cy;
    cw = std::min(cw, src_width - cx);
    ch = std::min(ch, src_height - cy);
    if (cw == 0 || ch == 0) {
      set_error(err_out, "screenshot: empty source rect");
      return false;
    }

    if (out_w == 0)
      out_w = cw;
    if (out_h == 0)
      out_h = ch;

    // Fast path: no resize and crop covers the full image with a tight stride.
    if (out_w == cw && out_h == ch) {
      copy_rect(pixels, src_stride_bytes, cx, cy, cw, ch, out_pixels);
      return true;
    }

    // Pack the crop into a contiguous buffer then resample.
    std::vector<u8> cropped;
    copy_rect(pixels, src_stride_bytes, cx, cy, cw, ch, cropped);

    out_pixels.assign(static_cast<usize>(out_w) * out_h * 4u, 0);
    auto* rc = stbir_resize_uint8_linear(cropped.data(), static_cast<int>(cw), static_cast<int>(ch),
                                         static_cast<int>(cw * 4u), out_pixels.data(),
                                         static_cast<int>(out_w), static_cast<int>(out_h),
                                         static_cast<int>(out_w * 4u), STBIR_RGBA);
    if (rc == nullptr) {
      set_error(err_out, "stb resize failed");
      return false;
    }
    return true;
  }
#else
  std::string encode_png_rgba8(const u8*, u32, u32, u32, std::string* err_out) {
    set_error(err_out, "screenshot: stb unavailable in this build");
    return {};
  }
  std::string encode_jpeg_rgba8(const u8*, u32, u32, u32, int, std::string* err_out) {
    set_error(err_out, "screenshot: stb unavailable in this build");
    return {};
  }
  bool crop_resize_rgba8(const u8*, u32, u32, u32, u32, u32, u32, u32, u32, u32, std::vector<u8>&,
                         std::string* err_out) {
    set_error(err_out, "screenshot: stb unavailable in this build");
    return false;
  }
#endif
} // namespace fxe::debug
