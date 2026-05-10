include(FetchContent)
# Lightweight dependencies: find_package first, then FetchContent when
# FXE_FETCH_DEPS=ON. With a vcpkg toolchain, vcpkg.json supplies binaries;
# product targets live under cmake/targets/.

add_library(fxe_deps INTERFACE)

find_package(glm QUIET)
if(glm_FOUND)
    target_link_libraries(fxe_deps INTERFACE glm::glm)
elseif(FXE_FETCH_DEPS)
    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glm)
    target_link_libraries(fxe_deps INTERFACE glm::glm)
else()
    message(
        FATAL_ERROR
        "glm was not found. Re-run with -DFXE_FETCH_DEPS=ON or install glm."
    )
endif()

find_package(glfw3 QUIET)
if(glfw3_FOUND)
    target_link_libraries(fxe_deps INTERFACE glfw)
elseif(FXE_FETCH_DEPS)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(fxe_deps INTERFACE nlohmann_json::nlohmann_json)
elseif(FXE_FETCH_DEPS)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
    target_link_libraries(fxe_deps INTERFACE nlohmann_json::nlohmann_json)
else()
    message(
        FATAL_ERROR
        "nlohmann_json was not found. Re-run with -DFXE_FETCH_DEPS=ON or install nlohmann_json."
    )
endif()

find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(fxe_deps INTERFACE ZLIB::ZLIB)
endif()

