// Spritesheet + default font glue. Phase 7 of FONT_STACK_OVERHAUL replaced
// the stb_truetype-based text path with the FreeType/CoreText + HarfBuzz
// font module. This file no longer rasterizes glyphs; `init_default_fonts`
// is now a thin wrapper that constructs a font::Face from system discovery
// and attaches it to the legacy `font_info` so existing callers work
// unchanged. The text path in primitives.cpp dispatches on
// `font_face_for(font)` and uses the font module for shaping + rendering.
//
// stb_image / stb_image_resize remain in use for image decode/resize.
// stb_truetype is gone.

#include <fxe/spritesheet.hpp>
#include <fxe/system_fonts.hpp>
#include <fxe/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <fxe/font.hpp>

#ifndef FXE_HAS_STB
#define FXE_HAS_STB 0
#endif

#if FXE_HAS_STB
#include <stb_image.h>
#include <stb_image_resize2.h>
#endif

namespace fxe {
  // Legacy-shape type kept around so callers compiling against the old
  // `font_info` continue to link. The interesting field is `face`; everything
  // else exists for source compat. Phase 9+ work may delete this struct
  // entirely once consumers migrate to font::Collection.
  struct font_runtime {
    texture_id texture = null_texture;
    // Default face (rasterised at the size requested by init_default_fonts).
    // Used as the cache miss fallback when source bytes are unavailable.
    std::shared_ptr<font::Face> face;
    // Original font bytes so we can re-load the face at a different pixel
    // size on demand (HiDPI rasterisation requested by the renderer at
    // pt × device_pixel_ratio). Empty when init_default_fonts couldn't
    // discover a system font.
    std::vector<u8> source_bytes;
    // (pixel_size_q = round(pt * 64)) → reusable face. Mutated under
    // `cache_mu` because the same font_info can be hit from multiple
    // threads in principle.
    mutable std::mutex cache_mu;
    mutable std::unordered_map<u32, std::shared_ptr<font::Face>> face_by_size;
  };

  font_info& fallback_font() {
    // Touch the FreeType library singleton first so its destructor is
    // registered before this `font_info`'s. The fallback owns a
    // `shared_ptr<font_runtime>` which calls FT_Done_Face on teardown;
    // if the library was destroyed first we'd segfault inside libharfbuzz.
    (void)font::shared_library();
    static font_info inst{};
    return inst;
  }

  texture_id spritesheet::add_texture(texture_data tex) {
    textures.push_back(std::move(tex));
    return static_cast<texture_id>(textures.size());
  }

  texture_id spritesheet::add_sprite(sprite s) {
    sprites.push_back(s);
    return static_cast<texture_id>(sprites.size());
  }

  texture_id spritesheet::resolve_if(texture_id id, float time_seconds) const {
    if ((id & asprite_flag) == 0)
      return id;
    auto index = (id & sprite_mask);
    if (index == 0 || index > asprites.size())
      return null_texture;
    const auto& anim = asprites[index - 1];
    if (anim.delays.empty())
      return anim.base_texture;
    float total = 0.0f;
    for (float d : anim.delays)
      total += d;
    if (total <= 0.0f)
      return anim.base_texture;
    float t = math::fmod(time_seconds, total);
    usize frame = 0;
    while (frame + 1 < anim.delays.size() && t > anim.delays[frame])
      t -= anim.delays[frame++];
    return anim.base_texture + static_cast<texture_id>(frame);
  }

  std::shared_ptr<font::Face> font_face_for(const font_info& font) {
    if (!font.runtime)
      return nullptr;
    return font.runtime->face;
  }

  std::shared_ptr<font::Face> font_face_for(const font_info& font, float pixel_size_px) {
    if (!font.runtime)
      return nullptr;
    if (!std::isfinite(pixel_size_px) || pixel_size_px <= 0.0f)
      return font.runtime->face;
    auto& rt = *font.runtime;
    const u32 key = static_cast<u32>(std::lround(pixel_size_px * 64.0f));
    {
      std::lock_guard<std::mutex> lock(rt.cache_mu);
      if (auto it = rt.face_by_size.find(key); it != rt.face_by_size.end())
        return it->second;
    }
    // Miss: load a fresh face from the original bytes at the requested size.
    // If we don't have source bytes (e.g. a face was attached externally),
    // fall back to the cached default face — text will look slightly
    // upsampled but won't crash.
    if (rt.source_bytes.empty())
      return rt.face;
    auto raw = font::load_face_from_bytes(rt.source_bytes, pixel_size_px);
    if (!raw)
      return rt.face;
    auto sp = std::shared_ptr<font::Face>(std::move(raw));
    std::lock_guard<std::mutex> lock(rt.cache_mu);
    rt.face_by_size.emplace(key, sp);
    return sp;
  }

