// Embedded JetBrainsMono Nerd Font Mono accessor.
//
// The byte array itself lives in a generated translation unit emitted by
// scripts/embed_binary.py at configure time (target `fxe_font` adds it as a
// private source). This file just wraps the raw symbol in a typed span and
// owns the per-pixel-size Face cache used by the shaper cascade.

#include <fxe/font/embedded_nerd.hpp>
#include <fxe/font/face.hpp>

#include <cmath>
#include <mutex>
#include <unordered_map>

// Linked in from the auto-generated jetbrains_nerd_font.cpp.
namespace fxe::font::embedded {
  extern const unsigned char jetbrains_nerd_font_ttf[];
  extern const unsigned long long jetbrains_nerd_font_ttf_size;
} // namespace fxe::font::embedded

namespace fxe::font {
  namespace {
    // Nerd Font glyph blocks (v3.x). Sourced from the upstream Nerd Fonts
    // i_*.sh helper script ranges plus a few extra PUA blocks the patcher
    // uses for Material Design Icons and Codicons. Each entry is half-open
    // `[lo, hi)`. Kept inline so `is_nerd_font_codepoint` is a tight linear
    // scan — short enough to beat any hash structure overhead.
    struct Range {
      char32_t lo;
      char32_t hi;
    };

    constexpr Range kNerdRanges[] = {
        {0x23fb, 0x23ff + 1},   // IEC Power Symbols
        {0x2665, 0x2665 + 1},   // Octicons heart
        {0x26a1, 0x26a1 + 1},   // Octicons zap
        {0x2b58, 0x2b58 + 1},   // Power, Sleep
        {0xe000, 0xe00a + 1},   // Pomicons
        {0xe0a0, 0xe0a3 + 1},   // Powerline
        {0xe0b0, 0xe0d4 + 1},   // Powerline Extra
        {0xe200, 0xe2a9 + 1},   // Font Awesome Extension
        {0xe300, 0xe3e3 + 1},   // Weather
        {0xe5fa, 0xe6b7 + 1},   // Seti-UI + Custom
        {0xe700, 0xe8ef + 1},   // Devicons
        {0xea60, 0xec1e + 1},   // Codicons
        {0xed00, 0xefce + 1},   // Font Awesome 6
        {0xf000, 0xf2ff + 1},   // Font Awesome 4
        {0xf300, 0xf381 + 1},   // Font Logos
        {0xf400, 0xf533 + 1},   // Octicons (post-relocation)
        {0xf500, 0xfd46 + 1},   // Material Design Icons (legacy block)
        {0xf0001, 0xf1af0 + 1}, // Material Design Icons (post-v6 PUA block)
    };

    [[nodiscard]] u32 quantise(float pixel_size) noexcept {
      if (!std::isfinite(pixel_size) || pixel_size <= 0.0f)
        return 0;
      return static_cast<u32>(std::lround(pixel_size * 64.0f));
    }

    struct FaceCache {
      std::mutex mu;
      std::unordered_map<u32, std::shared_ptr<Face>> by_size;
    };

    FaceCache& face_cache() {
      static FaceCache c;
      return c;
    }
  } // namespace

  std::span<const u8> embedded_nerd_font_bytes() noexcept {
    return std::span<const u8>(reinterpret_cast<const u8*>(embedded::jetbrains_nerd_font_ttf),
                               static_cast<usize>(embedded::jetbrains_nerd_font_ttf_size));
  }

  bool is_nerd_font_codepoint(char32_t cp) noexcept {
    for (const auto& r : kNerdRanges) {
      if (cp >= r.lo && cp < r.hi)
        return true;
    }
    return false;
  }

  std::shared_ptr<Face> embedded_nerd_font_face(float pixel_size) {
    const u32 key = quantise(pixel_size);
    if (key == 0)
      return nullptr;

    auto& cache = face_cache();
    {
      std::lock_guard<std::mutex> lock(cache.mu);
      if (auto it = cache.by_size.find(key); it != cache.by_size.end())
        return it->second;
    }

    auto raw = load_face_from_bytes(embedded_nerd_font_bytes(), pixel_size, 0);
    if (!raw)
      return nullptr;

    std::shared_ptr<Face> sp = std::move(raw);
    std::lock_guard<std::mutex> lock(cache.mu);
    // Re-check under the lock in case a parallel caller raced us.
    if (auto it = cache.by_size.find(key); it != cache.by_size.end())
      return it->second;
    cache.by_size.emplace(key, sp);
    return sp;
  }
} // namespace fxe::font
