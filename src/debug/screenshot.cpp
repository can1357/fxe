#include "screenshot.hpp"
#include <fxe/types.hpp>

#include <csetjmp>
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <png.h>
#include <turbojpeg.h>

#ifndef FXE_HAS_STB
#define FXE_HAS_STB 0
#endif
#if FXE_HAS_STB
// Resize stays on stb_image_resize2 (header-only, single-TU implementation
// in src/core/stb_impl.cpp). PNG/JPEG encoding now uses libpng /
// libjpeg-turbo directly.
#include <stb_image_resize2.h>
#endif

namespace fxe::debug {
  namespace {
    void set_error(std::string* err_out, std::string_view message) {
      if (err_out)
        *err_out = std::string(message);
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

    void png_write_to_string(png_structp png, png_bytep data, png_size_t size) {
      auto* out = static_cast<std::string*>(png_get_io_ptr(png));
      out->append(reinterpret_cast<const char*>(data), size);
    }
    void png_flush_noop(png_structp) {}

    void png_error_fn(png_structp png, png_const_charp msg) {
      auto* err = static_cast<std::string*>(png_get_error_ptr(png));
      if (err)
        *err = std::string("libpng: ") + (msg ? msg : "");
      std::longjmp(png_jmpbuf(png), 1);
    }
    void png_warn_noop(png_structp, png_const_charp) noexcept {}
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

    std::string err_msg;
    png_structp png =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, &err_msg, &png_error_fn, &png_warn_noop);
    if (!png) {
      set_error(err_out, "screenshot: png_create_write_struct failed");
      return {};
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
      png_destroy_write_struct(&png, nullptr);
      set_error(err_out, "screenshot: png_create_info_struct failed");
      return {};
    }

    std::string out;
    out.reserve(static_cast<usize>(width) * height);

    if (setjmp(png_jmpbuf(png))) {
      png_destroy_write_struct(&png, &info);
      set_error(err_out, err_msg.empty() ? "libpng: encode failed" : err_msg);
      return {};
    }

    png_set_write_fn(png, &out, &png_write_to_string, &png_flush_noop);
    png_set_compression_level(png, 6);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(height);
    for (u32 y = 0; y < height; ++y) {
      rows[y] = const_cast<png_bytep>(pixels + static_cast<usize>(y) * row_stride_bytes);
    }
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
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

    tjhandle handle = tj3Init(TJINIT_COMPRESS);
    if (!handle) {
      set_error(err_out, "screenshot: tj3Init failed");
      return {};
    }
    if (tj3Set(handle, TJPARAM_QUALITY, quality) < 0 ||
        tj3Set(handle, TJPARAM_SUBSAMP, TJSAMP_420) < 0) {
      set_error(err_out, std::string("libjpeg-turbo: ") + tj3GetErrorStr(handle));
      tj3Destroy(handle);
      return {};
    }

    unsigned char* jpeg_buf = nullptr;
    size_t jpeg_size = 0;
    const int rc =
        tj3Compress8(handle, pixels, static_cast<int>(width),
                     static_cast<int>(row_stride_bytes), static_cast<int>(height), TJPF_RGBA,
                     &jpeg_buf, &jpeg_size);
    if (rc < 0) {
      set_error(err_out, std::string("libjpeg-turbo: ") + tj3GetErrorStr(handle));
      if (jpeg_buf)
        tj3Free(jpeg_buf);
      tj3Destroy(handle);
      return {};
    }

    std::string out(reinterpret_cast<const char*>(jpeg_buf), jpeg_size);
    tj3Free(jpeg_buf);
    tj3Destroy(handle);
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

#if FXE_HAS_STB
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
#else
    set_error(err_out, "screenshot: stb resize unavailable in this build");
    return false;
#endif
  }
} // namespace fxe::debug
