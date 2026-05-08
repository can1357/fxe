# fxe_debug — out-of-process debugger / control-plane server (NDJSON over TCP).
# Always available; FXE_HAS_V8 unlocks Runtime.* handlers when fxe_js is linked.
# Stubs under src/debug/stubs/ are pulled in only when V8 is disabled.
file(GLOB _fxe_debug_sources CONFIGURE_DEPENDS src/debug/*.cpp)
add_library(fxe_debug STATIC ${_fxe_debug_sources})
if(NOT FXE_ENABLE_V8)
    # Stubs for fxe::js::host accessor symbols that fxe_debug references but
    # never invokes (call sites guard on cx.host != nullptr). Only compiled in
    # builds without V8; otherwise the real implementations live in fxe_js.
    file(GLOB _fxe_debug_stubs CONFIGURE_DEPENDS src/debug/stubs/*.cpp)
    target_sources(fxe_debug PRIVATE ${_fxe_debug_stubs})
endif()
add_library(fxe::debug ALIAS fxe_debug)
target_link_libraries(fxe_debug PUBLIC fxe_window)
target_compile_features(fxe_debug PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(fxe_debug PUBLIC Threads::Threads)
target_link_libraries(fxe_debug PRIVATE MbedTLS::mbedcrypto)
target_link_libraries(fxe_debug PRIVATE unofficial-sodium::sodium)
if(WIN32)
    target_link_libraries(fxe_debug PUBLIC ws2_32)
endif()

if(FXE_ENABLE_V8 OR FXE_ENABLE_WEBAUTHN)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
endif()
