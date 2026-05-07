#pragma once

// Backend selector for the fxe font stack. Mirrors the matrix from Ghostty's
// `src/font/backend.zig`: each enum value is a (rasterizer, shaper, discovery)
// triple. The active value is fixed at build time by the FXE_FONT_BACKEND
// CMake option, but the enum is intentionally exposed at runtime so JS code
// (or tests) can introspect the choice.

#include <string_view>

namespace fxe::font {

  enum class Backend {
    freetype,            // bare FT + HB, no discovery
    fontconfig_freetype, // Linux default
    freetype_windows,    // Windows default
    coretext,            // macOS default — CT for everything
    coretext_freetype,   // CT discovery + FT raster + HB shape
    coretext_harfbuzz,   // CT discovery + raster, HB shape
  };

  // Compile-time backend selected by the build system. Sources can branch on
  // this value at runtime; the dead-code branches are stripped in release
  // builds because the value is constexpr.
  consteval Backend default_backend() noexcept {
#if defined(__APPLE__) && FXE_FONT_HAS_CORETEXT && !FXE_FONT_HAS_FREETYPE
    return Backend::coretext;
#elif defined(__APPLE__) && FXE_FONT_HAS_CORETEXT && FXE_FONT_HAS_FREETYPE && FXE_FONT_HAS_HARFBUZZ
    return Backend::coretext_freetype;
#elif defined(_WIN32) && FXE_FONT_HAS_FREETYPE && FXE_FONT_HAS_WIN32_DIR
    return Backend::freetype_windows;
#elif FXE_FONT_HAS_FONTCONFIG && FXE_FONT_HAS_FREETYPE
    return Backend::fontconfig_freetype;
#elif FXE_FONT_HAS_FREETYPE && FXE_FONT_HAS_HARFBUZZ
    return Backend::freetype;
#else
    return Backend::freetype;
#endif
  }

  // Capability predicates. These are constexpr so callers can branch with
  // `if constexpr` and have the unused branch dropped at compile time. They
  // mirror Ghostty's per-backend booleans.
  [[nodiscard]] constexpr bool has_freetype(Backend b) noexcept {
    switch (b) {
    case Backend::freetype:
    case Backend::fontconfig_freetype:
    case Backend::freetype_windows:
    case Backend::coretext_freetype:
      return true;
    case Backend::coretext:
    case Backend::coretext_harfbuzz:
      return false;
    }
    return false;
  }

  [[nodiscard]] constexpr bool has_harfbuzz(Backend b) noexcept {
    switch (b) {
    case Backend::freetype:
    case Backend::fontconfig_freetype:
    case Backend::freetype_windows:
    case Backend::coretext_freetype:
    case Backend::coretext_harfbuzz:
      return true;
    case Backend::coretext:
      return false;
    }
    return false;
  }

  [[nodiscard]] constexpr bool has_coretext(Backend b) noexcept {
    switch (b) {
    case Backend::coretext:
    case Backend::coretext_freetype:
    case Backend::coretext_harfbuzz:
      return true;
    case Backend::freetype:
    case Backend::fontconfig_freetype:
    case Backend::freetype_windows:
      return false;
    }
    return false;
  }

  [[nodiscard]] constexpr bool has_fontconfig(Backend b) noexcept {
    return b == Backend::fontconfig_freetype;
  }

  [[nodiscard]] std::string_view backend_name(Backend b) noexcept;

} // namespace fxe::font
