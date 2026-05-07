// Build-time selector for the discovery backend.

#include <fxe/font/discover.hpp>

namespace fxe::font {

#if FXE_FONT_HAS_CORETEXT
  std::unique_ptr<Discover> make_coretext_discover();
#endif
#if FXE_FONT_HAS_FONTCONFIG
  std::unique_ptr<Discover> make_fontconfig_discover();
#endif
#if FXE_FONT_HAS_WIN32_DIR
  std::unique_ptr<Discover> make_win32_discover();
#endif

  // Always-available no-op discoverer.
  std::unique_ptr<Discover> make_none_discover();

  std::unique_ptr<Discover> default_discover() {
#if FXE_FONT_HAS_CORETEXT
    return make_coretext_discover();
#elif FXE_FONT_HAS_FONTCONFIG
    return make_fontconfig_discover();
#elif FXE_FONT_HAS_WIN32_DIR
    return make_win32_discover();
#else
    return make_none_discover();
#endif
  }

} // namespace fxe::font
