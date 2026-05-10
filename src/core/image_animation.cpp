// TODO(image-animation): replace the Lottie placeholder path with rlottie-backed
// decode + rasterization once rlottie is wired into the build.

#include <fxe/image_animation.hpp>

#include <fxe/log.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gif_load/gif_load.h"

namespace fxe {
  namespace {
    constexpr std::array<u8, 8> kPngSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    constexpr u8 kApngDisposeNone = 0;
    constexpr u8 kApngDisposeBackground = 1;
    constexpr u8 kApngDisposePrevious = 2;
    constexpr u8 kApngBlendSource = 0;
    constexpr u8 kApngBlendOver = 1;

    [[nodiscard]] u32 read_be_u32(std::span<const u8> data, usize offset) {
      if (offset + 4 > data.size())
        throw std::runtime_error("EIMG_FORMAT: truncated PNG chunk");
      return (static_cast<u32>(data[offset]) << 24) | (static_cast<u32>(data[offset + 1]) << 16) |
             (static_cast<u32>(data[offset + 2]) << 8) | static_cast<u32>(data[offset + 3]);
    }

    [[nodiscard]] u16 read_be_u16(std::span<const u8> data, usize offset) {
      if (offset + 2 > data.size())
        throw std::runtime_error("EIMG_FORMAT: truncated APNG frame control");
      return static_cast<u16>((static_cast<u16>(data[offset]) << 8) |
                              static_cast<u16>(data[offset + 1]));
    }

    void write_be_u32(std::vector<u8>& out, u32 value) {
      out.push_back(static_cast<u8>((value >> 24) & 0xff));
      out.push_back(static_cast<u8>((value >> 16) & 0xff));
      out.push_back(static_cast<u8>((value >> 8) & 0xff));
      out.push_back(static_cast<u8>(value & 0xff));
    }

    [[nodiscard]] u32 png_crc32(std::span<const u8> data) noexcept {
      static const u32 table[256] = {
          0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u,
          0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu,
          0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du,
          0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u, 0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu,
          0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u,
          0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
          0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u, 0x26d930acu,
          0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
          0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu,
          0xb6662d3du, 0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu,
          0x9fbfe4a5u, 0xe8b8d433u, 0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu,
          0x086d3d2du, 0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
          0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau,
          0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u, 0x4db26158u, 0x3ab551ceu,
          0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au,
          0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
          0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u,
          0xce61e49fu, 0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
          0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u,
          0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u,
          0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u, 0xf00f9344u, 0x8708a3d2u, 0x1e01f268u,
          0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u,
          0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u,
          0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
          0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu,
          0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u,
          0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u,
          0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du, 0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au,
          0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu,
          0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
          0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u, 0x88085ae6u,
          0xff0f6a70u, 0x66063bceu, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
          0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du,
          0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u,
          0x47b2cf7fu, 0x30b5ffe9u, 0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u,
          0xcdd70693u, 0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
          0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du};
      u32 crc = 0xffffffffu;
      for (u8 byte : data)
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
      return crc ^ 0xffffffffu;
    }

    void append_png_chunk(std::vector<u8>& out, std::string_view type, std::span<const u8> data) {
      write_be_u32(out, static_cast<u32>(data.size()));
      const usize type_offset = out.size();
      out.insert(out.end(), type.begin(), type.end());
      out.insert(out.end(), data.begin(), data.end());
      const u32 crc =
          png_crc32(std::span<const u8>(out).subspan(type_offset, type.size() + data.size()));
      write_be_u32(out, crc);
    }

    [[nodiscard]] bool is_png(std::span<const u8> encoded) noexcept {
      return encoded.size() >= kPngSignature.size() &&
             std::equal(kPngSignature.begin(), kPngSignature.end(), encoded.begin());
    }

    [[nodiscard]] bool looks_like_json(std::span<const u8> encoded) noexcept {
      for (u8 byte : encoded) {
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')
          continue;
        return byte == '{' || byte == '[';
      }
      return false;
    }

