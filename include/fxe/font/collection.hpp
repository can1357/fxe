#pragma once

// Style-keyed face list with per-style fallbacks. A Collection is the
// concrete face-resolution unit consumed by the text path: the codepoint
// resolver walks the entries for the active style and returns the first
// face that has a glyph for the codepoint.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <fxe/font/descriptor.hpp>
#include <fxe/font/face.hpp>

namespace fxe::font {

  // One entry in a collection: either a fully-loaded Face or a deferred
  // descriptor that hasn't been opened yet (loaded lazily on first use).
  struct CollectionEntry {
    std::shared_ptr<Face> face;
    Descriptor deferred;
    bool is_fallback = false;
  };

  class Collection {
  public:
    static constexpr std::size_t kStyleCount = 4;

    Collection() = default;

    // Registers a primary entry under `style`. Inserted at the front of the
    // style's list (i.e. preferred over later additions).
    void add_primary(Style style, std::shared_ptr<Face> face);
    void add_primary(Style style, Descriptor d);

    // Registers a fallback entry under `style`. Appended to the end of the
    // list and marked `is_fallback = true`.
    void add_fallback(Style style, std::shared_ptr<Face> face);
    void add_fallback(Style style, Descriptor d);

    // Returns the first entry under `style` whose face has a glyph for `cp`.
    // Resolves deferred entries on demand. Returns nullptr if no entry in
    // the style covers the codepoint.
    [[nodiscard]] std::shared_ptr<Face> resolve(Style style, char32_t cp);

    // Returns the primary face for `style`, loading the first deferred
    // entry if none are loaded. Used as the "default" face when no
    // codepoint-specific resolution is needed.
    [[nodiscard]] std::shared_ptr<Face> primary(Style style);

    [[nodiscard]] std::span<const CollectionEntry> entries(Style style) const noexcept;

  private:
    std::shared_ptr<Face> load_entry_(CollectionEntry& e);

    std::array<std::vector<CollectionEntry>, kStyleCount> by_style_{};
  };

} // namespace fxe::font
