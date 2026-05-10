include(cmake/shaders.cmake)

file(GLOB _fxe_core_sources CONFIGURE_DEPENDS src/core/*.cpp)
add_library(fxe_core STATIC ${_fxe_core_sources})
add_library(fxe::core ALIAS fxe_core)
target_include_directories(fxe_core PUBLIC include)
target_link_libraries(fxe_core PUBLIC fxe_deps)
target_compile_features(fxe_core PUBLIC cxx_std_20)

embed_wgsl(fxe_shaders src/wgpu/shaders/main.wgsl)
target_link_libraries(fxe_core PUBLIC fxe_shaders)

add_library(fxe_log STATIC src/font/log.cpp)
add_library(fxe::log ALIAS fxe_log)
target_include_directories(fxe_log PUBLIC include)
target_link_libraries(fxe_log PUBLIC fxe_deps)
target_compile_features(fxe_log PUBLIC cxx_std_20)
if(TARGET fxe_runtime)
    target_link_libraries(fxe_runtime PUBLIC fxe_log)
endif()

# fxe_font — FreeType/CoreText face/atlas/shaper module. Always-on sources
# live at src/font/*.cpp; per-backend impls live in
# src/font/backends/{freetype,harfbuzz,coretext,fontconfig,windows,null}/
# and are pulled in by directory glob based on the discovered backend matrix.
file(GLOB _fxe_font_sources CONFIGURE_DEPENDS src/font/*.cpp src/font/*.mm)
list(REMOVE_ITEM _fxe_font_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/font/log.cpp")
add_library(fxe_font STATIC ${_fxe_font_sources})
add_library(fxe::font ALIAS fxe_font)
target_include_directories(fxe_font PUBLIC include)
target_link_libraries(fxe_font PUBLIC fxe_deps)
target_link_libraries(fxe_font PUBLIC fxe_log)
target_compile_features(fxe_font PUBLIC cxx_std_20)

# Embed JetBrainsMono Nerd Font Mono (SIL OFL-1.1, vendored under
# assets/fonts/) so apps get Nerd Font icon coverage without bundling
# a separate file. Generated at configure time via scripts/embed_binary.py.
set(_fxe_nerd_font_input
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/JetBrainsMonoNerdFontMono-Regular.ttf"
)
set(_fxe_nerd_font_output
    "${CMAKE_CURRENT_BINARY_DIR}/generated/fxe/jetbrains_nerd_font.cpp"
)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
    OUTPUT "${_fxe_nerd_font_output}"
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_binary.py"
        --input "${_fxe_nerd_font_input}"
        --output "${_fxe_nerd_font_output}"
        --namespace "fxe::font::embedded"
        --variable "jetbrains_nerd_font_ttf"
    DEPENDS "${_fxe_nerd_font_input}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_binary.py"
    COMMENT "Embedding JetBrainsMono Nerd Font Mono"
    VERBATIM
)
target_sources(fxe_font PRIVATE "${_fxe_nerd_font_output}")

if(FXE_FONT_HAS_FREETYPE)
    file(
        GLOB _fxe_font_freetype
        CONFIGURE_DEPENDS
        src/font/backends/freetype/*.cpp
    )
    target_sources(fxe_font PRIVATE ${_fxe_font_freetype})
    target_link_libraries(fxe_font PUBLIC Freetype::Freetype)
else()
    target_sources(fxe_font PRIVATE src/font/backends/null/library.cpp)
endif()

if(FXE_FONT_HAS_HARFBUZZ)
    file(
        GLOB _fxe_font_harfbuzz
        CONFIGURE_DEPENDS
        src/font/backends/harfbuzz/*.cpp
    )
    target_sources(fxe_font PRIVATE ${_fxe_font_harfbuzz})
    if(TARGET harfbuzz::harfbuzz)
        target_link_libraries(fxe_font PUBLIC harfbuzz::harfbuzz)
    elseif(TARGET PkgConfig::HARFBUZZ)
        target_link_libraries(fxe_font PUBLIC PkgConfig::HARFBUZZ)
    endif()
endif()

if(FXE_FONT_HAS_CORETEXT)
    file(
        GLOB _fxe_font_coretext
        CONFIGURE_DEPENDS
        src/font/backends/coretext/*.mm
    )
    target_sources(fxe_font PRIVATE ${_fxe_font_coretext})
    set_source_files_properties(
        ${_fxe_font_coretext}
        PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc"
    )
    target_link_libraries(
        fxe_font
        PUBLIC
            "-framework CoreText"
            "-framework CoreFoundation"
            "-framework CoreGraphics"
    )
endif()

if(FXE_FONT_HAS_FONTCONFIG)
    file(
        GLOB _fxe_font_fontconfig
        CONFIGURE_DEPENDS
        src/font/backends/fontconfig/*.cpp
    )
    target_sources(fxe_font PRIVATE ${_fxe_font_fontconfig})
    target_link_libraries(fxe_font PUBLIC Fontconfig::Fontconfig)
endif()

if(FXE_FONT_HAS_WIN32_DIR)
    file(
        GLOB _fxe_font_windows
        CONFIGURE_DEPENDS
        src/font/backends/windows/*.cpp
    )
    target_sources(fxe_font PRIVATE ${_fxe_font_windows})
endif()

# Null fallbacks: each null/<file>.cpp is included only when no real backend
# claims its slot.
if(NOT FXE_FONT_HAS_HARFBUZZ AND NOT FXE_FONT_HAS_CORETEXT)
    target_sources(fxe_font PRIVATE src/font/backends/null/shaper.cpp)
endif()
if(
    NOT FXE_FONT_HAS_FONTCONFIG
    AND NOT FXE_FONT_HAS_WIN32_DIR
    AND NOT FXE_FONT_HAS_CORETEXT
)
    target_sources(fxe_font PRIVATE src/font/backends/null/discover.cpp)
endif()

# fxe_core consumes the font module (text path uses font::Collection, font::Shaper,
# font::GlyphCache).
target_link_libraries(fxe_core PUBLIC fxe_font)
