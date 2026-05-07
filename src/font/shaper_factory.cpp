// Build-time selector for the shaper backend. Mirrors face_factory.cpp.

#include <fxe/font/shaper.hpp>

namespace fxe::font {

#if FXE_FONT_HAS_HARFBUZZ
  std::unique_ptr<Shaper> make_harfbuzz_shaper();
#endif

#if FXE_FONT_HAS_CORETEXT
  std::unique_ptr<Shaper> make_coretext_shaper();
#endif

  std::unique_ptr<Shaper> default_shaper() {
#if FXE_FONT_HAS_CORETEXT && !FXE_FONT_HAS_HARFBUZZ
    return make_coretext_shaper();
#elif FXE_FONT_HAS_HARFBUZZ
    return make_harfbuzz_shaper();
#elif FXE_FONT_HAS_CORETEXT
    return make_coretext_shaper();
#else
    return nullptr;
#endif
  }

} // namespace fxe::font
