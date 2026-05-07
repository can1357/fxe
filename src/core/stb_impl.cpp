// Single translation unit hosting the stb single-header library implementations.
// Toggled by FXE_HAS_STB (set by cmake/deps.cmake). When the headers are not
// available the TU collapses to a link anchor so fxe_core still builds.

#ifndef FXE_HAS_STB
#define FXE_HAS_STB 0
#endif

#if FXE_HAS_STB
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#endif

namespace fxe {
  void stb_link_anchor() {}
} // namespace fxe
