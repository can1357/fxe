set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

option(FXE_BUILD_EXAMPLES "Build fxe examples" ON)
option(FXE_BUILD_TESTS "Build fxe tests" ON)
option(FXE_ENABLE_WGPU "Build Dawn/WebGPU backend when Dawn is available" ON)
option(FXE_ENABLE_V8 "Build embedded V8 host/bindings when V8 is available" ON)
option(
    FXE_ENABLE_WEBAUTHN
    "Build the platform-agnostic WebAuthn core library"
    ${FXE_ENABLE_V8}
)
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
