if(FXE_ENABLE_WEBAUTHN)
    file(GLOB _fxe_webauthn_sources CONFIGURE_DEPENDS src/webauthn/*.cpp)
    list(
        FILTER _fxe_webauthn_sources
        EXCLUDE
        REGEX "/platform_(win32|linux|macos|stub)\\.cpp$"
    )
    add_library(fxe_webauthn STATIC ${_fxe_webauthn_sources})
    add_library(fxe::webauthn ALIAS fxe_webauthn)
    target_include_directories(fxe_webauthn PUBLIC include src)
    target_compile_features(fxe_webauthn PUBLIC cxx_std_20)
    target_link_libraries(
        fxe_webauthn
        PUBLIC fxe_runtime
        PRIVATE MbedTLS::mbedcrypto
    )
    target_link_libraries(fxe_webauthn PRIVATE unofficial::sqlite3::sqlite3)
    target_compile_definitions(fxe_webauthn PUBLIC FXE_HAS_WEBAUTHN=1)
    target_link_libraries(fxe_debug PUBLIC fxe_webauthn)
    target_compile_definitions(fxe_debug PUBLIC FXE_DEBUG_HAS_WEBAUTHN=1)
    if(APPLE)
        set(_fxe_webauthn_platform src/webauthn/platform_macos.mm)
        target_sources(fxe_webauthn PRIVATE ${_fxe_webauthn_platform})
        set_source_files_properties(
            ${_fxe_webauthn_platform}
            PROPERTIES
                COMPILE_FLAGS
                    "-x objective-c++ -fobjc-arc -Wno-old-style-cast -Wno-gnu-conditional-omitted-operand"
        )
        target_link_libraries(
            fxe_webauthn
            PRIVATE
                "-framework AppKit"
                "-framework Foundation"
                "-framework AuthenticationServices"
                "-framework LocalAuthentication"
        )
    elseif(WIN32)
        target_sources(fxe_webauthn PRIVATE src/webauthn/platform_win32.cpp)
    elseif(UNIX)
        find_package(PkgConfig QUIET)
        set(_fxe_libfido2_found FALSE)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBFIDO2 IMPORTED_TARGET libfido2)
            if(LIBFIDO2_FOUND)
                set(_fxe_libfido2_found TRUE)
            endif()
        endif()
        if(_fxe_libfido2_found)
            target_sources(fxe_webauthn PRIVATE src/webauthn/platform_linux.cpp)
            target_link_libraries(fxe_webauthn PRIVATE PkgConfig::LIBFIDO2)
            target_compile_definitions(fxe_webauthn PRIVATE FXE_HAS_LIBFIDO2=1)
            message(STATUS "fxe_webauthn: libfido2 backend enabled")
        else()
            target_sources(fxe_webauthn PRIVATE src/webauthn/platform_stub.cpp)
            message(
                STATUS
                "fxe_webauthn: libfido2 not found via pkg-config; using stub backend"
            )
        endif()
    else()
        target_sources(fxe_webauthn PRIVATE src/webauthn/platform_stub.cpp)
    endif()
endif()
