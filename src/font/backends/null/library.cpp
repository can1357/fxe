// No-op FreeType library shim. Compiled when FXE_FONT_HAS_FREETYPE=0 (i.e.
// pure-CoreText builds). Keeps the public Library API available so callers
// don't need to branch on the active backend at the call site.

#include <fxe/font/library.hpp>

namespace fxe::font {

  Library::Library() = default;
  Library::~Library() = default;

  void* Library::raw() const noexcept {
    return nullptr;
  }

  Library& shared_library() {
    static Library inst;
    return inst;
  }

} // namespace fxe::font
