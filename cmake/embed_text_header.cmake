if(NOT DEFINED INPUT)
    message(FATAL_ERROR "embed_text_header: INPUT is required")
endif()
if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "embed_text_header: OUTPUT is required")
endif()
if(NOT DEFINED NAMESPACE)
    message(FATAL_ERROR "embed_text_header: NAMESPACE is required")
endif()
if(NOT DEFINED VARIABLE)
    message(FATAL_ERROR "embed_text_header: VARIABLE is required")
endif()
if(NOT DEFINED DELIMITER)
    set(DELIMITER "FXEEMBED")
endif()

string(LENGTH "${DELIMITER}" _delimiter_len)
if(_delimiter_len GREATER 16)
    message(
        FATAL_ERROR
        "embed_text_header: raw string delimiter must be 16 characters or fewer"
    )
endif()

file(READ "${INPUT}" _content)
string(FIND "${_content}" ")${DELIMITER}\"" _delimiter_collision)
if(NOT _delimiter_collision EQUAL -1)
    message(
        FATAL_ERROR
        "embed_text_header: ${INPUT} contains raw string delimiter collision for ${DELIMITER}"
    )
endif()

get_filename_component(_output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")
file(
    WRITE "${OUTPUT}"
    "#pragma once\n\n#include <string_view>\n\nnamespace ${NAMESPACE} {\n  inline constexpr std::string_view ${VARIABLE} = R\"${DELIMITER}(${_content})${DELIMITER}\";\n} // namespace ${NAMESPACE}\n"
)
