// Collection — style-keyed face list with per-style fallbacks. The codepoint
// resolver walks the entries for the active style and returns the first
// face that has a glyph for the codepoint. Deferred entries (descriptor
// without a loaded face) are loaded on first use.

#include <fxe/font/collection.hpp>
#include <fxe/font/face.hpp>

#include <utility>

namespace fxe::font {

  void Collection::add_primary(Style style, std::shared_ptr<Face> face) {
    auto& bucket = by_style_[static_cast<std::size_t>(style)];
    bucket.insert(bucket.begin(), CollectionEntry{std::move(face), Descriptor{}, false});
  }

  void Collection::add_primary(Style style, Descriptor d) {
    auto& bucket = by_style_[static_cast<std::size_t>(style)];
    bucket.insert(bucket.begin(), CollectionEntry{nullptr, std::move(d), false});
  }

  void Collection::add_fallback(Style style, std::shared_ptr<Face> face) {
    auto& bucket = by_style_[static_cast<std::size_t>(style)];
    bucket.push_back(CollectionEntry{std::move(face), Descriptor{}, true});
  }

  void Collection::add_fallback(Style style, Descriptor d) {
    auto& bucket = by_style_[static_cast<std::size_t>(style)];
    bucket.push_back(CollectionEntry{nullptr, std::move(d), true});
  }

  std::shared_ptr<Face> Collection::load_entry_(CollectionEntry& e) {
    if (e.face)
      return e.face;
    if (!e.deferred.is_resolved())
      return nullptr;
    const float pixel_size = e.deferred.size_pt.value_or(16.0f);
    auto face = load_face_from_file(*e.deferred.path, pixel_size, e.deferred.face_index);
    if (face) {
      e.face = std::move(face);
    }
    return e.face;
  }

  std::shared_ptr<Face> Collection::resolve(Style style, char32_t cp) {
    auto& bucket = by_style_[static_cast<std::size_t>(style)];
    for (auto& entry : bucket) {
      auto face = load_entry_(entry);
      if (!face)
        continue;
      if (face->glyph_index(cp) != 0)
        return face;
    }
    return nullptr;
  }

  std::shared_ptr<Face> Collection::primary(Style style) {
    auto& bucket = by_style_[static_cast<std::size_t>(style)];
    for (auto& entry : bucket) {
      auto face = load_entry_(entry);
      if (face)
        return face;
    }
    return nullptr;
  }

  std::span<const CollectionEntry> Collection::entries(Style style) const noexcept {
    return std::span<const CollectionEntry>{by_style_[static_cast<std::size_t>(style)]};
  }

} // namespace fxe::font
