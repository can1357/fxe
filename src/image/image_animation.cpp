// Animated image decoding wired through libgif (GIF), libpng with the
// APNG patch (animated PNG), libwebp + libwebpdemux (animated WebP), and
// rlottie (Lottie JSON). Static (single-frame) WebPs are handled
// transparently by the libwebp animation decoder, which exposes them as
// a one-frame animation.

#include <fxe/image_animation.hpp>
#include <fxe/log.hpp>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gif_lib.h>
#include <png.h>
#include <rlottie.h>
#include <webp/decode.h>
#include <webp/demux.h>

namespace fxe {
  namespace {
    // APNG dispose / blend constants (mirror PNG_DISPOSE_OP_* / PNG_BLEND_OP_*).
    constexpr u8 kApngDisposeNone = 0;
    constexpr u8 kApngDisposeBackground = 1;
    constexpr u8 kApngDisposePrevious = 2;
    constexpr u8 kApngBlendSource = 0;
    constexpr u8 kApngBlendOver = 1;

    [[nodiscard]] bool starts_with(std::span<const u8> data, std::string_view prefix) noexcept {
      return data.size() >= prefix.size() &&
             std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
    }

    [[nodiscard]] bool is_gif(std::span<const u8> data) noexcept {
      return starts_with(data, "GIF87a") || starts_with(data, "GIF89a");
    }

    [[nodiscard]] bool is_png(std::span<const u8> data) noexcept {
      static constexpr u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      return data.size() >= sizeof(sig) && std::memcmp(data.data(), sig, sizeof(sig)) == 0;
    }

    [[nodiscard]] bool is_webp(std::span<const u8> data) noexcept {
      return data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 &&
             std::memcmp(data.data() + 8, "WEBP", 4) == 0;
    }

    [[nodiscard]] bool looks_like_json(std::span<const u8> data) noexcept {
      for (u8 byte : data) {
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')
          continue;
        return byte == '{' || byte == '[';
      }
      return false;
    }

    [[nodiscard]] u32 saturating_duration_sum(const std::vector<animated_frame>& frames) noexcept {
      u64 sum = 0;
      for (const auto& frame : frames)
        sum += frame.delay_ms;
      return static_cast<u32>(std::min<u64>(sum, std::numeric_limits<u32>::max()));
    }

    [[nodiscard]] u32 delay_ms_from_fraction(u16 numerator, u16 denominator) noexcept {
      const u32 den = denominator == 0 ? 100u : denominator;
      return static_cast<u32>((static_cast<u64>(numerator) * 1000u) / den);
    }

