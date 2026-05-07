set(FXE_WGSL_VALIDATOR
    ""
    CACHE FILEPATH
    "WGSL validator executable used by embed_wgsl; if empty, CMake searches for Dawn/Tint tools."
)

function(embed_wgsl target shader)
    set(shader_path "${CMAKE_CURRENT_SOURCE_DIR}/${shader}")
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/fxe")
    set(out_cpp "${out_dir}/${target}.cpp")
    set(out_hpp "${out_dir}/${target}.hpp")
    file(MAKE_DIRECTORY "${out_dir}")
    file(READ "${shader_path}" shader_text HEX)
    string(LENGTH "${shader_text}" hex_len)
    math(EXPR byte_len "${hex_len} / 2")
    set(bytes "")
    foreach(i RANGE 0 ${hex_len} 2)
        if(i LESS hex_len)
            string(SUBSTRING "${shader_text}" ${i} 2 byte)
            string(APPEND bytes "0x${byte},")
        endif()
    endforeach()
    set(_hpp_content
        "#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace fxe::shaders { extern const std::uint8_t main_wgsl[]; extern const std::size_t main_wgsl_size; }\n"
    )
    set(_cpp_content
        "#include <cstddef>\n#include <cstdint>\nnamespace fxe::shaders { extern const std::uint8_t main_wgsl[] = {${bytes}0}; extern const std::size_t main_wgsl_size = ${byte_len}; }\n"
    )
    file(WRITE "${out_hpp}.tmp" "${_hpp_content}")
    file(WRITE "${out_cpp}.tmp" "${_cpp_content}")
    # Only update the real outputs when the content actually changed so we
    # don't bump mtimes on every reconfigure (would force a relink storm).
    file(COPY_FILE "${out_hpp}.tmp" "${out_hpp}" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${out_cpp}.tmp" "${out_cpp}" ONLY_IF_DIFFERENT)
    file(REMOVE "${out_hpp}.tmp" "${out_cpp}.tmp")

    add_library(${target} STATIC "${out_cpp}")
    target_include_directories(
        ${target}
        PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )

    set(_fxe_wgsl_validator "${FXE_WGSL_VALIDATOR}")
    if(NOT _fxe_wgsl_validator)
        find_program(
            _fxe_wgsl_validator
            NAMES tint tint.exe tint_cmd dawn_tint wgsl_validator
            HINTS
                "$ENV{DAWN_DIR}/bin"
                "$ENV{DAWN_DIR}/out/Release"
                "$ENV{DAWN_DIR}/out/Debug"
                "$ENV{DAWN_DIR}/out/Default"
                "${Dawn_DIR}/../../../bin"
                "${dawn_DIR}/../../../bin"
            DOC "WGSL validator executable used by fxe shader embedding"
        )
    endif()

    if(_fxe_wgsl_validator)
        set(validate_stamp "${out_dir}/${target}.wgsl.validated")
        add_custom_command(
            OUTPUT "${validate_stamp}"
            COMMAND "${_fxe_wgsl_validator}" "${shader_path}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${validate_stamp}"
            DEPENDS "${shader_path}"
            COMMENT "Validating WGSL shader ${shader}"
            VERBATIM
        )
        add_custom_target(${target}_validate_wgsl DEPENDS "${validate_stamp}")
        add_dependencies(${target} ${target}_validate_wgsl)
        message(
            STATUS
            "fxe: WGSL validation for ${target} uses ${_fxe_wgsl_validator}"
        )
    else()
        message(
            STATUS
            "fxe: no WGSL validator found for ${shader}; set FXE_WGSL_VALIDATOR to a Dawn/Tint validator executable to validate at build time."
        )
    endif()
endfunction()
