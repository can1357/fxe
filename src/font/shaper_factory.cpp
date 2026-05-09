// Build-time selector for the shaper backend. Mirrors face_factory.cpp.

#include <fxe/font/shaper.hpp>

namespace fxe::font {

#if FXE_FONT_HAS_HARFBUZZ
  std::unique_ptr<Shaper> make_harfbuzz_shaper();
#endif

#if FXE_FONT_HAS_CORETEXT
  std::unique_ptr<Shaper> make_coretext_shaper();
#endif

  Shaper* default_shaper() {
    // Built once on first use and leaked deliberately — the shaper's caches
    // are valid for the entire process lifetime. Constructing a fresh
    // shaper per text draw was the source of a glyph-cache thrash bug:
    // CoreText's per-shaper substitute-face cache went away with the
    // shaper, so every cascade fallback (e.g. emoji within Latin text)
    // minted a new face_id, the glyph cache never deduplicated, and after
    // ~250 frames the cache hit its 4096-entry budget and started
    // evicting + repacking on every frame, scrambling cached text UVs.
    static Shaper* const k_shaper = []() -> Shaper* {
#if FXE_FONT_HAS_CORETEXT && !FXE_FONT_HAS_HARFBUZZ
      return make_coretext_shaper().release();
#elif FXE_FONT_HAS_HARFBUZZ
      return make_harfbuzz_shaper().release();
#elif FXE_FONT_HAS_CORETEXT
      return make_coretext_shaper().release();
#else
      return nullptr;
#endif
    }();
    return k_shaper;
  }

} // namespace fxe::font
