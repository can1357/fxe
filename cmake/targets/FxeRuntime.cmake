# Runtime support shared by the V8 runner and packager-facing embedder APIs.
# V8-host-only sources live under src/runtime/v8/ and are picked up by fxe_js.
file(GLOB _fxe_runtime_sources CONFIGURE_DEPENDS src/runtime/*.cpp)
list(APPEND _fxe_runtime_sources tools/fxe-pack/bundle.cpp)
add_library(fxe_runtime STATIC ${_fxe_runtime_sources})
add_library(fxe::runtime ALIAS fxe_runtime)
target_link_libraries(fxe_runtime PRIVATE unofficial-sodium::sodium)
find_package(pugixml CONFIG REQUIRED)
target_link_libraries(fxe_runtime PRIVATE pugixml::pugixml)
target_compile_features(fxe_runtime PUBLIC cxx_std_20)
target_include_directories(
    fxe_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
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
