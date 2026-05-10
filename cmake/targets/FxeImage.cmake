# fxe_image — animated image decoding. Wraps giflib (animated GIF),
# libpng with the APNG patch (animated PNG), and libwebp/libwebpdemux
# (animated WebP). Kept out of fxe_core so the lean core target keeps its
# slim dependency surface (glm/glfw/stb/fxe_font).
file(GLOB _fxe_image_sources CONFIGURE_DEPENDS src/image/*.cpp)
add_library(fxe_image STATIC ${_fxe_image_sources})
add_library(fxe::image ALIAS fxe_image)
target_include_directories(fxe_image PUBLIC include)
target_link_libraries(fxe_image PUBLIC fxe_core)
target_link_libraries(
    fxe_image
    PRIVATE
        PNG::PNG
        GIF::GIF
        WebP::webp
        WebP::webpdemux
)
target_compile_features(fxe_image PUBLIC cxx_std_20)
