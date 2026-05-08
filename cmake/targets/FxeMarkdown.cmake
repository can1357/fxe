# fxe_markdown — Markdown parser bridge over md4c. Static library used by
# fxe_js (for the `Markdown` JS binding) and by the markdown UI component.
file(GLOB _fxe_markdown_sources CONFIGURE_DEPENDS src/markdown/*.cpp)
add_library(fxe_markdown STATIC ${_fxe_markdown_sources})
add_library(fxe::markdown ALIAS fxe_markdown)
target_include_directories(fxe_markdown PUBLIC include src)
target_compile_features(fxe_markdown PUBLIC cxx_std_20)
if(TARGET md4c::md4c)
    target_link_libraries(fxe_markdown PRIVATE md4c::md4c)
else()
    target_link_libraries(fxe_markdown PRIVATE md4c)
endif()
