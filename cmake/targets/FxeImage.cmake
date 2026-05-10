# fxe_image — image decoding/encoding. Wraps libpng (static + APNG),
# libjpeg-turbo (static JPEG), giflib (static + animated GIF),
# libwebp/libwebpdemux (static + animated WebP), and rlottie (Lottie JSON).
# Kept out of fxe_core so the lean core target keeps its slim dependency
# surface (glm/glfw/stb-resize/fxe_font).
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
        fxe::turbojpeg
        rlottie::rlottie
)
target_compile_features(fxe_image PUBLIC cxx_std_20)
