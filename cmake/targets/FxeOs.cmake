# OS shims (App, shell, dialog, notification, menu, tray, globalShortcut).
set(_fxe_os_sources
    src/os/a11y.cpp
    src/os/crash_common.cpp
    src/os/menu_handler.cpp
    src/os/single_instance.cpp
)
add_library(fxe_os STATIC ${_fxe_os_sources})
target_include_directories(fxe_os PUBLIC src include)
target_link_libraries(fxe_os PUBLIC fxe_deps fxe_log)
target_compile_features(fxe_os PUBLIC cxx_std_20)
if(APPLE)
    set(_fxe_os_platform
        src/os/macos/a11y_macos.mm
        src/os/macos/crash.mm
        src/os/macos/os_macos.mm
        src/os/macos/power.mm
    )
    target_sources(fxe_os PRIVATE ${_fxe_os_platform})
    set_source_files_properties(
        ${_fxe_os_platform}
        PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc"
    )
    target_link_libraries(
        fxe_os
        PUBLIC
            "-framework AppKit"
            "-framework Foundation"
            "-framework UserNotifications"
            "-framework UniformTypeIdentifiers"
            "-framework Carbon"
            "-framework IOKit"
            "-framework Network"
            "-framework SystemConfiguration"
            "-framework CoreGraphics"
    )
elseif(WIN32)
    file(GLOB _fxe_os_platform CONFIGURE_DEPENDS src/os/win32/*.cpp)
    target_sources(fxe_os PRIVATE ${_fxe_os_platform})
    target_link_libraries(fxe_os PUBLIC Wininet Wtsapi32 Dbghelp Winhttp)
else()
    file(GLOB _fxe_os_platform CONFIGURE_DEPENDS src/os/linux/*.cpp)
    target_sources(fxe_os PRIVATE ${_fxe_os_platform})
    if(FXE_HAS_XSS)
        target_link_libraries(fxe_os PUBLIC X11::X11 X11::Xss)
        target_compile_definitions(fxe_os PUBLIC FXE_HAS_XSS=1)
    else()
        target_compile_definitions(fxe_os PUBLIC FXE_HAS_XSS=0)
    endif()
    if(FXE_OS_DBUS)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
        endif()
        if(DBUS_FOUND)
            find_package(Threads REQUIRED)
            target_link_libraries(
                fxe_os
                PUBLIC PkgConfig::DBUS Threads::Threads
            )
            target_compile_definitions(fxe_os PUBLIC FXE_HAS_DBUS=1)
        else()
            message(
                STATUS
                "libdbus-1 not found; Linux desktop D-Bus integrations disabled"
            )
            target_compile_definitions(fxe_os PUBLIC FXE_HAS_DBUS=0)
        endif()
    else()
        target_compile_definitions(fxe_os PUBLIC FXE_HAS_DBUS=0)
    endif()
endif()
