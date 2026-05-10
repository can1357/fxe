// Single translation unit hosting the stb single-header library
// implementations still in use after the libpng/libjpeg-turbo/libwebp/giflib
// migration. Only stb_image_resize2 remains; static-image decode and
// PNG/JPEG encode now go through real codec libraries.

#ifndef FXE_HAS_STB
#define FXE_HAS_STB 0
#endif

#if FXE_HAS_STB
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#endif

namespace fxe {
  void stb_link_anchor() {}
} // namespace fxe
