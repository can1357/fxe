cmake_minimum_required(VERSION 3.24)
project(fxe VERSION 0.1.0 LANGUAGES CXX)

# Touch CMAKE_TOOLCHAIN_FILE so passing it via preset/-D doesn't trigger
# CMake's "manually-specified variable not used" warning when vcpkg has
# already cached it.
if(CMAKE_TOOLCHAIN_FILE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

option(FXE_BUILD_EXAMPLES "Build fxe examples" ON)
option(FXE_BUILD_TESTS "Build fxe tests" ON)
option(FXE_ENABLE_WGPU "Build Dawn/WebGPU backend when Dawn is available" ON)
option(FXE_ENABLE_V8 "Build embedded V8 host/bindings when V8 is available" ON)
option(
    FXE_ENABLE_NODE_COMPAT
    "Generate vendored unenv assets for V8 Node compatibility shims"
    ON
)
option(
    FXE_ENABLE_LIBUV
    "Build libuv-backed runtime loop foundation for Node network work"
    ON
)
option(
    FXE_ENABLE_NATIVE_TLS_HTTP2
    "Configure mbedTLS/nghttp2 dependencies for future native HTTPS/HTTP2 transport; does not enable HTTP/2 support"
    ON
)
option(
    FXE_FETCH_DEPS
    "Fetch header-only/core dependencies with FetchContent"
    ON
)
if(UNIX AND NOT APPLE)
    option(
        FXE_OS_DBUS
        "Enable Linux desktop integrations through libdbus-1 when available"
        ON
    )
else()
    option(
        FXE_OS_DBUS
        "Enable Linux desktop integrations through libdbus-1 when available"
        OFF
    )
endif()

option(
    FXE_ENABLE_WARNINGS
    "Enable useful compiler warnings for project targets"
    ON
)
option(FXE_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/deps.cmake)

if(FXE_ENABLE_WARNINGS)
    set(_fxe_clang_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wcast-align
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
        -Wimplicit-fallthrough
        -Wunreachable-code
        -Wunreachable-code-aggressive
    )
    set(_fxe_gnu_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wcast-align
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
        -Wimplicit-fallthrough=5
    )
    set(_fxe_msvc_warnings
        /W4
        /permissive-
        /w14242
        /w14254
        /w14263
        /w14265
        /w14287
        /we4289
        /w14296
        /we4702
        /w14311
        /w14545
        /w14546
        /w14547
        /w14549
        /w14555
        /w14640
        /w14826
        /w14928
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        add_compile_options(${_fxe_clang_warnings})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(${_fxe_gnu_warnings})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(${_fxe_msvc_warnings})
    endif()
endif()
if(FXE_WARNINGS_AS_ERRORS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        add_compile_options(-Werror)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(/WX)
    endif()
endif()

# OS shims (App, shell, dialog, notification, menu, tray, globalShortcut).
# Common sources live at src/os/*.cpp; per-platform impls live in
# src/os/{macos,linux,win32}/ and are picked up by directory glob.
file(GLOB _fxe_os_sources CONFIGURE_DEPENDS src/os/*.cpp)
add_library(fxe_os STATIC ${_fxe_os_sources})
target_include_directories(fxe_os PUBLIC src include)
target_compile_features(fxe_os PUBLIC cxx_std_20)
if(APPLE)
    file(
        GLOB _fxe_os_platform
        CONFIGURE_DEPENDS
        src/os/macos/*.cpp
        src/os/macos/*.mm
    )
    target_sources(fxe_os PRIVATE ${_fxe_os_platform})
    set_source_files_properties(
        ${_fxe_os_platform}
        PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc"
    )
    target_link_libraries(
        fxe_os
        PUBLIC
            "-framework AppKit"
            "-framework Foundation"
            "-framework UserNotifications"
            "-framework UniformTypeIdentifiers"
            "-framework Carbon"
            "-framework IOKit"
            "-framework Network"
            "-framework SystemConfiguration"
            "-framework CoreGraphics"
    )
elseif(WIN32)
    file(GLOB _fxe_os_platform CONFIGURE_DEPENDS src/os/win32/*.cpp)
    target_sources(fxe_os PRIVATE ${_fxe_os_platform})
    target_link_libraries(fxe_os PUBLIC Wininet Wtsapi32 Dbghelp Winhttp)
else()
    file(GLOB _fxe_os_platform CONFIGURE_DEPENDS src/os/linux/*.cpp)
    target_sources(fxe_os PRIVATE ${_fxe_os_platform})
    if(FXE_HAS_XSS)
        target_link_libraries(fxe_os PUBLIC X11::X11 X11::Xss)
        target_compile_definitions(fxe_os PUBLIC FXE_HAS_XSS=1)
    else()
        target_compile_definitions(fxe_os PUBLIC FXE_HAS_XSS=0)
    endif()
    if(FXE_OS_DBUS)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
        endif()
        if(DBUS_FOUND)
            find_package(Threads REQUIRED)
            target_link_libraries(
                fxe_os
                PUBLIC PkgConfig::DBUS Threads::Threads
            )
            target_compile_definitions(fxe_os PUBLIC FXE_HAS_DBUS=1)
        else()
            message(
                STATUS
                "libdbus-1 not found; Linux desktop D-Bus integrations disabled"
            )
            target_compile_definitions(fxe_os PUBLIC FXE_HAS_DBUS=0)
        endif()
    else()
        target_compile_definitions(fxe_os PUBLIC FXE_HAS_DBUS=0)
    endif()
endif()

# Networking — http (libcurl optional) + websocket client.
add_library(
    fxe_net
    STATIC
    src/net/cookie_jar.cpp
    src/net/http_client.cpp
    src/net/websocket_client.cpp
)
target_include_directories(fxe_net PUBLIC src)
target_compile_features(fxe_net PUBLIC cxx_std_20)
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(fxe_net PUBLIC CURL::libcurl)
    target_compile_definitions(fxe_net PUBLIC FXE_HAS_CURL=1)
    target_link_libraries(fxe_os PUBLIC CURL::libcurl)
    target_compile_definitions(fxe_os PUBLIC FXE_HAS_CURL=1)
else()
    message(STATUS "libcurl not found; fetch() will reject")
endif()

if(FXE_ENABLE_NATIVE_TLS_HTTP2)
    find_package(MbedTLS CONFIG REQUIRED)
    find_package(PkgConfig REQUIRED)
    find_package(ZLIB REQUIRED)
    pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)
    target_link_libraries(
        fxe_net
        PRIVATE
            MbedTLS::mbedtls
            MbedTLS::mbedx509
            MbedTLS::mbedcrypto
            PkgConfig::NGHTTP2
            ZLIB::ZLIB
    )
    target_compile_definitions(fxe_net PUBLIC FXE_HAS_NATIVE_TLS_HTTP2_DEPS=1)
    target_sources(
        fxe_net
        PRIVATE
            src/net/tls_client.cpp
            src/net/tls_server.cpp
            src/net/http2_client.cpp
            src/net/http2_server.cpp
    )
else()
    target_compile_definitions(fxe_net PUBLIC FXE_HAS_NATIVE_TLS_HTTP2_DEPS=0)
endif()
if(WIN32)
    target_link_libraries(fxe_net PUBLIC ws2_32)
endif()

find_package(MbedTLS CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

# Runtime support shared by the V8 runner and packager-facing embedder APIs.
# V8-host-only sources live under src/runtime/v8/ and are picked up by fxe_js.
file(GLOB _fxe_runtime_sources CONFIGURE_DEPENDS src/runtime/*.cpp)
list(APPEND _fxe_runtime_sources tools/fxe-pack/bundle.cpp)
add_library(fxe_runtime STATIC ${_fxe_runtime_sources})
add_library(fxe::runtime ALIAS fxe_runtime)
target_link_libraries(fxe_runtime PRIVATE unofficial-sodium::sodium)
target_compile_features(fxe_runtime PUBLIC cxx_std_20)
target_include_directories(
    fxe_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/fxe-pack"
)
if(FXE_ENABLE_LIBUV)
    find_package(libuv CONFIG QUIET)
    if(TARGET libuv::uv_a)
        set(_fxe_libuv_target libuv::uv_a)
    elseif(TARGET libuv::uv)
        set(_fxe_libuv_target libuv::uv)
    else()
        message(
            FATAL_ERROR
            "FXE_ENABLE_LIBUV=ON requires libuv. Install the vcpkg manifest dependency or provide a libuv package exporting libuv::uv_a or libuv::uv."
        )
    endif()
    target_link_libraries(fxe_runtime PUBLIC ${_fxe_libuv_target})
    target_compile_definitions(fxe_runtime PUBLIC FXE_HAS_LIBUV=1)
else()
    target_compile_definitions(fxe_runtime PUBLIC FXE_HAS_LIBUV=0)
endif()
if(APPLE)
    target_link_libraries(
        fxe_runtime
        PUBLIC "-framework CoreServices" "-framework CoreFoundation"
    )
endif()
# Audio — miniaudio-backed engine.
add_library(fxe_audio STATIC src/audio/audio.cpp)
target_include_directories(fxe_audio SYSTEM PUBLIC ${FXE_MINIAUDIO_INCLUDE_DIR})
target_include_directories(fxe_audio PUBLIC src)
target_compile_features(fxe_audio PUBLIC cxx_std_20)
if(APPLE)
    target_link_libraries(
        fxe_audio
        PUBLIC
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework AudioUnit"
            "-framework CoreFoundation"
    )
endif()
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(fxe_audio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

include(cmake/shaders.cmake)

file(GLOB _fxe_core_sources CONFIGURE_DEPENDS src/core/*.cpp)
add_library(fxe_core STATIC ${_fxe_core_sources})
add_library(fxe::core ALIAS fxe_core)
target_include_directories(fxe_core PUBLIC include)
target_link_libraries(fxe_core PUBLIC fxe_deps)
target_compile_features(fxe_core PUBLIC cxx_std_20)

embed_wgsl(fxe_shaders src/wgpu/shaders/main.wgsl)
target_link_libraries(fxe_core PUBLIC fxe_shaders)

# fxe_font — FreeType/CoreText face/atlas/shaper module. Always-on sources
# live at src/font/*.cpp; per-backend impls live in
# src/font/backends/{freetype,harfbuzz,coretext,fontconfig,windows,null}/
# and are pulled in by directory glob based on the discovered backend matrix.
file(GLOB _fxe_font_sources CONFIGURE_DEPENDS src/font/*.cpp src/font/*.mm)
add_library(fxe_font STATIC ${_fxe_font_sources})
add_library(fxe::font ALIAS fxe_font)
target_include_directories(fxe_font PUBLIC include)
target_link_libraries(fxe_font PUBLIC fxe_deps)
target_compile_features(fxe_font PUBLIC cxx_std_20)

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

add_library(
    fxe_window
    STATIC
    src/window/glfw_window.cpp
    src/wgpu/renderer_wgpu.cpp
)
add_library(fxe::window ALIAS fxe_window)
target_link_libraries(fxe_window PUBLIC fxe_core)
target_link_libraries(fxe_window PUBLIC fxe_os)
if(TARGET glfw)
    target_link_libraries(fxe_window PUBLIC glfw)
    target_compile_definitions(fxe_window PUBLIC FXE_HAS_GLFW=1)
else()
    target_compile_definitions(fxe_window PUBLIC FXE_HAS_GLFW=0)
endif()

if(APPLE)
    set_source_files_properties(
        src/window/glfw_window.cpp
        PROPERTIES
            COMPILE_FLAGS
                "-x objective-c++ -fobjc-arc -Wno-old-style-cast -Wno-gnu-conditional-omitted-operand"
    )
endif()

if(FXE_ENABLE_WGPU)
    target_compile_definitions(fxe_window PUBLIC FXE_HAS_WGPU=1)
    add_library(
        fxe_wgpu
        STATIC
        src/wgpu/renderer_dawn.cpp
        src/wgpu/pipeline.cpp
        src/wgpu/offscreen.cpp
    )
    add_library(fxe::wgpu ALIAS fxe_wgpu)
    target_link_libraries(fxe_wgpu PUBLIC fxe_window)
    target_link_libraries(fxe_wgpu PUBLIC fxe_debug)
    target_compile_definitions(fxe_wgpu PUBLIC FXE_HAS_WGPU=1)
    find_package(Threads REQUIRED)
    # Auto-discover Dawn at common install prefixes if the caller didn't pin one.
    if(NOT DEFINED Dawn_DIR AND NOT DEFINED dawn_DIR)
        foreach(
            _p
            "$ENV{HOME}/dawn/install/lib/cmake/Dawn"
            "/opt/dawn/lib/cmake/Dawn"
            "/usr/local/lib/cmake/Dawn"
        )
            if(EXISTS "${_p}/DawnConfig.cmake")
                set(Dawn_DIR "${_p}" CACHE PATH "Dawn install dir" FORCE)
                message(STATUS "fxe: discovered Dawn at ${_p}")
                break()
            endif()
        endforeach()
    endif()
    find_package(Dawn QUIET)
    if(TARGET dawn::webgpu_dawn)
        target_link_libraries(fxe_window PUBLIC dawn::webgpu_dawn)
        target_link_libraries(fxe_wgpu PUBLIC dawn::webgpu_dawn)
    elseif(TARGET webgpu_dawn)
        target_link_libraries(fxe_window PUBLIC webgpu_dawn)
        target_link_libraries(fxe_wgpu PUBLIC webgpu_dawn)
    else()
        message(
            WARNING
            "FXE_ENABLE_WGPU=ON but Dawn target not found; the Dawn renderer will compile only against `<webgpu/webgpu_cpp.h>` discovered via include path."
        )
    endif()
    if(TARGET dawn::dawn_public_config)
        # glfw_window.cpp #includes <webgpu/webgpu_cpp.h> for make_wgpu_surface;
        # expose Dawn's public include dirs on fxe_window so the TU compiles.
        target_link_libraries(fxe_window PUBLIC dawn::dawn_public_config)
    endif()
    if(APPLE)
        target_link_libraries(fxe_window PUBLIC "-framework QuartzCore")
    endif()
else()
    target_compile_definitions(fxe_window PUBLIC FXE_HAS_WGPU=0)
    target_sources(fxe_window PRIVATE src/wgpu/offscreen.cpp)
endif()

# fxe_debug — out-of-process debugger / control-plane server (NDJSON over TCP).
# Always available; FXE_HAS_V8 unlocks Runtime.* handlers when fxe_js is linked.
# Stubs under src/debug/stubs/ are pulled in only when V8 is disabled.
file(GLOB _fxe_debug_sources CONFIGURE_DEPENDS src/debug/*.cpp)
add_library(fxe_debug STATIC ${_fxe_debug_sources})
if(NOT FXE_ENABLE_V8)
    # Stubs for fxe::js::host accessor symbols that fxe_debug references but
    # never invokes (call sites guard on cx.host != nullptr). Only compiled in
    # builds without V8; otherwise the real implementations live in fxe_js.
    file(GLOB _fxe_debug_stubs CONFIGURE_DEPENDS src/debug/stubs/*.cpp)
    target_sources(fxe_debug PRIVATE ${_fxe_debug_stubs})
endif()
add_library(fxe::debug ALIAS fxe_debug)
target_link_libraries(fxe_debug PUBLIC fxe_window)
target_compile_features(fxe_debug PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(fxe_debug PUBLIC Threads::Threads)
target_link_libraries(fxe_debug PRIVATE MbedTLS::mbedcrypto)
if(WIN32)
    target_link_libraries(fxe_debug PUBLIC ws2_32)
endif()

if(FXE_ENABLE_V8)
    find_package(SQLite3 REQUIRED)
    find_package(V8 REQUIRED)
    # native_tls/http2/https sources moved under src/runtime/v8/native/ and
    # are picked up by fxe_js below when FXE_ENABLE_NATIVE_TLS_HTTP2 is set.
    find_package(MbedTLS CONFIG REQUIRED)
    find_program(NPM_EXECUTABLE npm REQUIRED)
    set(_fxe_typescript_js
        "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/typescript/lib/typescript.js"
    )
    if(NOT EXISTS "${_fxe_typescript_js}")
        add_custom_command(
            OUTPUT "${_fxe_typescript_js}"
            COMMAND "${NPM_EXECUTABLE}" ci
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            DEPENDS package.json package-lock.json
            COMMENT "Installing TypeScript compiler package"
        )
    endif()

    set(_fxe_js_generated_dir
        "${CMAKE_CURRENT_BINARY_DIR}/generated/fxe/generated"
    )
    set(_fxe_typescript_compiler_h
        "${_fxe_js_generated_dir}/typescript_compiler.hpp"
    )
    set(_fxe_types_h "${_fxe_js_generated_dir}/fxe_types.hpp")
    set(_fxe_unenv_asset_sources)
    if(FXE_ENABLE_NODE_COMPAT)
        include(FetchContent)
        FetchContent_Declare(
            unenv
            GIT_REPOSITORY https://github.com/unjs/unenv.git
            GIT_TAG v2.0.0-rc.24
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(unenv)
        FetchContent_Declare(
            pathe
            URL https://registry.npmjs.org/pathe/-/pathe-2.0.3.tgz
            URL_HASH
                SHA256=6e3b73bc5dbb7f8a108ff5d8459b07da04062e822c662339416525510696df36
        )
        FetchContent_MakeAvailable(pathe)
        set(_fxe_unenv_root "${unenv_SOURCE_DIR}")
        set(_fxe_pathe_dist_root "${pathe_SOURCE_DIR}/dist")
        set(_fxe_unenv_assets_h "${_fxe_js_generated_dir}/unenv_assets.hpp")
        list(APPEND _fxe_unenv_asset_sources "${_fxe_unenv_assets_h}")
        file(
            GLOB_RECURSE _fxe_unenv_runtime_sources
            CONFIGURE_DEPENDS
            "${_fxe_unenv_root}/src/runtime/*"
            "${_fxe_pathe_dist_root}/*"
        )
        add_custom_command(
            OUTPUT "${_fxe_unenv_assets_h}"
            COMMAND
                "${CMAKE_COMMAND}" -DUNENV_ROOT="${_fxe_unenv_root}"
                -DPATHE_DIST_ROOT="${_fxe_pathe_dist_root}"
                -DOUTPUT="${_fxe_unenv_assets_h}" -DNAMESPACE="fxe::runtime"
                -DVARIABLE="k_unenv_assets" -DDELIMITER="FXEUNENV" -P
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_unenv.cmake"
            DEPENDS ${_fxe_unenv_runtime_sources} cmake/embed_unenv.cmake
            COMMENT "Embedding unenv runtime assets"
        )
    endif()
    add_custom_command(
        OUTPUT "${_fxe_typescript_compiler_h}"
        COMMAND
            "${CMAKE_COMMAND}" -DINPUT="${_fxe_typescript_js}"
            -DOUTPUT="${_fxe_typescript_compiler_h}" -DNAMESPACE="fxe::js"
            -DVARIABLE="k_typescript_compiler_source" -DDELIMITER="FXETS" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text_header.cmake"
        DEPENDS "${_fxe_typescript_js}" cmake/embed_text_header.cmake
        COMMENT "Embedding TypeScript compiler"
    )
    add_custom_command(
        OUTPUT "${_fxe_types_h}"
        COMMAND
            "${CMAKE_COMMAND}"
            -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/types/fxe.d.ts"
            -DOUTPUT="${_fxe_types_h}" -DNAMESPACE="fxe::js"
            -DVARIABLE="k_fxe_types_source" -DDELIMITER="FXETYPES" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text_header.cmake"
        DEPENDS types/fxe.d.ts cmake/embed_text_header.cmake
        COMMENT "Embedding fxe TypeScript declarations"
    )

    set(_fxe_node_compat_scripts
        os_adapter
        tty_adapter
        crypto_adapter
        child_process_adapter
        fs_adapter
        fs_promises_adapter
        worker_threads_adapter
        net_adapter
        dgram_adapter
        dns_adapter
        dns_promises_adapter
        tls_adapter
        tls_native_adapter
        https_adapter
        https_native_adapter
        http2_adapter
        http2_native_adapter
        events_adapter
        buffer_adapter
        process_adapter
        path_adapter
        path_posix_adapter
        path_win32_adapter
        url_adapter
        querystring_adapter
        util_adapter
        util_types_adapter
        console_adapter
        timers_adapter
        timers_promises_adapter
        stream_adapter
        stream_promises_adapter
        prelude
    )
    set(_fxe_node_compat_js_headers)
    foreach(_script IN LISTS _fxe_node_compat_scripts)
        set(_input
            "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/v8/node_compat_js/${_script}.js"
        )
        set(_output "${_fxe_js_generated_dir}/node_compat/${_script}.hpp")
        add_custom_command(
            OUTPUT "${_output}"
            COMMAND
                "${CMAKE_COMMAND}" -DINPUT="${_input}" -DOUTPUT="${_output}"
                -DNAMESPACE="fxe::runtime::node_js"
                -DVARIABLE="k_${_script}_source" -DDELIMITER="FXENODE" -P
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text_header.cmake"
            DEPENDS "${_input}" cmake/embed_text_header.cmake
            COMMENT "Embedding node compat script ${_script}"
        )
        list(APPEND _fxe_node_compat_js_headers "${_output}")
    endforeach()
    # fs_watcher backends live under src/runtime/v8/fs_watcher/<plat>.cpp.
    if(APPLE)
        file(
            GLOB _fxe_fs_watcher_source
            CONFIGURE_DEPENDS
            src/runtime/v8/fs_watcher/macos.cpp
        )
    elseif(WIN32)
        file(
            GLOB _fxe_fs_watcher_source
            CONFIGURE_DEPENDS
            src/runtime/v8/fs_watcher/win32.cpp
        )
    else()
        file(
            GLOB _fxe_fs_watcher_source
            CONFIGURE_DEPENDS
            src/runtime/v8/fs_watcher/linux.cpp
        )
    endif()
    # fxe_js: bindings (src/js/*.cpp) + V8-host runtime (src/runtime/v8/*.cpp,
    # plus the platform-selected fs_watcher above). The runner exec lives in
    # src/runner/ so it is not picked up here.
    file(GLOB _fxe_js_sources CONFIGURE_DEPENDS src/js/*.cpp)
    file(GLOB _fxe_runtime_v8_sources CONFIGURE_DEPENDS src/runtime/v8/*.cpp)
    if(FXE_ENABLE_NATIVE_TLS_HTTP2)
        file(
            GLOB _fxe_runtime_v8_native
            CONFIGURE_DEPENDS
            src/runtime/v8/native/*.cpp
        )
        list(APPEND _fxe_runtime_v8_sources ${_fxe_runtime_v8_native})
    endif()
    add_library(
        fxe_js
        STATIC
        ${_fxe_js_sources}
        ${_fxe_runtime_v8_sources}
        ${_fxe_fs_watcher_source}
        "${_fxe_typescript_compiler_h}"
        "${_fxe_types_h}"
        ${_fxe_unenv_asset_sources}
        ${_fxe_node_compat_js_headers}
    )
    add_library(fxe::js ALIAS fxe_js)
    target_include_directories(
        fxe_js
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_link_libraries(fxe_js PUBLIC fxe_window V8::V8)
    target_link_libraries(fxe_js PUBLIC fxe_os fxe_net fxe_audio fxe_runtime)
    target_link_libraries(fxe_js PUBLIC MbedTLS::mbedcrypto)
    target_link_libraries(fxe_js PRIVATE unofficial-sodium::sodium)
    target_link_libraries(fxe_js PRIVATE SQLite3::SQLite3)
    if(WIN32)
        target_link_libraries(fxe_js PUBLIC bcrypt ws2_32)
    endif()
    target_compile_definitions(fxe_js PUBLIC FXE_HAS_V8=1)
    set_source_files_properties(
        src/js/typescript.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings"
    )
    target_compile_definitions(
        fxe_js
        PRIVATE FXE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(FXE_ENABLE_NODE_COMPAT)
        target_compile_definitions(fxe_js PUBLIC FXE_ENABLE_NODE_COMPAT=1)
    endif()
    # Locate icudtl.dat so the runner can hand V8 a valid ICU data path. brew V8
    # currently keeps it in the cellar libexec; allow override via FXE_V8_ICUDTL.
    set(FXE_V8_ICUDTL "" CACHE FILEPATH "Override path to V8 icudtl.dat")
    set(_v8_icudtl_dat "${FXE_V8_ICUDTL}")
    if(NOT _v8_icudtl_dat)
        find_file(
            _v8_icudtl_dat
            icudtl.dat
            HINTS
            ENV V8_DIR
            /opt/homebrew/opt/v8/libexec
            /opt/homebrew/share/v8
            /usr/local/opt/v8/libexec
            PATH_SUFFIXES libexec share
        )
        if(NOT _v8_icudtl_dat AND EXISTS /opt/homebrew/Cellar/v8)
            file(
                GLOB _v8_brew_cells
                "/opt/homebrew/Cellar/v8/*/libexec/icudtl.dat"
            )
            list(GET _v8_brew_cells 0 _v8_icudtl_dat)
        endif()
    endif()
    if(_v8_icudtl_dat)
        message(STATUS "fxe: using V8 icudtl.dat at ${_v8_icudtl_dat}")
    else()
        message(
            WARNING
            "fxe: icudtl.dat not located; ICU-dependent JS may fail at runtime. Set FXE_V8_ICUDTL or FXE_V8_ICUDTL env var."
        )
    endif()

    add_executable(
        fxe_run
        src/runner/fxe_run.cpp
        src/runner/cpu_profile_native.cpp
        src/runner/cpu_profile_merge.cpp
    )
    target_link_libraries(fxe_run PRIVATE fxe_js)
    target_link_libraries(fxe_js PUBLIC fxe_debug)
    if(TARGET fxe_wgpu)
        # When wgpu is enabled, fxe_wgpu provides create_renderer +
        # offscreen_renderer + pipeline. Tests and fxe_run that link
        # fxe_js need these symbols transitively.
        target_link_libraries(fxe_js PUBLIC fxe_wgpu)
    endif()
    if(TARGET fxe_wgpu)
        # When wgpu is enabled, fxe_wgpu provides create_renderer (defines
        # FXE_HAS_WGPU on fxe_window). Pull it in so fxe_run gets a real GPU
        # backend instead of the null_renderer fallback.
        target_link_libraries(fxe_run PRIVATE fxe_wgpu)
    endif()
    if(_v8_icudtl_dat)
        target_compile_definitions(
            fxe_run
            PRIVATE FXE_V8_ICUDTL_PATH="${_v8_icudtl_dat}"
        )
        add_custom_command(
            TARGET fxe_run
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "${_v8_icudtl_dat}"
                "$<TARGET_FILE_DIR:fxe_run>/icudtl.dat"
            COMMENT "Staging icudtl.dat next to fxe_run"
        )
    endif()
    install(TARGETS fxe_run RUNTIME DESTINATION bin)
    include(cmake/install.cmake)
    install(FILES types/fxe.d.ts DESTINATION share/fxe/types)
endif()

if(FXE_BUILD_EXAMPLES)
    foreach(example hello_triangle hello_sprite primitives_showcase)
        add_executable(${example} examples/${example}.cpp)
        if(TARGET fxe_wgpu)
            target_link_libraries(${example} PRIVATE fxe_wgpu)
        else()
            target_link_libraries(${example} PRIVATE fxe_window)
        endif()
    endforeach()
    install(
        FILES
            examples/js/hello.ts
            examples/js/showcase.ts
            examples/js/loop.ts
            examples/js/bench.ts
            examples/js/ui_demo.ts
            examples/js/jsx_demo.tsx
            examples/js/ui_reconciler_demo.ts
            examples/js/ui_kit_demo.tsx
            examples/js/login_form.tsx
            examples/js/two_windows.ts
            examples/js/window_chat.ts
            examples/js/git_log.ts
        DESTINATION share/fxe/examples/js
    )
endif()

if(FXE_BUILD_TESTS)
    enable_testing()
    if(TARGET fxe-pack AND TARGET fxe_run)
        set(_fxe_packager_contract_entry
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/packager_contract_test.ts"
        )
        add_test(
            NAME fxe_packager_plain_contract_tests
            COMMAND
                $<TARGET_FILE:fxe-pack> "${_fxe_packager_contract_entry}" --out
                "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.bin"
        )
        set_tests_properties(
            fxe_packager_plain_contract_tests
            PROPERTIES ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
        )
        if(APPLE)
            find_program(_fxe_hdiutil hdiutil)
            if(_fxe_hdiutil)
                add_test(
                    NAME fxe_packager_dmg_contract_tests
                    COMMAND
                        $<TARGET_FILE:fxe-pack>
                        "${_fxe_packager_contract_entry}" --platform macos --out
                        "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.dmg"
                )
                set_tests_properties(
                    fxe_packager_dmg_contract_tests
                    PROPERTIES ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
                )
            endif()
        endif()
        if(NOT WIN32)
            add_test(
                NAME fxe_packager_msi_requires_windows_tests
                COMMAND
                    $<TARGET_FILE:fxe-pack> "${_fxe_packager_contract_entry}"
                    --platform win --out
                    "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.msi"
            )
            set_tests_properties(
                fxe_packager_msi_requires_windows_tests
                PROPERTIES
                    ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
                    WILL_FAIL TRUE
                    PASS_REGULAR_EXPRESSION
                        "\\.msi output requires WiX tooling and can only be built on Windows hosts"
            )
            add_test(
                NAME fxe_packager_msix_requires_cert_tests
                COMMAND
                    $<TARGET_FILE:fxe-pack> "${_fxe_packager_contract_entry}"
                    --platform win --out
                    "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.msix"
            )
            set_tests_properties(
                fxe_packager_msix_requires_cert_tests
                PROPERTIES
                    ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
                    WILL_FAIL TRUE
                    PASS_REGULAR_EXPRESSION
                        "\\.msix output requires --cert because MSIX packages must be signed"
            )
        endif()
    endif()
    # ---------------------------------------------------------------------
    # Test helpers.
    # ---------------------------------------------------------------------
    function(fxe_add_cpp_test target source)
        add_executable(${target} ${source})
        if(ARGN)
            target_link_libraries(${target} PRIVATE ${ARGN})
        endif()
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    # typescript_tests.cpp runner; always prepends typescript_smoke.ts and
    # typescript_modules_smoke.ts to the script list. Extra script paths are
    # appended (relative to CMAKE_CURRENT_SOURCE_DIR).
    function(fxe_add_v8_ts_test target)
        if(NOT TARGET fxe_js)
            return()
        endif()
        add_executable(${target} tests/typescript_tests.cpp)
        target_link_libraries(${target} PRIVATE fxe_js)
        if(TARGET fxe_wgpu)
            target_link_libraries(${target} PRIVATE fxe_wgpu)
        endif()
        if(_v8_icudtl_dat)
            target_compile_definitions(
                ${target}
                PRIVATE FXE_V8_ICUDTL_PATH="${_v8_icudtl_dat}"
            )
        endif()
        set(_scripts
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/typescript_smoke.ts"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/typescript_modules_smoke.ts"
        )
        foreach(_s IN LISTS ARGN)
            list(APPEND _scripts "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
        endforeach()
        add_test(NAME ${target} COMMAND ${target} ${_scripts})
    endfunction()

    # ---------------------------------------------------------------------
    # C++ tests.
    # ---------------------------------------------------------------------
    fxe_add_cpp_test(fxe_core_tests tests/core_tests.cpp fxe_core)
    fxe_add_cpp_test(fxe_font_tests tests/font_module_test.cpp fxe_font)
    fxe_add_cpp_test(fxe_font_render_tests tests/font_render_test.cpp fxe_font)
    fxe_add_cpp_test(fxe_uv_loop_tests tests/uv_loop_test.cpp fxe_runtime)
    if(UNIX)
        fxe_add_cpp_test(fxe_net_http_advanced_tests tests/net_http_advanced_test.cpp fxe_net fxe_runtime)
    endif()
    if(TARGET fxe_wgpu)
        fxe_add_cpp_test(fxe_wgpu_pipeline_cache_tests tests/wgpu_pipeline_cache_test.cpp fxe_wgpu)
        fxe_add_cpp_test(fxe_wgpu_blur_smoke tests/wgpu_blur_smoke.cpp fxe_wgpu)
    endif()
    if(TARGET fxe_js)
        fxe_add_cpp_test(
            fxe_uv_microtask_flush_tests
            tests/uv_microtask_flush_test.cpp
            fxe_js
        )
        if(_v8_icudtl_dat)
            target_compile_definitions(
                fxe_uv_microtask_flush_tests
                PRIVATE FXE_V8_ICUDTL_PATH="${_v8_icudtl_dat}"
            )
        endif()
    endif()
    if(UNIX AND NOT APPLE)
        fxe_add_cpp_test(
            fxe_os_linux_smoke_tests
            tests/os_linux_smoke.cpp
            fxe_os
        )
    endif()
    if(FXE_ENABLE_NATIVE_TLS_HTTP2)
        fxe_add_cpp_test(
            fxe_native_tls_tests
            tests/native_tls_test.cpp
            fxe_net
            MbedTLS::mbedtls
            MbedTLS::mbedx509
            MbedTLS::mbedcrypto
        )
        fxe_add_cpp_test(
            fxe_ws_deflate_tests
            tests/ws_deflate_test.cpp
            fxe_net
            ZLIB::ZLIB
        )
        target_sources(fxe_ws_deflate_tests PRIVATE src/runtime/uv_loop.cpp)
    endif()

    # Debug-protocol C++ tests prefer fxe_js (Runtime.* handlers) when V8 is
    # enabled and fall back to fxe_debug otherwise.
    foreach(
        _entry
        IN
        ITEMS
            "fxe_debug_tests=tests/debug_protocol_tests.cpp"
            "fxe_debug_cdp_ws_tests=tests/debug_cdp_ws_test.cpp"
    )
        string(REPLACE "=" ";" _pair "${_entry}")
        list(GET _pair 0 _name)
        list(GET _pair 1 _src)
        add_executable(${_name} ${_src})
        if(TARGET fxe_js)
            target_link_libraries(${_name} PRIVATE fxe_js)
        else()
            target_link_libraries(${_name} PRIVATE fxe_debug)
        endif()
        if(TARGET fxe_wgpu)
            target_link_libraries(${_name} PRIVATE fxe_wgpu)
        endif()
        add_test(NAME ${_name} COMMAND ${_name})
    endforeach()

    if(TARGET fxe_run AND TARGET fxe_wgpu)
        add_test(
            NAME fxe_run_cpu_prof_screenshot_exit
            COMMAND
                fxe_run
                --cpu-prof=${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_screenshot.cpuprofile
                --cpu-prof-md=${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_screenshot.md
                --cpu-prof-hz=500
                --screenshot=${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_screenshot.png
                --screenshot-delay=50
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/js/hello.ts
        )
    endif()

    # ---------------------------------------------------------------------
    # V8 TypeScript tests.
    # Convention: every tests/*_test.ts becomes a target fxe_<stem> running
    # that single script (the smokes are prepended by the helper). To bundle
    # multiple scripts under one target, drop the per-script auto-name and
    # add an explicit fxe_add_v8_ts_test() call.
    # ---------------------------------------------------------------------
    if(TARGET fxe_js)
        fxe_add_v8_ts_test(fxe_ts_tests)
        fxe_add_v8_ts_test(fxe_ui_box_shadow_blur_test tests/ui_box_shadow_blur_test.tsx)
        file(
            GLOB _v8_ts_test_scripts
            RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
            CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.ts"
        )
        if(NOT FXE_ENABLE_NATIVE_TLS_HTTP2)
            # node_compat_http2_test.ts requires the native HTTP/2 stack.
            list(
                FILTER _v8_ts_test_scripts
                EXCLUDE
                REGEX "node_compat_http2_test\\.ts$"
            )
        endif()
        foreach(_script IN LISTS _v8_ts_test_scripts)
            get_filename_component(_stem "${_script}" NAME_WE)
            fxe_add_v8_ts_test("fxe_${_stem}" "${_script}")
        endforeach()
    endif()
    foreach(example hello_triangle hello_sprite primitives_showcase)
        if(TARGET ${example})
            add_test(NAME fxe_example_${example} COMMAND ${example})
            set_tests_properties(
                fxe_example_${example}
                PROPERTIES LABELS "examples;smoke"
            )
        endif()
    endforeach()
endif()