  const font_info& get_font_info() {
    return fallback_font();
  }

  spritesheet& get_default_spritesheet() {
    static spritesheet g_default;
    static bool g_inited = false;
    if (!g_inited) {
      init_default_fonts(g_default);
      g_inited = true;
    }
    return g_default;
  }

  // --------------------------------------------------------------------------
  // Image decode + resize. These remain backed by stb_image / stb_image_resize
  // because they're tiny single-header libs and replacing them is
  // out-of-scope for FONT_STACK_OVERHAUL.
  // --------------------------------------------------------------------------
  texture_data load_texture(std::span<const u8> encoded_rgba) {
    if (encoded_rgba.empty())
      throw std::runtime_error("load_texture: empty input");
#if FXE_HAS_STB
    int w = 0, h = 0, ch = 0;
    stbi_uc* data = stbi_load_from_memory(
        encoded_rgba.data(), static_cast<int>(encoded_rgba.size()), &w, &h, &ch, STBI_rgb_alpha);
    if (!data) {
      const char* why = stbi_failure_reason();
      throw std::runtime_error(std::string("load_texture: stbi_load_from_memory failed: ") +
                               (why ? why : "unknown"));
    }
    texture_data out;
    out.size = {static_cast<u32>(w), static_cast<u32>(h)};
    const usize count = static_cast<usize>(w) * static_cast<usize>(h);
    out.pixels.resize(count);
    std::memcpy(out.pixels.data(), data, count * 4);
    stbi_image_free(data);
    return out;
#else
    if (encoded_rgba.size() % 4 != 0)
      throw std::runtime_error("load_texture: stb disabled; input must be raw RGBA bytes");
    texture_data out;
    out.size = {static_cast<u32>(encoded_rgba.size() / 4), 1};
    out.pixels.resize(encoded_rgba.size() / 4);
    for (usize i = 0; i != out.pixels.size(); ++i)
      out.pixels[i] = {encoded_rgba[i * 4], encoded_rgba[i * 4 + 1], encoded_rgba[i * 4 + 2],
                       encoded_rgba[i * 4 + 3]};
    return out;
#endif
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
    return dst;
#else
    for (u32 y = 0; y < dst_size.y; ++y) {
      u32 sy = (y * src.size.y) / dst_size.y;
      for (u32 x = 0; x < dst_size.x; ++x) {
        u32 sx = (x * src.size.x) / dst_size.x;
        dst.pixels[y * dst_size.x + x] = src.pixels[sy * src.size.x + sx];
      }
    }
    return dst;
#endif
  }

  // --------------------------------------------------------------------------
  // Legacy resolve_font_variant / resolve_font_glyph compat shims. Real text
  // rendering now goes through the font module; these only fire if a caller
  // hands draw_text a font_info that lacks a font::Face. They synthesise a
  // best-effort empty variant + missing glyph so callers don't crash.
  // --------------------------------------------------------------------------
  const font_variant_info& resolve_font_variant(const font_info& font, float pt) {
    thread_local font_variant_info v;
    v = font_variant_info{};
    v.pixel_height = pt > 0.0f ? pt : (font.pixel_height > 0.0f ? font.pixel_height : 16.0f);
    v.ascent = v.pixel_height;
    v.line_gap = font.line_gap;
    v.line_height = v.ascent + v.line_gap;
    v.texture = font.texture;
    return v;
  }

  glyph_info resolve_font_glyph(const font_info& font, const font_variant_info& variant,
                                char32_t /*codepoint*/) {
    glyph_info g{};
    g.advance = variant.pixel_height * 0.5f;
    g.tx = font.texture;
    return g;
  }

