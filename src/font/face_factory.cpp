// Backend-agnostic face factory. Dispatches `load_face_from_*` to the
// rasterizer chosen at build time. The concrete backends live in
// `face_freetype.cpp` and `face_coretext.mm` and are linked conditionally.

#include <fxe/font/face.hpp>

#include <fstream>
#include <fxe/types.hpp>
#include <vector>

namespace fxe::font {

#if FXE_FONT_HAS_FREETYPE
  std::unique_ptr<Face> load_face_freetype(std::span<const u8> bytes, float pixel_size,
                                           u32 face_index);
#else
  std::unique_ptr<Face> load_face_freetype(std::span<const u8>, float, u32) {
    return nullptr;
  }
#endif

#if FXE_FONT_HAS_CORETEXT
  std::unique_ptr<Face> load_face_coretext(std::span<const u8> bytes, float pixel_size);
  std::unique_ptr<Face> load_face_coretext_name(std::string_view family, float pixel_size,
                                                Style style);
  std::unique_ptr<Face> make_face_from_ctfont(void* ct_font_ref, float pixel_size);
#else
  std::unique_ptr<Face> load_face_coretext(std::span<const u8>, float) {
    return nullptr;
  }
  std::unique_ptr<Face> load_face_coretext_name(std::string_view, float, Style) {
    return nullptr;
  }
  std::unique_ptr<Face> make_face_from_ctfont(void*, float) {
    return nullptr;
  }
#endif

  std::unique_ptr<Face> load_face_from_bytes(std::span<const u8> bytes, float pixel_size,
                                             u32 face_index) {
#if FXE_FONT_HAS_CORETEXT && !FXE_FONT_HAS_FREETYPE
    (void)face_index;
    return load_face_coretext(bytes, pixel_size);
#elif FXE_FONT_HAS_FREETYPE
    return load_face_freetype(bytes, pixel_size, face_index);
#else
    (void)bytes;
    (void)pixel_size;
    (void)face_index;
    return nullptr;
#endif
  }

  std::unique_ptr<Face> load_face_from_file(std::string_view path, float pixel_size,
                                            u32 face_index) {
    std::ifstream in(std::string{path}, std::ios::binary | std::ios::ate);
    if (!in)
      return nullptr;
    auto sz = in.tellg();
    if (sz <= 0)
      return nullptr;
    std::vector<u8> buf(static_cast<usize>(sz));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(buf.data()), sz))
      return nullptr;
    return load_face_from_bytes(buf, pixel_size, face_index);
  }

} // namespace fxe::font
