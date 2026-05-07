if(NOT DEFINED UNENV_ROOT)
    message(FATAL_ERROR "embed_unenv: UNENV_ROOT is required")
endif()
if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "embed_unenv: OUTPUT is required")
endif()
if(NOT DEFINED NAMESPACE)
    message(FATAL_ERROR "embed_unenv: NAMESPACE is required")
endif()
if(NOT DEFINED VARIABLE)
    message(FATAL_ERROR "embed_unenv: VARIABLE is required")
endif()
if(NOT DEFINED DELIMITER)
    set(DELIMITER "FXEUNENV")
endif()

string(LENGTH "${DELIMITER}" _delimiter_len)
if(_delimiter_len GREATER 16)
    message(
        FATAL_ERROR
        "embed_unenv: raw string delimiter must be 16 characters or fewer"
    )
endif()

set(_runtime_root "${UNENV_ROOT}/src/runtime")
if(NOT DEFINED PATHE_DIST_ROOT)
    set(PATHE_DIST_ROOT "${UNENV_ROOT}/node_modules/pathe/dist")
endif()
set(_pathe_dist_root "${PATHE_DIST_ROOT}")
if(NOT IS_DIRECTORY "${_runtime_root}")
    message(FATAL_ERROR "embed_unenv: runtime root not found: ${_runtime_root}")
endif()
if(NOT IS_DIRECTORY "${_pathe_dist_root}")
    message(
        FATAL_ERROR
        "embed_unenv: pathe dist root not found: ${_pathe_dist_root}"
    )
endif()

file(GLOB_RECURSE _unenv_inputs "${_runtime_root}/*")
file(GLOB_RECURSE _pathe_inputs "${_pathe_dist_root}/*")
list(SORT _unenv_inputs)
list(SORT _pathe_inputs)

set(_entries "")
set(_input_count 0)
foreach(_input IN LISTS _unenv_inputs _pathe_inputs)
    if(IS_DIRECTORY "${_input}")
        continue()
    endif()

    if(_input MATCHES "^${_pathe_dist_root}/")
        file(RELATIVE_PATH _rel_inside "${_pathe_dist_root}" "${_input}")
        set(_relative "node_modules/pathe/dist/${_rel_inside}")
    else()
        file(RELATIVE_PATH _relative "${UNENV_ROOT}" "${_input}")
    endif()
    file(READ "${_input}" _content)
    string(FIND "${_content}" ")${DELIMITER}\"" _delimiter_collision)
    if(NOT _delimiter_collision EQUAL -1)
        message(
            FATAL_ERROR
            "embed_unenv: ${_input} contains raw string delimiter collision for ${DELIMITER}"
        )
    endif()

    math(EXPR _input_count "${_input_count} + 1")
    string(
        APPEND _entries
        "  {\"${_relative}\", R\"${DELIMITER}(${_content})${DELIMITER}\"},\n"
    )
endforeach()

get_filename_component(_output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")
file(
    WRITE "${OUTPUT}"
    "#pragma once\n\n#include <array>\n#include <string_view>\n\nnamespace ${NAMESPACE} {\n  struct unenv_asset {\n    std::string_view path;   // Relative to vendor/unenv (for example, src/runtime/node/path.ts).\n    std::string_view source;\n  };\n\n  inline constexpr std::array<unenv_asset, "
)
file(
    APPEND "${OUTPUT}"
    "${_input_count}> ${VARIABLE}{{\n${_entries}  }};\n} // namespace ${NAMESPACE}\n"
)