    [[nodiscard]] bool is_gif(std::span<const u8> encoded) noexcept {
      return encoded.size() >= 4 && encoded[0] == 'G' && encoded[1] == 'I' && encoded[2] == 'F' &&
             encoded[3] == '8';
    }

    [[nodiscard]] bool has_apng_actl(std::span<const u8> encoded) {
      if (!is_png(encoded))
        return false;
      usize offset = kPngSignature.size();
      while (offset + 12 <= encoded.size()) {
        const u32 length = read_be_u32(encoded, offset);
        offset += 4;
        if (offset + 4 + length + 4 > encoded.size())
          return false;
        const std::string_view type(reinterpret_cast<const char*>(encoded.data() + offset), 4);
        offset += 4 + length + 4;
        if (type == "acTL")
          return true;
        if (type == "IEND")
          return false;
      }
      return false;
    }

    [[nodiscard]] u32 delay_ms_from_fraction(u16 numerator, u16 denominator) noexcept {
      const u32 den = denominator == 0 ? 100u : denominator;
      return static_cast<u32>((static_cast<u64>(numerator) * 1000u) / den);
    }

    [[nodiscard]] u32 saturating_duration_sum(const std::vector<animated_frame>& frames) noexcept {
      u64 sum = 0;
      for (const auto& frame : frames)
        sum += frame.delay_ms;
      return static_cast<u32>(std::min<u64>(sum, std::numeric_limits<u32>::max()));
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

    struct gif_decode_state {
      std::vector<animated_frame> frames;
      std::vector<r8g8b8a8> canvas;
      std::vector<r8g8b8a8> snapshot;
      std::string error;
    };

    [[nodiscard]] r8g8b8a8 gif_palette_color(const GIF_WHDR& whdr, long index) noexcept {
      if (index < 0 || index >= whdr.clrs || whdr.cpal == nullptr)
        return {};
      const auto& entry = whdr.cpal[index];
      return {entry.R, entry.G, entry.B, 255};
    }

    [[nodiscard]] r8g8b8a8 gif_clear_color(const GIF_WHDR& whdr) noexcept {
      return whdr.tran >= 0 ? r8g8b8a8{} : gif_palette_color(whdr, whdr.bkgd);
    }

    void gif_write_frame(void* user, GIF_WHDR* whdr) {
      auto& state = *static_cast<gif_decode_state*>(user);
      if (!state.error.empty())
        return;
      const u32 canvas_width = static_cast<u32>(whdr->xdim);
      const u32 canvas_height = static_cast<u32>(whdr->ydim);
      const usize pixel_count = static_cast<usize>(canvas_width) * canvas_height;
      if (state.canvas.empty()) {
        state.canvas.assign(pixel_count, gif_clear_color(*whdr));
        state.snapshot.assign(pixel_count, {});
      }
      state.snapshot = state.canvas;

      const u32 frame_width = static_cast<u32>(whdr->frxd);
      const u32 frame_height = static_cast<u32>(whdr->fryd);
      const u32 offset_x = static_cast<u32>(whdr->frxo);
      const u32 offset_y = static_cast<u32>(whdr->fryo);
      if (offset_x > canvas_width || offset_y > canvas_height ||
          frame_width > canvas_width - offset_x || frame_height > canvas_height - offset_y) {
        state.error = "EIMG_FORMAT: GIF frame exceeds canvas bounds";
        return;
      }

      long source_index = -1;
      const long final_pass = whdr->intr ? 4 : 5;
      for (long pass = whdr->intr ? 0 : 4; pass < final_pass; ++pass) {
        const u32 y_step = static_cast<u32>(16u >> ((pass > 1) ? pass : 1));
        const u32 start_y = static_cast<u32>((8u >> pass) & 7u);
        for (u32 y = start_y; y < frame_height; y += y_step) {
          for (u32 x = 0; x < frame_width; ++x) {
            const u8 palette_index = whdr->bptr[++source_index];
            if (whdr->tran >= 0 && palette_index == static_cast<u8>(whdr->tran))
              continue;
            const usize dst = static_cast<usize>(offset_y + y) * canvas_width + (offset_x + x);
            state.canvas[dst] = gif_palette_color(*whdr, palette_index);
          }
        }
      }

      animated_frame frame;
      frame.delay_ms = static_cast<u32>((whdr->time < 0 ? (-whdr->time - 1) : whdr->time) * 10);
      frame.image.size = {canvas_width, canvas_height};
      frame.image.pixels = state.canvas;
      state.frames.push_back(std::move(frame));

      switch (whdr->mode) {
      case GIF_BKGD:
        clear_region(state.canvas, canvas_width, canvas_height, offset_x, offset_y, frame_width,
                     frame_height, gif_clear_color(*whdr));
        break;
      case GIF_PREV:
        state.canvas = state.snapshot;
        break;
      case GIF_NONE:
      case GIF_CURR:
      default:
        break;
      }
    }

    [[nodiscard]] animated_image decode_gif(std::span<const u8> encoded) {
      gif_decode_state state;
      const long loaded =
          GIF_Load(const_cast<u8*>(encoded.data()), static_cast<long>(encoded.size()),
                   gif_write_frame, nullptr, &state, 0);
      if (!state.error.empty())
        throw std::runtime_error(state.error);
      if (loaded <= 0 || state.frames.empty())
        throw std::runtime_error("EIMG_FORMAT: failed to decode animated GIF");
      animated_image out;
      out.format = animated_image_format::gif;
      out.frames = std::move(state.frames);
      out.duration_ms = saturating_duration_sum(out.frames);
      return out;
    }

    struct png_chunk_copy {
      std::array<char, 4> type{};
      std::vector<u8> data;
    };

    struct apng_frame_control {
      u32 width = 0;
      u32 height = 0;
      u32 x_offset = 0;
      u32 y_offset = 0;
      u16 delay_num = 0;
      u16 delay_den = 0;
      u8 dispose_op = 0;
      u8 blend_op = 0;
    };

    struct apng_raw_frame {
      apng_frame_control control;
      std::vector<std::vector<u8>> idat_chunks;
    };

    [[nodiscard]] apng_frame_control parse_fctl(std::span<const u8> data) {
      if (data.size() != 26)
        throw std::runtime_error("EIMG_FORMAT: malformed APNG fcTL chunk");
      return {
          .width = read_be_u32(data, 0),
          .height = read_be_u32(data, 4),
          .x_offset = read_be_u32(data, 8),
          .y_offset = read_be_u32(data, 12),
          .delay_num = read_be_u16(data, 16),
          .delay_den = read_be_u16(data, 18),
          .dispose_op = data[20],
          .blend_op = data[21],
      };
    }

    [[nodiscard]] texture_data
    decode_apng_frame_png(const std::vector<u8>& ihdr,
                          const std::vector<png_chunk_copy>& shared_chunks,
                          const apng_raw_frame& frame) {
      if (ihdr.size() != 13)
        throw std::runtime_error("EIMG_FORMAT: malformed PNG IHDR chunk");
      std::vector<u8> png;
      png.insert(png.end(), kPngSignature.begin(), kPngSignature.end());
      std::vector<u8> ihdr_frame = ihdr;
      ihdr_frame[0] = static_cast<u8>((frame.control.width >> 24) & 0xff);
      ihdr_frame[1] = static_cast<u8>((frame.control.width >> 16) & 0xff);
      ihdr_frame[2] = static_cast<u8>((frame.control.width >> 8) & 0xff);
      ihdr_frame[3] = static_cast<u8>(frame.control.width & 0xff);
      ihdr_frame[4] = static_cast<u8>((frame.control.height >> 24) & 0xff);
      ihdr_frame[5] = static_cast<u8>((frame.control.height >> 16) & 0xff);
      ihdr_frame[6] = static_cast<u8>((frame.control.height >> 8) & 0xff);
      ihdr_frame[7] = static_cast<u8>(frame.control.height & 0xff);
      append_png_chunk(png, "IHDR", ihdr_frame);
      for (const auto& chunk : shared_chunks)
        append_png_chunk(png, std::string_view(chunk.type.data(), chunk.type.size()), chunk.data);
      for (const auto& idat : frame.idat_chunks)
        append_png_chunk(png, "IDAT", idat);
      append_png_chunk(png, "IEND", {});
      texture_data decoded = load_texture(png);
      if (decoded.size.x != frame.control.width || decoded.size.y != frame.control.height)
        throw std::runtime_error("EIMG_FORMAT: APNG frame size mismatch");
      return decoded;
    }

    [[nodiscard]] animated_image decode_apng(std::span<const u8> encoded) {
      if (!is_png(encoded))
        throw std::runtime_error("EIMG_FORMAT: invalid PNG signature");

      std::vector<u8> ihdr;
      std::vector<png_chunk_copy> shared_chunks;
      std::vector<apng_raw_frame> raw_frames;
      u32 canvas_width = 0;
      u32 canvas_height = 0;
      bool seen_actl = false;
      bool seen_ihdr = false;
      bool collecting_shared_chunks = true;
      usize offset = kPngSignature.size();

      while (offset + 12 <= encoded.size()) {
        const u32 length = read_be_u32(encoded, offset);
        offset += 4;
        if (offset + 4 + length + 4 > encoded.size())
          throw std::runtime_error("EIMG_FORMAT: truncated PNG chunk stream");
        const char* type_ptr = reinterpret_cast<const char*>(encoded.data() + offset);
        const std::string_view type(type_ptr, 4);
        offset += 4;
        const std::span<const u8> data = encoded.subspan(offset, length);
        offset += length + 4;

        if (type == "IHDR") {
          ihdr.assign(data.begin(), data.end());
          if (ihdr.size() != 13)
            throw std::runtime_error("EIMG_FORMAT: malformed IHDR chunk");
          canvas_width = read_be_u32(ihdr, 0);
          canvas_height = read_be_u32(ihdr, 4);
          seen_ihdr = true;
          continue;
        }
        if (!seen_ihdr)
          throw std::runtime_error("EIMG_FORMAT: PNG missing IHDR before data chunks");
        if (type == "acTL") {
          seen_actl = true;
          continue;
        }
        if (type == "fcTL") {
          raw_frames.push_back({parse_fctl(data), {}});
          continue;
        }
        if (type == "IDAT") {
          collecting_shared_chunks = false;
          if (raw_frames.empty()) {
            raw_frames.push_back({{.width = canvas_width,
                                   .height = canvas_height,
                                   .x_offset = 0,
                                   .y_offset = 0,
                                   .delay_num = 0,
                                   .delay_den = 100,
                                   .dispose_op = kApngDisposeNone,
                                   .blend_op = kApngBlendSource},
                                  {}});
          }
          raw_frames.back().idat_chunks.emplace_back(data.begin(), data.end());
          continue;
        }
        if (type == "fdAT") {
          collecting_shared_chunks = false;
          if (raw_frames.empty() || data.size() < 4)
            throw std::runtime_error("EIMG_FORMAT: malformed APNG fdAT chunk");
          raw_frames.back().idat_chunks.emplace_back(data.begin() + 4, data.end());
          continue;
        }
        if (type == "IEND")
          break;
        if (collecting_shared_chunks && type != "tIME")
          shared_chunks.push_back(
              {{type[0], type[1], type[2], type[3]}, std::vector<u8>(data.begin(), data.end())});
      }

      if (!seen_actl || raw_frames.empty())
        throw std::runtime_error("EIMG_FORMAT: PNG is not animated");

      animated_image out;
      out.format = animated_image_format::apng;
      std::vector<r8g8b8a8> canvas(static_cast<usize>(canvas_width) * canvas_height);
      std::vector<r8g8b8a8> snapshot = canvas;

      for (const auto& raw_frame : raw_frames) {
        if (raw_frame.idat_chunks.empty())
          throw std::runtime_error("EIMG_FORMAT: APNG frame missing image data");
        if (raw_frame.control.width == 0 || raw_frame.control.height == 0)
          throw std::runtime_error("EIMG_FORMAT: APNG frame has zero size");
        if (raw_frame.control.x_offset > canvas_width ||
            raw_frame.control.y_offset > canvas_height ||
            raw_frame.control.width > canvas_width - raw_frame.control.x_offset ||
            raw_frame.control.height > canvas_height - raw_frame.control.y_offset) {
          throw std::runtime_error("EIMG_FORMAT: APNG frame exceeds canvas bounds");
        }

        snapshot = canvas;
        texture_data frame_tex = decode_apng_frame_png(ihdr, shared_chunks, raw_frame);
        if (raw_frame.control.blend_op == kApngBlendSource) {
          clear_region(canvas, canvas_width, canvas_height, raw_frame.control.x_offset,
                       raw_frame.control.y_offset, raw_frame.control.width,
                       raw_frame.control.height);
        }
        for (u32 y = 0; y < raw_frame.control.height; ++y) {
          for (u32 x = 0; x < raw_frame.control.width; ++x) {
            const usize src = static_cast<usize>(y) * raw_frame.control.width + x;
            const usize dst = static_cast<usize>(raw_frame.control.y_offset + y) * canvas_width +
                              (raw_frame.control.x_offset + x);
            const auto& pixel = frame_tex.pixels[src];
            if (raw_frame.control.blend_op == kApngBlendOver)
              canvas[dst] = alpha_over(canvas[dst], pixel);
            else
              canvas[dst] = pixel;
          }
        }

        animated_frame composed;
        composed.delay_ms =
            delay_ms_from_fraction(raw_frame.control.delay_num, raw_frame.control.delay_den);
        composed.image.size = {canvas_width, canvas_height};
        composed.image.pixels = canvas;
        out.frames.push_back(std::move(composed));

        switch (raw_frame.control.dispose_op) {
        case kApngDisposeBackground:
          clear_region(canvas, canvas_width, canvas_height, raw_frame.control.x_offset,
                       raw_frame.control.y_offset, raw_frame.control.width,
                       raw_frame.control.height);
          break;
        case kApngDisposePrevious:
          canvas = snapshot;
          break;
        case kApngDisposeNone:
        default:
          break;
        }
      }

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

  animated_image load_lottie_placeholder(std::span<const u8> json, std::string_view) {
    if (!looks_like_json(json))
      throw std::runtime_error("EIMG_FORMAT: expected Lottie JSON");
    static std::once_flag warned_once;
    std::call_once(warned_once, [] {
      FXE_WARN("image.lottie", "rlottie integration not yet built; static frame returned");
    });
    animated_image out;
    out.format = animated_image_format::lottie;
    out.frames.push_back(animated_frame{
        .delay_ms = 0, .image = texture_data{.size = {1, 1}, .pixels = {r8g8b8a8{0, 0, 0, 0}}}});
    out.duration_ms = 0;
    return out;
  }

  animated_image load_animated_image(std::span<const u8> encoded, std::string_view source_name) {
    (void)source_name;
    if (encoded.empty())
      throw std::runtime_error("EIMG_FORMAT: empty input");
    if (is_gif(encoded))
      return decode_gif(encoded);
    if (is_png(encoded) && has_apng_actl(encoded))
      return decode_apng(encoded);
    if (looks_like_json(encoded))
      return load_lottie_placeholder(encoded, source_name);
    throw std::runtime_error("EIMG_FORMAT: unsupported animated image format");
  }
} // namespace fxe
