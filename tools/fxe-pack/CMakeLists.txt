# fxe-pack — single-file packager CLI.
#
# Builds a tiny tool that takes a TypeScript entry and emits a platform
# bundle (.app / .exe / .AppDir) carrying the script + assets appended to a
# copy of `fxe_run`.

add_executable(fxe-pack
  main.cpp
  bundle.cpp
)
target_compile_features(fxe-pack PRIVATE cxx_std_20)
target_include_directories(fxe-pack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/include")

if(APPLE)
  target_link_libraries(fxe-pack PRIVATE "-framework CoreFoundation")
endif()

install(TARGETS fxe-pack RUNTIME DESTINATION bin)
# Installs all packaging templates (.plist/.wxs/AppRun), including macOS entitlements.
install(DIRECTORY templates/
        DESTINATION share/fxe/fxe-pack/templates
        FILES_MATCHING PATTERN "*.in")