  // --------------------------------------------------------------------------
  // init_default_fonts. Loads a system font via fxe::font::default_discover
  // (or the caller-supplied bytes), constructs a font::Face, and wraps it in
  // a `font_info`. Always adds one placeholder atlas texture to the
  // spritesheet so legacy code that introspects `sheet.textures.size()` after
  // init keeps working. The placeholder is a 1x1 transparent pixel and is
  // never sampled by the renderer (the new text path uses font::shared_glyph_cache).
  // --------------------------------------------------------------------------
  namespace {
    constexpr float kDefaultFontPixelHeight = 16.0f;

    [[nodiscard]] float clean_pt(float pt) noexcept {
      if (!std::isfinite(pt) || pt <= 0.0f)
        return kDefaultFontPixelHeight;
      return pt;
    }

    void warn_no_face_once() {
      try {
        static std::once_flag once;
        std::call_once(once, [] {
          std::fprintf(stderr,
                       "fxe::font: discovery + system font load failed; draw_text is a no-op\n");
        });
      } catch (...) {
      }
    }

    // Allocate a 1×1 transparent placeholder texture so the spritesheet's
    // texture index advances. Returns the texture_id with msprite_flag set
    // for source-compat with callers reading `default_font.texture`.
    [[nodiscard]] texture_id reserve_font_placeholder(spritesheet& sheet) {
      texture_data atlas;
      atlas.size = {1, 1};
      atlas.pixels = {r8g8b8a8{0, 0, 0, 0}};
      const texture_id tex = sheet.add_texture(std::move(atlas));
      return tex | msprite_flag;
    }

    void build_default_font(spritesheet& sheet, font_info& out, std::shared_ptr<font::Face> face,
                            std::vector<u8> source_bytes) {
      out = font_info{};
      out.runtime = std::make_shared<font_runtime>();
      out.runtime->texture = reserve_font_placeholder(sheet);
      out.texture = out.runtime->texture;
      out.runtime->face = std::move(face);
      out.runtime->source_bytes = std::move(source_bytes);
      if (out.runtime->face) {
        const auto m = out.runtime->face->metrics();
        out.pixel_height = out.runtime->face->pixel_size();
        out.line_gap = m.line_gap;
        // Seed the size-keyed face cache with the default size so identity
        // lookups (pt = pixel_height) skip the load_face round-trip.
        const u32 key = static_cast<u32>(std::lround(out.runtime->face->pixel_size() * 64.0f));
        out.runtime->face_by_size.emplace(key, out.runtime->face);
      } else {
        out.pixel_height = kDefaultFontPixelHeight;
        out.line_gap = 4.0f;
        warn_no_face_once();
      }
    }
  } // namespace

  void init_default_fonts(spritesheet& sheet) {
    std::shared_ptr<font::Face> face;
    std::vector<u8> bytes_storage;
    if (auto bytes = load_default_system_font(); bytes && !bytes->empty()) {
      auto raw = font::load_face_from_bytes(*bytes, kDefaultFontPixelHeight);
      if (raw)
        face = std::shared_ptr<font::Face>(std::move(raw));
      bytes_storage = std::move(*bytes);
    }
    build_default_font(sheet, sheet.default_font, std::move(face), std::move(bytes_storage));
    fallback_font() = sheet.default_font;
  }

  void init_default_fonts(spritesheet& sheet, std::span<const u8> ttf_bytes) {
    init_default_fonts(sheet, ttf_bytes, kDefaultFontPixelHeight);
  }

  void init_default_fonts(spritesheet& sheet, std::span<const u8> ttf_bytes, float pixel_height) {
    const float pt = clean_pt(pixel_height);
    std::shared_ptr<font::Face> face;
    std::vector<u8> bytes_storage;
    if (!ttf_bytes.empty()) {
      bytes_storage.assign(ttf_bytes.begin(), ttf_bytes.end());
      auto raw = font::load_face_from_bytes(bytes_storage, pt);
      if (raw)
        face = std::shared_ptr<font::Face>(std::move(raw));
    } else if (auto bytes = load_default_system_font(); bytes && !bytes->empty()) {
      auto raw = font::load_face_from_bytes(*bytes, pt);
      if (raw)
        face = std::shared_ptr<font::Face>(std::move(raw));
      bytes_storage = std::move(*bytes);
    }
    build_default_font(sheet, sheet.default_font, std::move(face), std::move(bytes_storage));
    fallback_font() = sheet.default_font;
  }
} // namespace fxe