    void clear_region(std::vector<r8g8b8a8>& canvas, u32 canvas_width, u32 canvas_height, u32 x0,
                      u32 y0, u32 width, u32 height, r8g8b8a8 fill = {}) {
      const u32 x1 = std::min<u32>(canvas_width, x0 + width);
      const u32 y1 = std::min<u32>(canvas_height, y0 + height);
      for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x)
          canvas[static_cast<usize>(y) * canvas_width + x] = fill;
      }
    }

    [[nodiscard]] r8g8b8a8 alpha_over(r8g8b8a8 dst, r8g8b8a8 src) noexcept {
      if (src.a == 255 || dst.a == 0)
        return src;
      if (src.a == 0)
        return dst;
      const u32 out_a =
          static_cast<u32>(src.a) + ((static_cast<u32>(dst.a) * (255u - src.a) + 127u) / 255u);
      if (out_a == 0)
        return {};
      const auto blend = [&](u8 s, u8 d) -> u8 {
        const u32 sp = static_cast<u32>(s) * src.a;
        const u32 dp = static_cast<u32>(d) * dst.a;
        const u32 out_p = sp + ((dp * (255u - src.a) + 127u) / 255u);
        return static_cast<u8>((out_p + out_a / 2u) / out_a);
      };
      return {blend(src.r, dst.r), blend(src.g, dst.g), blend(src.b, dst.b),
              static_cast<u8>(out_a)};
    }

    // ------------------------------------------------------------------
    // GIF (giflib)
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

    [[nodiscard]] r8g8b8a8 gif_palette_color(const ColorMapObject* map, int index) noexcept {
      if (!map || index < 0 || index >= map->ColorCount)
        return {};
      const auto& c = map->Colors[index];
      return {c.Red, c.Green, c.Blue, 255};
    }

    [[nodiscard]] animated_image decode_gif(std::span<const u8> encoded) {
      gif_reader reader{encoded, 0};
      int err = 0;
      GifFileType* gif = DGifOpen(&reader, &gif_read_fn, &err);
      if (!gif) {
        throw std::runtime_error(std::string("EIMG_FORMAT: DGifOpen failed: ") +
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
        throw std::runtime_error(std::string("EIMG_FORMAT: DGifSlurp failed: ") +
                                 GifErrorString(gif->Error));
      }
      if (gif->ImageCount <= 0)
        throw std::runtime_error("EIMG_FORMAT: GIF has no frames");

      const u32 canvas_width = static_cast<u32>(gif->SWidth);
      const u32 canvas_height = static_cast<u32>(gif->SHeight);
      const usize pixel_count = static_cast<usize>(canvas_width) * canvas_height;
      if (pixel_count == 0)
        throw std::runtime_error("EIMG_FORMAT: GIF has zero canvas size");

      std::vector<r8g8b8a8> canvas(pixel_count, r8g8b8a8{});
      std::vector<r8g8b8a8> snapshot(pixel_count, r8g8b8a8{});

      animated_image out;
      out.format = animated_image_format::gif;
      out.frames.reserve(static_cast<usize>(gif->ImageCount));

      for (int i = 0; i < gif->ImageCount; ++i) {
        const SavedImage& img = gif->SavedImages[i];

        GraphicsControlBlock gcb{};
        gcb.DisposalMode = DISPOSAL_UNSPECIFIED;
        gcb.UserInputFlag = false;
        gcb.DelayTime = 0;
        gcb.TransparentColor = NO_TRANSPARENT_COLOR;
        DGifSavedExtensionToGCB(gif, i, &gcb);

        const ColorMapObject* map =
            img.ImageDesc.ColorMap ? img.ImageDesc.ColorMap : gif->SColorMap;
        const int trans = gcb.TransparentColor;
        const u32 fx = static_cast<u32>(img.ImageDesc.Left);
        const u32 fy = static_cast<u32>(img.ImageDesc.Top);
        const u32 fw = static_cast<u32>(img.ImageDesc.Width);
        const u32 fh = static_cast<u32>(img.ImageDesc.Height);
        if (fx > canvas_width || fy > canvas_height || fw > canvas_width - fx ||
            fh > canvas_height - fy) {
          throw std::runtime_error("EIMG_FORMAT: GIF frame exceeds canvas bounds");
        }

        snapshot = canvas;

        // giflib's RasterBits is row-major after de-interlacing.
        const GifByteType* raster = img.RasterBits;
        for (u32 y = 0; y < fh; ++y) {
          for (u32 x = 0; x < fw; ++x) {
            const u8 idx = raster[static_cast<usize>(y) * fw + x];
            if (trans >= 0 && idx == static_cast<u8>(trans))
              continue;
            canvas[static_cast<usize>(fy + y) * canvas_width + (fx + x)] =
                gif_palette_color(map, idx);
          }
        }

        animated_frame frame;
        frame.delay_ms = static_cast<u32>(gcb.DelayTime) * 10u;
        frame.image.size = {canvas_width, canvas_height};
        frame.image.pixels = canvas;
        out.frames.push_back(std::move(frame));

        switch (gcb.DisposalMode) {
        case DISPOSE_BACKGROUND: {
          const r8g8b8a8 bg =
              trans >= 0 ? r8g8b8a8{} : gif_palette_color(gif->SColorMap, gif->SBackGroundColor);
          clear_region(canvas, canvas_width, canvas_height, fx, fy, fw, fh, bg);
          break;
        }
        case DISPOSE_PREVIOUS:
          canvas = snapshot;
          break;
        case DISPOSE_DO_NOT:
        case DISPOSAL_UNSPECIFIED:
        default:
          break;
        }
      }

      out.duration_ms = saturating_duration_sum(out.frames);
      return out;
    }

    // ------------------------------------------------------------------
    // APNG (libpng + APNG patch)
    // ------------------------------------------------------------------
    struct png_reader {
      std::span<const u8> data;
      usize pos = 0;
      std::string error;
    };

    void png_read_fn(png_structp png, png_bytep buf, png_size_t len) {
      auto* reader = static_cast<png_reader*>(png_get_io_ptr(png));
      if (!reader || reader->pos + len > reader->data.size()) {
        png_error(png, "EIMG_FORMAT: truncated PNG stream");
      }
      std::memcpy(buf, reader->data.data() + reader->pos, len);
      reader->pos += len;
    }

    void png_error_fn(png_structp png, png_const_charp msg) {
      auto* reader = static_cast<png_reader*>(png_get_error_ptr(png));
      if (reader) {
        reader->error = std::string("EIMG_FORMAT: libpng error: ") + (msg ? msg : "");
      }
      std::longjmp(png_jmpbuf(png), 1);
    }

    void png_warn_fn(png_structp, png_const_charp) noexcept {}

    [[nodiscard]] animated_image decode_apng(std::span<const u8> encoded) {
#ifndef PNG_APNG_SUPPORTED
      (void)encoded;
      throw std::runtime_error("EIMG_FORMAT: libpng built without APNG support");
#else
      png_reader reader{encoded, 0, {}};
      png_structp png =
          png_create_read_struct(PNG_LIBPNG_VER_STRING, &reader, &png_error_fn, &png_warn_fn);
      if (!png)
        throw std::runtime_error("EIMG_FORMAT: png_create_read_struct failed");
      png_infop info = png_create_info_struct(png);
      if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        throw std::runtime_error("EIMG_FORMAT: png_create_info_struct failed");
      }
      struct png_guard {
        png_structp png;
        png_infop info;
        ~png_guard() {
          png_destroy_read_struct(&png, &info, nullptr);
        }
      } guard{png, info};

      if (setjmp(png_jmpbuf(png))) {
        throw std::runtime_error(reader.error.empty() ? "EIMG_FORMAT: PNG decode failed"
                                                      : reader.error);
      }

      png_set_read_fn(png, &reader, &png_read_fn);
      png_read_info(png, info);

      png_uint_32 canvas_width = 0;
      png_uint_32 canvas_height = 0;
      int bit_depth = 0;
      int color_type = 0;
      png_get_IHDR(png, info, &canvas_width, &canvas_height, &bit_depth, &color_type, nullptr,
                   nullptr, nullptr);

      // Coerce input into 8-bit RGBA.
      if (bit_depth == 16)
        png_set_strip_16(png);
      if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
      if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
      if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
      if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
      if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
          color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
      }
      png_read_update_info(png, info);

      if (!png_get_valid(png, info, PNG_INFO_acTL))
        throw std::runtime_error("EIMG_FORMAT: PNG is not animated");

      png_uint_32 num_frames = 0;
      png_uint_32 num_plays = 0;
      png_get_acTL(png, info, &num_frames, &num_plays);
      if (num_frames == 0)
        throw std::runtime_error("EIMG_FORMAT: APNG has no frames");

      const usize pixel_count = static_cast<usize>(canvas_width) * canvas_height;
      if (pixel_count == 0)
        throw std::runtime_error("EIMG_FORMAT: APNG has zero canvas size");

      std::vector<r8g8b8a8> canvas(pixel_count, r8g8b8a8{});
      std::vector<r8g8b8a8> snapshot = canvas;
      const bool first_hidden = png_get_first_frame_is_hidden(png, info) != 0;
      if (first_hidden) {
        // The IDAT default image is not part of the animation. Discard it.
        std::vector<r8g8b8a8> discard(pixel_count);
        std::vector<png_bytep> rows(canvas_height);
        for (png_uint_32 y = 0; y < canvas_height; ++y) {
          rows[y] =
              reinterpret_cast<png_bytep>(discard.data() + static_cast<usize>(y) * canvas_width);
        }
        png_read_frame_head(png, info);
        png_read_image(png, rows.data());
      }

      animated_image out;
      out.format = animated_image_format::apng;
      out.frames.reserve(num_frames);

      for (png_uint_32 f = 0; f < num_frames; ++f) {
        png_read_frame_head(png, info);
        png_uint_32 fw = canvas_width;
        png_uint_32 fh = canvas_height;
        png_uint_32 fx = 0;
        png_uint_32 fy = 0;
        png_uint_16 delay_num = 0;
        png_uint_16 delay_den = 0;
        png_byte dispose_op = kApngDisposeNone;
        png_byte blend_op = kApngBlendSource;
        if (png_get_valid(png, info, PNG_INFO_fcTL)) {
          png_get_next_frame_fcTL(png, info, &fw, &fh, &fx, &fy, &delay_num, &delay_den,
                                  &dispose_op, &blend_op);
        }
        if (fx > canvas_width || fy > canvas_height || fw > canvas_width - fx ||
            fh > canvas_height - fy) {
          throw std::runtime_error("EIMG_FORMAT: APNG frame exceeds canvas bounds");
        }
        if (fw == 0 || fh == 0)
          throw std::runtime_error("EIMG_FORMAT: APNG frame has zero size");

        std::vector<r8g8b8a8> frame_pixels(static_cast<usize>(fw) * fh);
        std::vector<png_bytep> rows(fh);
        for (png_uint_32 y = 0; y < fh; ++y) {
          rows[y] = reinterpret_cast<png_bytep>(frame_pixels.data() + static_cast<usize>(y) * fw);
        }
        png_read_image(png, rows.data());

        snapshot = canvas;
        if (blend_op == kApngBlendSource) {
          clear_region(canvas, canvas_width, canvas_height, fx, fy, fw, fh);
        }
        for (png_uint_32 y = 0; y < fh; ++y) {
          for (png_uint_32 x = 0; x < fw; ++x) {
            const auto& pixel = frame_pixels[static_cast<usize>(y) * fw + x];
            const usize dst = static_cast<usize>(fy + y) * canvas_width + (fx + x);
            if (blend_op == kApngBlendOver)
              canvas[dst] = alpha_over(canvas[dst], pixel);
            else
              canvas[dst] = pixel;
          }
        }

        animated_frame composed;
        composed.delay_ms = delay_ms_from_fraction(delay_num, delay_den);
        composed.image.size = {canvas_width, canvas_height};
        composed.image.pixels = canvas;
        out.frames.push_back(std::move(composed));

        switch (dispose_op) {
        case kApngDisposeBackground:
          clear_region(canvas, canvas_width, canvas_height, fx, fy, fw, fh);
          break;
        case kApngDisposePrevious:
          canvas = snapshot;
          break;
        case kApngDisposeNone:
        default:
          break;
        }
      }

      (void)num_plays;
      out.duration_ms = saturating_duration_sum(out.frames);
      return out;
