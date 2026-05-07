// RGBA8 → PNG/JPEG encoder + crop/resize helpers used by Page.screenshot.
// Wraps stb_image_write and stb_image_resize2.

#pragma once

#include <fxe/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fxe::debug {
  // Encodes width*height RGBA8 pixels (row-major, 4 bytes/px, row_stride_bytes
  // between rows) into a PNG byte stream. Returns empty string on failure and
  // writes a diagnostic to `err_out` when provided.
  std::string encode_png_rgba8(const u8* pixels, u32 width, u32 height, u32 row_stride_bytes,
                               std::string* err_out = nullptr);

  // Encodes width*height RGBA8 pixels into a JPEG byte stream. Alpha is dropped
  // (RGB only). `quality` is clamped to [1, 100]. Returns empty string on failure
  // and writes a diagnostic to `err_out` when provided.
  std::string encode_jpeg_rgba8(const u8* pixels, u32 width, u32 height, u32 row_stride_bytes,
                                int quality, std::string* err_out = nullptr);

  // Crop + resample an RGBA8 image. `cx,cy,cw,ch` defines the source rect in
  // pixel coordinates (clamped to the source); `out_w/out_h` is the desired
  // output size. Output is tightly packed (row stride = out_w*4).
  //
  // Returns true on success and fills `out_pixels`. When the source rect is
  // already the requested size, the buffer is filled with a straight copy.
  // Returns false on invalid input, an empty source rect, or when stb is
  // unavailable, and writes a diagnostic to `err_out` when provided.
  bool crop_resize_rgba8(const u8* pixels, u32 src_width, u32 src_height, u32 src_stride_bytes,
                         u32 cx, u32 cy, u32 cw, u32 ch, u32 out_w, u32 out_h,
                         std::vector<u8>& out_pixels, std::string* err_out = nullptr);
} // namespace fxe::debug
