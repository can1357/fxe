# cmake/install.cmake — install rules and packager wiring for fxe.
#
# Included from the top-level CMakeLists.txt. Adds an option to control
# whether the packager (`fxe-pack`) is built, wires its install rules, and
# also exposes the runtime bundle loader as a small static library that
# `fxe_run` (and any embedder) can link against.

option(FXE_BUILD_PACKAGER "Build the fxe-pack single-file packager" ON)

# Runtime-side bundle mounting. Pulled in by fxe_run via the integration
# subagent's edit; safe to add unconditionally because it has no external
# dependencies.
if(NOT TARGET fxe_runtime)
    add_library(
        fxe_runtime
        STATIC
        src/runtime/bundle_loader.cpp
        tools/fxe-pack/bundle.cpp
    )
    add_library(fxe::runtime ALIAS fxe_runtime)
    target_compile_features(fxe_runtime PUBLIC cxx_std_20)
    target_include_directories(
        fxe_runtime
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/fxe-pack"
    )
endif()

# Link runtime into fxe_run if it has been declared already (FXE_ENABLE_V8 path).
if(TARGET fxe_run)
    target_link_libraries(fxe_run PRIVATE fxe_runtime)
endif()
if(TARGET fxe_js)
    # typescript.cpp's resolver calls fxe::runtime::read_virtual.
    target_link_libraries(fxe_js PUBLIC fxe_runtime)
endif()

install(FILES src/runtime/bundle_loader.hpp DESTINATION include/fxe/runtime)
install(FILES tools/fxe-pack/bundle.hpp DESTINATION include/fxe/bundle)