#endif
    }

    // ------------------------------------------------------------------
    // WebP (libwebp + libwebpdemux)
    // ------------------------------------------------------------------
    [[nodiscard]] animated_image decode_webp(std::span<const u8> encoded) {
      WebPData data;
      data.bytes = encoded.data();
      data.size = encoded.size();

      WebPAnimDecoderOptions opts;
      if (!WebPAnimDecoderOptionsInit(&opts))
        throw std::runtime_error("EIMG_FORMAT: WebPAnimDecoderOptionsInit failed");
      opts.color_mode = MODE_RGBA;
      opts.use_threads = 0;

      WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &opts);
      if (!dec)
        throw std::runtime_error("EIMG_FORMAT: WebPAnimDecoderNew failed (not a valid WebP)");
      struct webp_guard {
        WebPAnimDecoder* dec;
        ~webp_guard() {
          if (dec)
            WebPAnimDecoderDelete(dec);
        }
      } guard{dec};

      WebPAnimInfo info;
      if (!WebPAnimDecoderGetInfo(dec, &info))
        throw std::runtime_error("EIMG_FORMAT: WebPAnimDecoderGetInfo failed");

      if (info.canvas_width == 0 || info.canvas_height == 0)
        throw std::runtime_error("EIMG_FORMAT: WebP has zero canvas size");

      const u32 canvas_width = info.canvas_width;
      const u32 canvas_height = info.canvas_height;
      const usize pixel_count = static_cast<usize>(canvas_width) * canvas_height;

      animated_image out;
      out.format = animated_image_format::webp;
      out.frames.reserve(info.frame_count);

      int prev_timestamp = 0;
      while (WebPAnimDecoderHasMoreFrames(dec)) {
        u8* buf = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp))
          throw std::runtime_error("EIMG_FORMAT: WebPAnimDecoderGetNext failed");
        const int delta = timestamp - prev_timestamp;
        prev_timestamp = timestamp;

        animated_frame frame;
        frame.delay_ms = delta > 0 ? static_cast<u32>(delta) : 0u;
        frame.image.size = {canvas_width, canvas_height};
        frame.image.pixels.resize(pixel_count);
        std::memcpy(frame.image.pixels.data(), buf, pixel_count * sizeof(r8g8b8a8));
        out.frames.push_back(std::move(frame));
      }

      if (out.frames.empty())
        throw std::runtime_error("EIMG_FORMAT: WebP has no frames");
      out.duration_ms = saturating_duration_sum(out.frames);
      return out;
    }
  } // namespace

  usize animated_image::frame_index_at(double time_ms) const noexcept {
    if (frames.empty())
      return 0;
    if (!std::isfinite(time_ms) || duration_ms == 0)
      return 0;
    double local = std::fmod(time_ms, static_cast<double>(duration_ms));
    if (local < 0)
      local += static_cast<double>(duration_ms);
    u32 elapsed = 0;
    for (usize i = 0; i < frames.size(); ++i) {
      elapsed += frames[i].delay_ms;
      if (local < static_cast<double>(elapsed))
        return i;
    }
    return frames.size() - 1;
  }

  animated_image load_lottie(std::span<const u8> json, std::string_view source_name) {
    if (!looks_like_json(json))
      throw std::runtime_error("EIMG_FORMAT: expected Lottie JSON");
    std::string source(reinterpret_cast<const char*>(json.data()), json.size());
    const std::string key = source_name.empty() ? std::string{} : std::string(source_name);
    auto animation = rlottie::Animation::loadFromData(source, key, /*resourcePath=*/{},
                                                      /*cachePolicy=*/false);
    if (!animation)
      throw std::runtime_error("EIMG_FORMAT: failed to parse Lottie JSON");

    size_t width = 0;
    size_t height = 0;
    animation->size(width, height);
    if (width == 0 || height == 0)
      throw std::runtime_error("EIMG_FORMAT: Lottie has zero canvas size");

    const size_t total_frames = animation->totalFrame();
    const double frame_rate = animation->frameRate();
    if (total_frames == 0 || !std::isfinite(frame_rate) || frame_rate <= 0.0)
      throw std::runtime_error("EIMG_FORMAT: Lottie has no frames");

    const u32 per_frame_delay_ms = static_cast<u32>(std::lround(1000.0 / frame_rate));
    const usize pixel_count = static_cast<usize>(width) * height;

    animated_image out;
    out.format = animated_image_format::lottie;
    out.frames.reserve(total_frames);

    // rlottie renders into ARGB32-premultiplied with native byte order, which
    // means BGRA bytes on little-endian. Convert each frame to our
    // unpremultiplied RGBA storage.
    std::vector<u32> scratch(pixel_count);
    for (size_t i = 0; i < total_frames; ++i) {
      std::fill(scratch.begin(), scratch.end(), 0u);
      rlottie::Surface surface(scratch.data(), width, height,
                               static_cast<size_t>(width) * sizeof(u32));
      animation->renderSync(i, surface);

      animated_frame frame;
      frame.delay_ms = per_frame_delay_ms;
      frame.image.size = {static_cast<u32>(width), static_cast<u32>(height)};
      frame.image.pixels.resize(pixel_count);
      for (usize p = 0; p < pixel_count; ++p) {
        const u32 argb = scratch[p];
        const u8 a = static_cast<u8>((argb >> 24) & 0xff);
        u8 r = static_cast<u8>((argb >> 16) & 0xff);
        u8 g = static_cast<u8>((argb >> 8) & 0xff);
        u8 b = static_cast<u8>(argb & 0xff);
        // Unpremultiply so the pixel matches the RGBA contract the rest of
        // the engine expects from load_texture / decode_gif / decode_webp.
        if (a > 0 && a < 255) {
          r = static_cast<u8>(std::min<u32>(255u, (static_cast<u32>(r) * 255u + a / 2u) / a));
          g = static_cast<u8>(std::min<u32>(255u, (static_cast<u32>(g) * 255u + a / 2u) / a));
          b = static_cast<u8>(std::min<u32>(255u, (static_cast<u32>(b) * 255u + a / 2u) / a));
        }
        frame.image.pixels[p] = {r, g, b, a};
      }
      out.frames.push_back(std::move(frame));
    }

    out.duration_ms = saturating_duration_sum(out.frames);
    return out;
  }

  animated_image load_animated_image(std::span<const u8> encoded, std::string_view source_name) {
    (void)source_name;
    if (encoded.empty())
      throw std::runtime_error("EIMG_FORMAT: empty input");
    if (is_gif(encoded))
      return decode_gif(encoded);
    if (is_webp(encoded))
      return decode_webp(encoded);
    if (is_png(encoded))
      return decode_apng(encoded);
    if (looks_like_json(encoded))
      return load_lottie(encoded, source_name);
    throw std::runtime_error("EIMG_FORMAT: unsupported animated image format");
  }
} // namespace fxe
