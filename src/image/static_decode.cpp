// Static-image decode (PNG / JPEG / WebP / GIF first frame) and RGBA8
// resize, dispatched from magic bytes. Replaces the previous stb_image-based
// path. Resize stays on stb_image_resize2 — small, isolated, and orthogonal
// to the codec choice.

#include <fxe/log.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gif_lib.h>
#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>

#ifndef FXE_HAS_STB
#define FXE_HAS_STB 0
#endif
#if FXE_HAS_STB
#include <stb_image_resize2.h>
#endif

namespace fxe {
  namespace {
    [[nodiscard]] bool starts_with(std::span<const u8> data, std::string_view prefix) noexcept {
      return data.size() >= prefix.size() &&
             std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
    }

    [[nodiscard]] bool is_png(std::span<const u8> data) noexcept {
      static constexpr u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      return data.size() >= sizeof(sig) && std::memcmp(data.data(), sig, sizeof(sig)) == 0;
    }

    [[nodiscard]] bool is_jpeg(std::span<const u8> data) noexcept {
      return data.size() >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
    }

    [[nodiscard]] bool is_webp(std::span<const u8> data) noexcept {
      return data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 &&
             std::memcmp(data.data() + 8, "WEBP", 4) == 0;
    }

    [[nodiscard]] bool is_gif(std::span<const u8> data) noexcept {
      return starts_with(data, "GIF87a") || starts_with(data, "GIF89a");
    }

    // ------------------------------------------------------------------
    // PNG (libpng simplified API)
    // ------------------------------------------------------------------
    [[nodiscard]] texture_data decode_png(std::span<const u8> encoded) {
      png_image image;
      std::memset(&image, 0, sizeof(image));
      image.version = PNG_IMAGE_VERSION;
      if (!png_image_begin_read_from_memory(&image, encoded.data(), encoded.size())) {
        std::string msg = std::string("load_texture: PNG header parse failed: ") + image.message;
        png_image_free(&image);
        throw std::runtime_error(std::move(msg));
      }
      image.format = PNG_FORMAT_RGBA;
      texture_data out;
      out.size = {image.width, image.height};
      const usize pixel_count = static_cast<usize>(image.width) * image.height;
      out.pixels.resize(pixel_count);
      const png_int_32 row_stride = static_cast<png_int_32>(image.width) * 4;
      if (!png_image_finish_read(&image, /*background=*/nullptr,
                                 reinterpret_cast<png_bytep>(out.pixels.data()), row_stride,
                                 /*colormap=*/nullptr)) {
        std::string msg = std::string("load_texture: PNG decode failed: ") + image.message;
        png_image_free(&image);
        throw std::runtime_error(std::move(msg));
      }
      return out;
    }

    // ------------------------------------------------------------------
    // JPEG (libjpeg-turbo TurboJPEG API)
    // ------------------------------------------------------------------
    [[nodiscard]] texture_data decode_jpeg(std::span<const u8> encoded) {
      tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
      if (!handle)
        throw std::runtime_error("load_texture: tj3Init failed");
      struct tj_guard {
        tjhandle h;
        ~tj_guard() {
          tj3Destroy(h);
        }
      } guard{handle};

      if (tj3DecompressHeader(handle, encoded.data(), encoded.size()) < 0) {
        throw std::runtime_error(std::string("load_texture: JPEG header parse failed: ") +
                                 tj3GetErrorStr(handle));
      }
      const int width = tj3Get(handle, TJPARAM_JPEGWIDTH);
      const int height = tj3Get(handle, TJPARAM_JPEGHEIGHT);
      if (width <= 0 || height <= 0)
        throw std::runtime_error("load_texture: JPEG has zero size");

      texture_data out;
      out.size = {static_cast<u32>(width), static_cast<u32>(height)};
      out.pixels.resize(static_cast<usize>(width) * static_cast<usize>(height));
      if (tj3Decompress8(handle, encoded.data(), encoded.size(),
                         reinterpret_cast<unsigned char*>(out.pixels.data()), /*pitch=*/0,
                         TJPF_RGBA) < 0) {
        throw std::runtime_error(std::string("load_texture: JPEG decode failed: ") +
                                 tj3GetErrorStr(handle));
      }
      return out;
    }

    // ------------------------------------------------------------------
    // WebP (libwebp simple decoder)
    // ------------------------------------------------------------------
    [[nodiscard]] texture_data decode_webp(std::span<const u8> encoded) {
      int width = 0;
      int height = 0;
      if (!WebPGetInfo(encoded.data(), encoded.size(), &width, &height) || width <= 0 ||
          height <= 0) {
        throw std::runtime_error("load_texture: WebP header parse failed");
      }
      texture_data out;
      out.size = {static_cast<u32>(width), static_cast<u32>(height)};
      out.pixels.resize(static_cast<usize>(width) * static_cast<usize>(height));
      const int row_stride = width * 4;
      if (!WebPDecodeRGBAInto(encoded.data(), encoded.size(),
                              reinterpret_cast<u8*>(out.pixels.data()),
                              out.pixels.size() * sizeof(r8g8b8a8), row_stride)) {
        throw std::runtime_error("load_texture: WebP decode failed");
      }
      return out;
    }

