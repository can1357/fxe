#include <fxe/font/backend.hpp>

namespace fxe::font {

  std::string_view backend_name(Backend b) noexcept {
    switch (b) {
    case Backend::freetype:
      return "freetype";
    case Backend::fontconfig_freetype:
      return "fontconfig_freetype";
    case Backend::freetype_windows:
      return "freetype_windows";
    case Backend::coretext:
      return "coretext";
    case Backend::coretext_freetype:
      return "coretext_freetype";
    case Backend::coretext_harfbuzz:
      return "coretext_harfbuzz";
    }
    return "unknown";
  }

} // namespace fxe::font
