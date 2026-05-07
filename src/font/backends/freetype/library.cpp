// FreeType library singleton. Compiled when FXE_FONT_HAS_FREETYPE=1.
//
// FreeType's `FT_Library` is heavy to initialise (it loads modules and a
// memory allocator), but cheap to share — every face we open hangs off the
// same library handle. We expose it as a process-global with a mutex so the
// callers can serialise FT calls.

#include <fxe/font/library.hpp>

#include <stdexcept>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace fxe::font {

  Library::Library() {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
      throw std::runtime_error("fxe::font: FT_Init_FreeType failed");
    }
    impl_ = lib;
  }

  Library::~Library() {
    if (impl_) {
      FT_Done_FreeType(static_cast<FT_Library>(impl_));
      impl_ = nullptr;
    }
  }

  void* Library::raw() const noexcept {
    return impl_;
  }

  Library& shared_library() {
    static Library inst;
    return inst;
  }

} // namespace fxe::font
