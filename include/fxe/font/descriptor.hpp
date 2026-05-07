#pragma once

// Descriptor — declarative font search request, used both as a discovery
// query and as a stored "deferred face" entry in a Collection. Mirrors the
// shape Ghostty's `DeferredFace.zig` exposes.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fxe/font/face.hpp>

namespace fxe::font {

  struct Descriptor {
    // Family name, e.g. "Helvetica", "Apple Color Emoji".
    std::string family;
    // Optional style hint. Discovery may relax it to find the closest match.
    Style style = Style::regular;
    // Optional point size hint. Pure load_face callers pass this as the
    // pixel size; discovery returns a Descriptor with size carried through.
    std::optional<float> size_pt;
    // Optional weight in [1, 1000]; default 400 (regular), 700 (bold).
    std::optional<std::uint16_t> weight;
    // Optional codepoints the face MUST cover. Discovery uses this to pick
    // a fallback for color-emoji or scripts the primary doesn't cover.
    std::vector<char32_t> required_codepoints;
    // If true, the face must contain color glyphs (CBDT/sbix/COLR). Used to
    // surface emoji fallbacks.
    bool require_color = false;
    // Resolved on-disk path if a discovery backend already knows it. Lets
    // the descriptor double as a load handle.
    std::optional<std::string> path;
    std::uint32_t face_index = 0;

    // Returns true if the descriptor encodes enough to load a face directly
    // (path is set), false if it needs a discovery pass.
    [[nodiscard]] bool is_resolved() const noexcept {
      return path.has_value();
    }
  };

} // namespace fxe::font
