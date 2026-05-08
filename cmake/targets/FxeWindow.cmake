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

if(WIN32)
    target_sources(fxe_window PRIVATE src/window/ime_win32.cpp)
    target_link_libraries(fxe_window PRIVATE imm32)
endif()
if(UNIX AND NOT APPLE AND FXE_OS_DBUS)
    target_sources(fxe_window PRIVATE src/window/ime_linux.cpp)
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