# Image codecs for fxe_image: animated GIF (giflib), animated PNG
# (libpng built with the APNG patch), and animated WebP (libwebp +
# libwebpdemux). All come from vcpkg in manifest mode; system installs
# of libpng without the APNG patch are not supported.
find_package(PNG REQUIRED)
find_package(GIF REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
# libjpeg-turbo ships either `libjpeg-turbo::turbojpeg` (shared) or
# `libjpeg-turbo::turbojpeg-static` depending on the triplet. Hide the
# split behind a single alias used by fxe_image / fxe_debug.
if(TARGET libjpeg-turbo::turbojpeg)
    add_library(fxe::turbojpeg ALIAS libjpeg-turbo::turbojpeg)
elseif(TARGET libjpeg-turbo::turbojpeg-static)
    add_library(fxe::turbojpeg ALIAS libjpeg-turbo::turbojpeg-static)
else()
    message(FATAL_ERROR "libjpeg-turbo::turbojpeg target not found")
endif()
find_package(RapidJSON CONFIG REQUIRED)
find_package(rlottie CONFIG REQUIRED)

# spdlog — fast structured logger. Used by `fxe::log` (include/fxe/log.hpp)
# for category-gated logging across the engine. Always fetched (vs. relying
# on system spdlog) so the bundled-fmt build is consistent across machines
# and we don't accidentally inherit a system spdlog built against external
# fmt that we don't ship.
if(NOT TARGET spdlog::spdlog)
    if(FXE_FETCH_DEPS)
        set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
        set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.15.3
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(spdlog)
        if(TARGET spdlog AND NOT MSVC)
            # spdlog's bundled fmt trips -Wconversion / -Wsign-conversion under
            # gcc; we don't own that source, so silence diagnostics on the
            # vendor target without leaking onto the rest of the build.
            target_compile_options(spdlog PRIVATE -w)
        endif()
    else()
        find_package(spdlog QUIET CONFIG)
        if(NOT TARGET spdlog::spdlog)
            message(
                FATAL_ERROR
                "spdlog was not found. Re-run with -DFXE_FETCH_DEPS=ON or install spdlog."
            )
        endif()
    endif()
endif()
target_link_libraries(fxe_deps INTERFACE spdlog::spdlog)

# Dawn and V8 are intentionally opt-in because both are heavyweight vendor flows.
# FXE_ENABLE_WGPU expects Dawn's C++ WebGPU headers/library to be supplied by
# the caller (for example via Dawn_DIR/CMAKE_PREFIX_PATH).
# FXE_ENABLE_V8 uses find_package(V8) and does not pull depot_tools implicitly.

# stb single-header libraries (image/decode, resize). truetype is no longer used
# by the text path (FreeType/CoreText replace it) but the headers still live
# alongside stb_image / stb_image_resize.
set(_FXE_STB_WIRED OFF)
if(FXE_FETCH_DEPS)
    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(stb)
    if(stb_SOURCE_DIR)
        target_include_directories(fxe_deps SYSTEM INTERFACE ${stb_SOURCE_DIR})
        set(_FXE_STB_WIRED ON)
    endif()
else()
    find_path(STB_INCLUDE_DIR stb_image.h)
    if(STB_INCLUDE_DIR)
        target_include_directories(fxe_deps SYSTEM INTERFACE ${STB_INCLUDE_DIR})
        set(_FXE_STB_WIRED ON)
    else()
        message(
            WARNING
            "stb headers not found; PNG decode and TTF rasterisation will be disabled (FXE_HAS_STB=0)."
        )
    endif()
endif()

# miniaudio single-header audio engine. Always fetched (not provided by vcpkg);
# callers may override FXE_MINIAUDIO_INCLUDE_DIR to point at a local copy.
if(NOT FXE_MINIAUDIO_INCLUDE_DIR)
    FetchContent_Declare(
        miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 0.11.25
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(miniaudio)
    set(FXE_MINIAUDIO_INCLUDE_DIR
        "${miniaudio_SOURCE_DIR}"
        CACHE PATH
        "miniaudio include directory"
        FORCE
    )
endif()
if(NOT EXISTS "${FXE_MINIAUDIO_INCLUDE_DIR}/miniaudio.h")
    message(
        FATAL_ERROR
        "miniaudio.h not found under FXE_MINIAUDIO_INCLUDE_DIR=${FXE_MINIAUDIO_INCLUDE_DIR}"
    )
endif()


# xstd — header-only C++20 utility library. Pulled in for `xstd::const_tag` /
# `xstd::type_tag` (compile-time value/type names used by `<fxe/v8_helpers.hpp>`
# to derive JS function names from the C++ callback identifier). Header-only
# INTERFACE target; no transitive deps.
if(NOT TARGET xstd)
    FetchContent_Declare(
        xstd
        GIT_REPOSITORY https://github.com/can1357/xstd.git
        GIT_TAG 983110fa89a9cacbdc5b9f4ad8d13651e0422ee6
        GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(xstd)
endif()
target_link_libraries(fxe_deps INTERFACE xstd)
# md4c — fast Markdown parser (CommonMark + GFM extensions). Used by the
# fxe_markdown library to power the Markdown JS API and the markdown UI
# component. Always fetched when FXE_FETCH_DEPS is on; otherwise expects a
# preinstalled `md4c` CMake target.
find_package(md4c QUIET CONFIG)
if(NOT TARGET md4c::md4c AND NOT TARGET md4c)
    if(FXE_FETCH_DEPS)
        set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            md4c
            GIT_REPOSITORY https://github.com/mity/md4c.git
            GIT_TAG release-0.5.2
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(md4c)
        if(TARGET md4c)
            target_include_directories(md4c PUBLIC $<BUILD_INTERFACE:${md4c_SOURCE_DIR}/src>)
            if(NOT MSVC)
                target_compile_options(md4c PRIVATE -w)
            endif()
        endif()
        if(DEFINED BUILD_SHARED_LIBS_SAVED)
            set(BUILD_SHARED_LIBS
                ${BUILD_SHARED_LIBS_SAVED}
                CACHE BOOL
                ""
                FORCE
            )
        endif()
    else()
        message(
            FATAL_ERROR
            "md4c was not found. Re-run with -DFXE_FETCH_DEPS=ON or install md4c."
        )
    endif()
endif()

# yoga — Facebook flexbox solver. Used by fxe_layout to back the JS
# `Layout.solve` binding. Prefer the vcpkg config; fall back to FetchContent
# when FXE_FETCH_DEPS is set.
find_package(yoga CONFIG QUIET)
if(NOT TARGET yoga::yogacore AND NOT TARGET yogacore)
    if(FXE_FETCH_DEPS)
        FetchContent_Declare(
            yoga
            GIT_REPOSITORY https://github.com/facebook/yoga.git
            GIT_TAG v3.2.1
            GIT_SHALLOW TRUE
        )
        set(BUILD_TESTING_SAVED ${BUILD_TESTING})
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(yoga)
        if(DEFINED BUILD_TESTING_SAVED)
            set(BUILD_TESTING ${BUILD_TESTING_SAVED} CACHE BOOL "" FORCE)
        endif()
        if(NOT TARGET yoga::yogacore AND TARGET yogacore)
            add_library(yoga::yogacore ALIAS yogacore)
        endif()
    else()
        message(FATAL_ERROR "yoga was not found. Re-run with -DFXE_FETCH_DEPS=ON or install yoga via vcpkg.")
    endif()
endif()

if(_FXE_STB_WIRED)
    target_compile_definitions(fxe_deps INTERFACE FXE_HAS_STB=1)
else()
    target_compile_definitions(fxe_deps INTERFACE FXE_HAS_STB=0)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11 COMPONENTS Xss QUIET)
    if(X11_FOUND AND X11_Xss_FOUND)
        set(FXE_HAS_XSS ON)
    else()
        set(FXE_HAS_XSS OFF)
    endif()
else()
    set(FXE_HAS_XSS OFF)
endif()

# Breakpad/Crashpad are optional crash dump providers. There is no hard
# dependency: platform crash handlers compile their in-tree minidump-lite
# fallback when no package target is found.
set(FXE_HAS_BREAKPAD OFF)
find_package(unofficial-breakpad CONFIG QUIET)
if(unofficial-breakpad_FOUND)
    set(FXE_HAS_BREAKPAD ON)
elseif(NOT unofficial-breakpad_FOUND)
    find_package(Breakpad CONFIG QUIET)
    if(Breakpad_FOUND)
        set(FXE_HAS_BREAKPAD ON)
    endif()
endif()
add_compile_definitions(FXE_HAS_BREAKPAD=$<BOOL:${FXE_HAS_BREAKPAD}>)

# ---------------------------------------------------------------------------
# Font stack: FreeType + HarfBuzz, plus per-platform discovery (Fontconfig on
# Linux, CoreText on macOS, manual font-dir scan on Windows). Mirrors
# Ghostty's src/font Backend matrix.
#
# FXE_FONT_BACKEND selects the high-level backend; auto-detection picks a
# sensible per-platform default if the user does not specify one. Each
# backend implies a (rasterizer, shaper, discovery) tuple.
# ---------------------------------------------------------------------------

set(_FXE_FONT_BACKENDS
    "freetype" # bare FreeType + HarfBuzz, no discovery
    "fontconfig_freetype" # Linux default
    "freetype_windows" # Windows default
    "coretext" # macOS default — CT raster + CT shape + CT discovery
    "coretext_freetype" # macOS opt-in: CT discovery, FT raster, HB shape
    "coretext_harfbuzz" # macOS opt-in: CT discovery + raster, HB shape
)

if(NOT DEFINED FXE_FONT_BACKEND OR FXE_FONT_BACKEND STREQUAL "")
    if(APPLE)
        set(_fxe_font_default "coretext")
    elseif(WIN32)
        set(_fxe_font_default "freetype_windows")
    elseif(UNIX)
        set(_fxe_font_default "fontconfig_freetype")
    else()
        set(_fxe_font_default "freetype")
    endif()
    set(FXE_FONT_BACKEND
        "${_fxe_font_default}"
        CACHE STRING
        "Font backend (one of: ${_FXE_FONT_BACKENDS})"
    )
endif()
set_property(CACHE FXE_FONT_BACKEND PROPERTY STRINGS ${_FXE_FONT_BACKENDS})

list(FIND _FXE_FONT_BACKENDS "${FXE_FONT_BACKEND}" _fxe_font_idx)
if(_fxe_font_idx LESS 0)
    message(
        FATAL_ERROR
        "FXE_FONT_BACKEND='${FXE_FONT_BACKEND}' is not one of: ${_FXE_FONT_BACKENDS}"
    )
endif()

# Decompose the backend string into the four feature flags consumed by the
# fxe_font CMake target and the source files. Mirrors Ghostty's Backend.zig
# `hasFreetype`, `hasCoretext`, etc.
set(FXE_FONT_HAS_FREETYPE OFF)
set(FXE_FONT_HAS_HARFBUZZ OFF)
set(FXE_FONT_HAS_CORETEXT OFF)
set(FXE_FONT_HAS_FONTCONFIG OFF)
set(FXE_FONT_HAS_WIN32_DIR OFF)
set(FXE_FONT_RASTERIZER "freetype") # freetype | coretext
set(FXE_FONT_SHAPER "harfbuzz") # harfbuzz | coretext
set(FXE_FONT_DISCOVERY "none") # none | fontconfig | coretext | win32

if(FXE_FONT_BACKEND STREQUAL "freetype")
    set(FXE_FONT_HAS_FREETYPE ON)
    set(FXE_FONT_HAS_HARFBUZZ ON)
elseif(FXE_FONT_BACKEND STREQUAL "fontconfig_freetype")
    set(FXE_FONT_HAS_FREETYPE ON)
    set(FXE_FONT_HAS_HARFBUZZ ON)
    set(FXE_FONT_HAS_FONTCONFIG ON)
    set(FXE_FONT_DISCOVERY "fontconfig")
elseif(FXE_FONT_BACKEND STREQUAL "freetype_windows")
    set(FXE_FONT_HAS_FREETYPE ON)
    set(FXE_FONT_HAS_HARFBUZZ ON)
    set(FXE_FONT_HAS_WIN32_DIR ON)
    set(FXE_FONT_DISCOVERY "win32")
elseif(FXE_FONT_BACKEND STREQUAL "coretext")
    set(FXE_FONT_HAS_CORETEXT ON)
    set(FXE_FONT_RASTERIZER "coretext")
    set(FXE_FONT_SHAPER "coretext")
    set(FXE_FONT_DISCOVERY "coretext")
elseif(FXE_FONT_BACKEND STREQUAL "coretext_freetype")
    set(FXE_FONT_HAS_FREETYPE ON)
    set(FXE_FONT_HAS_HARFBUZZ ON)
    set(FXE_FONT_HAS_CORETEXT ON)
    set(FXE_FONT_DISCOVERY "coretext")
elseif(FXE_FONT_BACKEND STREQUAL "coretext_harfbuzz")
    set(FXE_FONT_HAS_HARFBUZZ ON)
    set(FXE_FONT_HAS_CORETEXT ON)
    set(FXE_FONT_RASTERIZER "coretext")
    set(FXE_FONT_DISCOVERY "coretext")
endif()

if(FXE_FONT_HAS_FREETYPE)
    find_package(Freetype REQUIRED)
    target_link_libraries(fxe_deps INTERFACE Freetype::Freetype)
endif()

if(FXE_FONT_HAS_HARFBUZZ)
    # vcpkg ships a CMake config; system installs may only have pkg-config.
    find_package(harfbuzz CONFIG QUIET)
    if(NOT harfbuzz_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(HARFBUZZ IMPORTED_TARGET harfbuzz)
        endif()
        if(NOT HARFBUZZ_FOUND)
            message(
                FATAL_ERROR
                "harfbuzz was not found. Install via vcpkg manifest or pkg-config."
            )
        endif()
    endif()
endif()

if(FXE_FONT_HAS_FONTCONFIG)
    find_package(Fontconfig REQUIRED)
endif()

# Backend feature flags become compile definitions on fxe_deps so any TU can
# branch on the chosen backend without re-deriving it.
target_compile_definitions(
    fxe_deps
    INTERFACE
        FXE_FONT_BACKEND_NAME="${FXE_FONT_BACKEND}"
        FXE_FONT_HAS_FREETYPE=$<BOOL:${FXE_FONT_HAS_FREETYPE}>
        FXE_FONT_HAS_HARFBUZZ=$<BOOL:${FXE_FONT_HAS_HARFBUZZ}>
        FXE_FONT_HAS_CORETEXT=$<BOOL:${FXE_FONT_HAS_CORETEXT}>
        FXE_FONT_HAS_FONTCONFIG=$<BOOL:${FXE_FONT_HAS_FONTCONFIG}>
        FXE_FONT_HAS_WIN32_DIR=$<BOOL:${FXE_FONT_HAS_WIN32_DIR}>
)

message(
    STATUS
    "fxe font backend: ${FXE_FONT_BACKEND} (raster=${FXE_FONT_RASTERIZER} shape=${FXE_FONT_SHAPER} discover=${FXE_FONT_DISCOVERY})"
)

# Treat third-party dep headers as system headers so their warnings
# (e.g. -Wold-style-cast in stb, -Wsign-conversion in md4c, etc.) do
# not surface in our build. Only affects targets we don't own.
function(_fxe_mark_system_include tgt)
    if(NOT TARGET ${tgt})
        return()
    endif()
    get_target_property(_aliased ${tgt} ALIASED_TARGET)
    if(_aliased)
        set(tgt ${_aliased})
    endif()
    get_target_property(_type ${tgt} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(_inc ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
    else()
        get_target_property(_inc ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
    endif()
    if(_inc)
        set_target_properties(${tgt} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
endfunction()

foreach(_dep
    glm::glm glm
    glfw
    nlohmann_json::nlohmann_json nlohmann_json
    spdlog::spdlog spdlog
    md4c::md4c md4c md4c::md4c_html
    yoga::yogacore yogacore
    xstd
)
    _fxe_mark_system_include(${_dep})
endforeach()
