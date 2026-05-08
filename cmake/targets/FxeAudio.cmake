# Audio — miniaudio-backed engine.
add_library(fxe_audio STATIC src/audio/audio.cpp)
target_include_directories(fxe_audio SYSTEM PUBLIC ${FXE_MINIAUDIO_INCLUDE_DIR})
target_include_directories(fxe_audio PUBLIC src include)
target_compile_features(fxe_audio PUBLIC cxx_std_20)
if(APPLE)
    target_link_libraries(
        fxe_audio
        PUBLIC
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework AudioUnit"
            "-framework CoreFoundation"
    )
endif()
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(fxe_audio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
