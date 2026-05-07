include(FindPackageHandleStandardArgs)

set(_V8_HINTS
    "$ENV{V8_ROOT}"
    "$ENV{V8_DIR}"
    "/opt/homebrew/opt/v8"
    "/usr/local/opt/v8"
)

find_path(
    V8_INCLUDE_DIR
    NAMES v8.h v8-fast-api-calls.h
    HINTS ${_V8_HINTS}
    PATH_SUFFIXES include
)

find_library(V8_LIBRARY NAMES v8 HINTS ${_V8_HINTS} PATH_SUFFIXES lib)
find_library(
    V8_LIBBASE_LIBRARY
    NAMES v8_libbase
    HINTS ${_V8_HINTS}
    PATH_SUFFIXES lib
)
find_library(
    V8_LIBPLATFORM_LIBRARY
    NAMES v8_libplatform
    HINTS ${_V8_HINTS}
    PATH_SUFFIXES lib
)

find_package_handle_standard_args(
    V8
    REQUIRED_VARS
        V8_INCLUDE_DIR
        V8_LIBRARY
        V8_LIBBASE_LIBRARY
        V8_LIBPLATFORM_LIBRARY
)

if(V8_FOUND AND NOT TARGET V8::V8)
    add_library(V8::V8 INTERFACE IMPORTED)
    target_include_directories(V8::V8 INTERFACE "${V8_INCLUDE_DIR}")
    target_link_libraries(
        V8::V8
        INTERFACE
            "${V8_LIBRARY}"
            "${V8_LIBBASE_LIBRARY}"
            "${V8_LIBPLATFORM_LIBRARY}"
    )
    # Match the ABI of the V8 build we link against (brew/vendored both ship
    # libv8 with pointer compression enabled). Propagated to every consumer so
    # individual TUs no longer need to redefine V8_COMPRESS_POINTERS.
    target_compile_definitions(V8::V8 INTERFACE V8_COMPRESS_POINTERS=1)
endif()
