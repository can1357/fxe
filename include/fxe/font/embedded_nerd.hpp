#pragma once

// Embedded JetBrainsMono Nerd Font Mono (SIL OFL-1.1 license — see
// assets/fonts/JetBrainsMonoNerdFontMono-OFL.txt). The font ships compiled
// into `fxe_font` so apps get Nerd Font icons "for free" without having to
// install or bundle a separate file.
//
// The intended use is as a transparent codepoint-range fallback: when the
// active font lacks a glyph for a Nerd Font Private-Use-Area codepoint
// (powerline, devicons, font-awesome, material, octicons, …), the shaper
// cascades through this font and emits the icon at the same baseline as
// surrounding text. The mechanism is backend-specific:
//
//   - CoreText: the shaper bakes a `kCTFontCascadeListAttribute` containing
//     a descriptor pointing at this font on every shape() call, so the
//     CTLine cascade picks up icons automatically.
//   - HarfBuzz (FreeType-based backends): the shaper post-processes the
//     primary HB shape, detects tofu glyphs (`glyph_id == 0`) whose source
//     codepoint is in a Nerd Font range, and re-shapes that cluster with
//     the embedded face — emitting it as a separate ShapeRun with the
//     correct `face` pointer so the glyph cache lookup hits the right
//     atlas.
//   - Null backend: no fallback (raw bytes are still exposed for tests).

#include <cstddef>
#include <memory>
#include <span>

#include <fxe/types.hpp>

namespace fxe::font {
  class Face;

  // Raw TTF bytes for JetBrainsMono Nerd Font Mono Regular. Backed by a
  // static linked-in array; safe to take a pointer/span at any time, the
  // lifetime is whole-program.
  [[nodiscard]] std::span<const u8> embedded_nerd_font_bytes() noexcept;

  // Returns true if `cp` falls into any Nerd Font glyph block. The check is
  // a flat list of half-open ranges; ~ a dozen comparisons.
  [[nodiscard]] bool is_nerd_font_codepoint(char32_t cp) noexcept;

  // Shared, lazily constructed Face for the embedded font at the given
  // pixel size. Quantised to a 1/64-pixel grid so repeated lookups at the
  // same logical size return the same Face (and the same id() — important
  // for the glyph cache key).
  //
  // Returns nullptr only if the backend doesn't support loading from raw
  // bytes at all (e.g. the null backend).
  [[nodiscard]] std::shared_ptr<Face> embedded_nerd_font_face(float pixel_size);
} // namespace fxe::font
