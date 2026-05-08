# fxe-pack — single-file packager CLI.
#
# Builds a tiny tool that takes a TypeScript entry and emits a platform
# bundle (.app / .exe / .AppDir) carrying the script + assets appended to a
# copy of `fxe_run`. Sources live under tools/fxe-pack/ but the target is
# declared here to keep all build wiring under cmake/.

if(FXE_BUILD_PACKAGER AND NOT TARGET fxe-pack)
    set(_fxe_pack_dir "${CMAKE_SOURCE_DIR}/tools/fxe-pack")
    add_executable(
        fxe-pack
        "${_fxe_pack_dir}/main.cpp"
        "${_fxe_pack_dir}/bundle.cpp"
    )
    target_compile_features(fxe-pack PRIVATE cxx_std_20)
    target_include_directories(
        fxe-pack
        PRIVATE "${_fxe_pack_dir}" "${CMAKE_SOURCE_DIR}/include"
    )
    if(APPLE)
        target_link_libraries(fxe-pack PRIVATE "-framework CoreFoundation")
    endif()
    install(TARGETS fxe-pack RUNTIME DESTINATION bin)
    # Installs all packaging templates (.plist/.wxs/AppRun), including macOS entitlements.
    install(
        DIRECTORY "${_fxe_pack_dir}/templates/"
        DESTINATION share/fxe/fxe-pack/templates
        FILES_MATCHING
        PATTERN "*.in"
    )
endif()
