#pragma once

// Discovery — turn a Descriptor into one or more concrete fonts on disk.
// Each platform has its own backend; the active one is selected at build
// time by FXE_FONT_BACKEND.

#include <memory>
#include <vector>

#include <fxe/font/descriptor.hpp>

namespace fxe::font {

  class Discover {
  public:
    virtual ~Discover() = default;
    // Returns 0 or more matching descriptors, ordered by quality. Each
    // returned descriptor has `path` set so a subsequent `load_face_*` call
    // can open it without further searching.
    [[nodiscard]] virtual std::vector<Descriptor> find(const Descriptor& query) = 0;
  };

  // Returns the platform-default discoverer. The chosen implementation is
  // controlled by FXE_FONT_BACKEND at build time:
  //   coretext / coretext_*       → DiscoverCoreText
  //   fontconfig_freetype          → DiscoverFontconfig
  //   freetype_windows             → DiscoverWindows
  //   freetype                     → DiscoverNone (always returns empty)
  [[nodiscard]] std::unique_ptr<Discover> default_discover();

} // namespace fxe::font