    // ------------------------------------------------------------------
    // GIF first frame (giflib)
    // ------------------------------------------------------------------
    struct gif_reader {
      std::span<const u8> data;
      usize pos = 0;
    };
    int gif_read_fn(GifFileType* gif, GifByteType* buf, int len) {
      auto* reader = static_cast<gif_reader*>(gif->UserData);
      const usize remaining = reader->data.size() - reader->pos;
      const usize n = std::min<usize>(static_cast<usize>(len), remaining);
      if (n == 0)
        return 0;
      std::memcpy(buf, reader->data.data() + reader->pos, n);
      reader->pos += n;
      return static_cast<int>(n);
    }

    [[nodiscard]] texture_data decode_gif_first_frame(std::span<const u8> encoded) {
      gif_reader reader{encoded, 0};
      int err = 0;
      GifFileType* gif = DGifOpen(&reader, &gif_read_fn, &err);
      if (!gif) {
        throw std::runtime_error(std::string("load_texture: DGifOpen failed: ") +
                                 GifErrorString(err));
      }
      struct gif_guard {
        GifFileType* gif;
        ~gif_guard() {
          if (gif) {
            int e = 0;
            DGifCloseFile(gif, &e);
          }
        }
      } guard{gif};

      if (DGifSlurp(gif) != GIF_OK) {
        throw std::runtime_error(std::string("load_texture: DGifSlurp failed: ") +
                                 GifErrorString(gif->Error));
      }
      if (gif->ImageCount <= 0)
        throw std::runtime_error("load_texture: GIF has no frames");

      const u32 cw = static_cast<u32>(gif->SWidth);
      const u32 ch = static_cast<u32>(gif->SHeight);
      const SavedImage& img = gif->SavedImages[0];
      const ColorMapObject* map = img.ImageDesc.ColorMap ? img.ImageDesc.ColorMap : gif->SColorMap;

      GraphicsControlBlock gcb{};
      gcb.TransparentColor = NO_TRANSPARENT_COLOR;
      DGifSavedExtensionToGCB(gif, 0, &gcb);
      const int trans = gcb.TransparentColor;

      const u32 fx = static_cast<u32>(img.ImageDesc.Left);
      const u32 fy = static_cast<u32>(img.ImageDesc.Top);
      const u32 fw = static_cast<u32>(img.ImageDesc.Width);
      const u32 fh = static_cast<u32>(img.ImageDesc.Height);
      if (fx > cw || fy > ch || fw > cw - fx || fh > ch - fy)
        throw std::runtime_error("load_texture: GIF frame exceeds canvas bounds");

      texture_data out;
      out.size = {cw, ch};
      out.pixels.assign(static_cast<usize>(cw) * ch, r8g8b8a8{});
      const GifByteType* raster = img.RasterBits;
      for (u32 y = 0; y < fh; ++y) {
        for (u32 x = 0; x < fw; ++x) {
          const u8 idx = raster[static_cast<usize>(y) * fw + x];
          if (trans >= 0 && idx == static_cast<u8>(trans))
            continue;
          if (!map || idx >= map->ColorCount)
            continue;
          const auto& c = map->Colors[idx];
          out.pixels[static_cast<usize>(fy + y) * cw + (fx + x)] = {c.Red, c.Green, c.Blue, 255};
        }
      }
      return out;
    }
  } // namespace

  texture_data load_texture(std::span<const u8> encoded) {
    if (encoded.empty())
      throw std::runtime_error("load_texture: empty input");
    if (is_png(encoded))
      return decode_png(encoded);
    if (is_jpeg(encoded))
      return decode_jpeg(encoded);
    if (is_webp(encoded))
      return decode_webp(encoded);
    if (is_gif(encoded))
      return decode_gif_first_frame(encoded);
    throw std::runtime_error("load_texture: unsupported image format");
  }

  texture_data load_texture_resized(std::span<const u8> encoded, math::uvec2 dst_size) {
    if (dst_size.x == 0 || dst_size.y == 0)
      throw std::runtime_error("load_texture_resized: zero destination size");
    texture_data src = load_texture(encoded);
    if (src.size.x == dst_size.x && src.size.y == dst_size.y)
      return src;
    texture_data dst;
    dst.size = dst_size;
    dst.pixels.resize(static_cast<usize>(dst_size.x) * dst_size.y);
#if FXE_HAS_STB
    stbir_resize_uint8_srgb(
        reinterpret_cast<const unsigned char*>(src.pixels.data()), static_cast<int>(src.size.x),
        static_cast<int>(src.size.y), 0, reinterpret_cast<unsigned char*>(dst.pixels.data()),
        static_cast<int>(dst_size.x), static_cast<int>(dst_size.y), 0, STBIR_RGBA);
#else
    for (u32 y = 0; y < dst_size.y; ++y) {
      const u32 sy = (y * src.size.y) / dst_size.y;
      for (u32 x = 0; x < dst_size.x; ++x) {
        const u32 sx = (x * src.size.x) / dst_size.x;
        dst.pixels[static_cast<usize>(y) * dst_size.x + x] =
            src.pixels[static_cast<usize>(sy) * src.size.x + sx];
      }
    }
#endif
    return dst;
  }
} // namespace fxe
