if(FXE_ENABLE_V8)
    find_package(V8 REQUIRED)
    # native_tls/http2/https sources moved under src/runtime/v8/native/ and
    # are picked up by fxe_js below when FXE_ENABLE_NATIVE_TLS_HTTP2 is set.
    find_package(MbedTLS CONFIG REQUIRED)
    find_program(NPM_EXECUTABLE npm REQUIRED)
    set(_fxe_typescript_js
        "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/typescript/lib/typescript.js"
    )
    if(NOT EXISTS "${_fxe_typescript_js}")
        add_custom_command(
            OUTPUT "${_fxe_typescript_js}"
            COMMAND "${NPM_EXECUTABLE}" ci
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            DEPENDS package.json package-lock.json
            COMMENT "Installing TypeScript compiler package"
        )
    endif()

    set(_fxe_js_generated_dir
        "${CMAKE_CURRENT_BINARY_DIR}/generated/fxe/generated"
    )
    set(_fxe_typescript_compiler_h
        "${_fxe_js_generated_dir}/typescript_compiler.hpp"
    )
    set(_fxe_types_h "${_fxe_js_generated_dir}/fxe_types.hpp")
    set(_fxe_unenv_asset_sources)
    if(FXE_ENABLE_NODE_COMPAT)
        include(FetchContent)
        FetchContent_Declare(
            unenv
            GIT_REPOSITORY https://github.com/unjs/unenv.git
            GIT_TAG v2.0.0-rc.24
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(unenv)
        FetchContent_Declare(
            pathe
            URL https://registry.npmjs.org/pathe/-/pathe-2.0.3.tgz
            URL_HASH
                SHA256=6e3b73bc5dbb7f8a108ff5d8459b07da04062e822c662339416525510696df36
        )
        FetchContent_MakeAvailable(pathe)
        set(_fxe_unenv_root "${unenv_SOURCE_DIR}")
        set(_fxe_pathe_dist_root "${pathe_SOURCE_DIR}/dist")
        set(_fxe_unenv_assets_h "${_fxe_js_generated_dir}/unenv_assets.hpp")
        list(APPEND _fxe_unenv_asset_sources "${_fxe_unenv_assets_h}")
        file(
            GLOB_RECURSE _fxe_unenv_runtime_sources
            CONFIGURE_DEPENDS
            "${_fxe_unenv_root}/src/runtime/*"
            "${_fxe_pathe_dist_root}/*"
        )
        add_custom_command(
            OUTPUT "${_fxe_unenv_assets_h}"
            COMMAND
                "${CMAKE_COMMAND}" -DUNENV_ROOT="${_fxe_unenv_root}"
                -DPATHE_DIST_ROOT="${_fxe_pathe_dist_root}"
                -DOUTPUT="${_fxe_unenv_assets_h}" -DNAMESPACE="fxe::runtime"
                -DVARIABLE="k_unenv_assets" -DDELIMITER="FXEUNENV" -P
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_unenv.cmake"
            DEPENDS ${_fxe_unenv_runtime_sources} cmake/embed_unenv.cmake
            COMMENT "Embedding unenv runtime assets"
        )
    endif()
    add_custom_command(
        OUTPUT "${_fxe_typescript_compiler_h}"
        COMMAND
            "${CMAKE_COMMAND}" -DINPUT="${_fxe_typescript_js}"
            -DOUTPUT="${_fxe_typescript_compiler_h}" -DNAMESPACE="fxe::js"
            -DVARIABLE="k_typescript_compiler_source" -DDELIMITER="FXETS" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text_header.cmake"
        DEPENDS "${_fxe_typescript_js}" cmake/embed_text_header.cmake
        COMMENT "Embedding TypeScript compiler"
    )
    add_custom_command(
        OUTPUT "${_fxe_types_h}"
        COMMAND
            "${CMAKE_COMMAND}"
            -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/types/fxe.d.ts"
            -DOUTPUT="${_fxe_types_h}" -DNAMESPACE="fxe::js"
            -DVARIABLE="k_fxe_types_source" -DDELIMITER="FXETYPES" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text_header.cmake"
        DEPENDS types/fxe.d.ts cmake/embed_text_header.cmake
        COMMENT "Embedding fxe TypeScript declarations"
    )

    set(_fxe_node_compat_scripts
        os_adapter
        tty_adapter
        crypto_adapter
        child_process_adapter
        fs_adapter
        fs_promises_adapter
        worker_threads_adapter
        net_adapter
        dgram_adapter
        dns_adapter
        dns_promises_adapter
        vm_adapter
        v8_adapter
        wasi_adapter
        inspector_adapter
        inspector_promises_adapter
        zlib_adapter
        tls_adapter
        tls_native_adapter
        https_adapter
        https_native_adapter
        http2_adapter
        http2_native_adapter
        events_adapter
        async_hooks_adapter
        buffer_adapter
        process_adapter
        path_adapter
        path_posix_adapter
        path_win32_adapter
        url_adapter
        querystring_adapter
        readline_adapter
        readline_promises_adapter
        repl_adapter
        util_adapter
        util_types_adapter
        console_adapter
        timers_adapter
        timers_promises_adapter
        stream_adapter
        stream_promises_adapter
        prelude
    )
    set(_fxe_node_compat_js_headers)
    foreach(_script IN LISTS _fxe_node_compat_scripts)
        set(_input
            "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/v8/node_compat_js/${_script}.js"
        )
        set(_output "${_fxe_js_generated_dir}/node_compat/${_script}.hpp")
        add_custom_command(
            OUTPUT "${_output}"
            COMMAND
                "${CMAKE_COMMAND}" -DINPUT="${_input}" -DOUTPUT="${_output}"
                -DNAMESPACE="fxe::runtime::node_js"
                -DVARIABLE="k_${_script}_source" -DDELIMITER="FXENODE" -P
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text_header.cmake"
            DEPENDS "${_input}" cmake/embed_text_header.cmake
            COMMENT "Embedding node compat script ${_script}"
        )
        list(APPEND _fxe_node_compat_js_headers "${_output}")
    endforeach()
    # fs_watcher backends live under src/runtime/v8/fs_watcher/<plat>.cpp.
    if(APPLE)
        file(
            GLOB _fxe_fs_watcher_source
            CONFIGURE_DEPENDS
            src/runtime/v8/fs_watcher/macos.cpp
        )
    elseif(WIN32)
        file(
            GLOB _fxe_fs_watcher_source
            CONFIGURE_DEPENDS
            src/runtime/v8/fs_watcher/win32.cpp
        )
    else()
        file(
            GLOB _fxe_fs_watcher_source
            CONFIGURE_DEPENDS
            src/runtime/v8/fs_watcher/linux.cpp
        )
    endif()
    # fxe_js: bindings (src/js/*.cpp) + V8-host runtime (src/runtime/v8/*.cpp,
    # plus the platform-selected fs_watcher above). The runner exec lives in
    # src/runner/ so it is not picked up here.
    file(GLOB _fxe_js_sources CONFIGURE_DEPENDS src/js/*.cpp)
    list(APPEND _fxe_js_sources src/js/bind_net.cpp src/js/bind_os.cpp)
    list(REMOVE_DUPLICATES _fxe_js_sources)
    file(GLOB _fxe_runtime_v8_sources CONFIGURE_DEPENDS src/runtime/v8/*.cpp)
    list(
        APPEND _fxe_runtime_v8_sources
        src/runtime/v8/native/v8_module.cpp
        src/runtime/v8/native/async_hooks.cpp
        src/runtime/v8/native/inspector.cpp
        src/runtime/v8/native/vm.cpp
    )
    if(FXE_ENABLE_NATIVE_TLS_HTTP2)
        file(
            GLOB _fxe_runtime_v8_native
            CONFIGURE_DEPENDS
            src/runtime/v8/native/*.cpp
        )
        list(
            FILTER _fxe_runtime_v8_native
            EXCLUDE
            REGEX "src/runtime/v8/native/(v8_module|https_transport)\\.cpp$"
        )
        list(APPEND _fxe_runtime_v8_sources ${_fxe_runtime_v8_native})
    endif()
    add_library(
        fxe_js
        STATIC
        ${_fxe_js_sources}
        ${_fxe_runtime_v8_sources}
        ${_fxe_fs_watcher_source}
        "${_fxe_typescript_compiler_h}"
        "${_fxe_types_h}"
        ${_fxe_unenv_asset_sources}
        ${_fxe_node_compat_js_headers}
    )
    add_library(fxe::js ALIAS fxe_js)
    target_include_directories(
        fxe_js
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_link_libraries(fxe_js PUBLIC fxe_window V8::V8)
    target_link_libraries(fxe_js PUBLIC fxe_os fxe_net fxe_audio fxe_runtime)
    target_link_libraries(fxe_js PUBLIC fxe_image)
    if(TARGET fxe_webauthn)
        target_link_libraries(fxe_js PUBLIC fxe_webauthn)
        target_compile_definitions(fxe_js PUBLIC FXE_HAS_WEBAUTHN=1)
    endif()
    target_link_libraries(fxe_js PUBLIC fxe_markdown)
    target_link_libraries(fxe_js PUBLIC fxe_layout)
    target_link_libraries(fxe_js PUBLIC MbedTLS::mbedcrypto)
    target_link_libraries(fxe_js PRIVATE unofficial-sodium::sodium)
    target_link_libraries(fxe_js PRIVATE unofficial::sqlite3::sqlite3)
    if(WIN32)
        target_link_libraries(fxe_js PUBLIC bcrypt ws2_32)
    endif()
    target_compile_definitions(fxe_js PUBLIC FXE_HAS_V8=1)
    set_source_files_properties(
        src/js/typescript.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings"
    )
    target_compile_definitions(
        fxe_js
        PRIVATE FXE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(FXE_ENABLE_NODE_COMPAT)
        target_compile_definitions(fxe_js PUBLIC FXE_ENABLE_NODE_COMPAT=1)
    endif()
    # Locate icudtl.dat so the runner can hand V8 a valid ICU data path. brew V8
    # currently keeps it in the cellar libexec; allow override via FXE_V8_ICUDTL.
    set(FXE_V8_ICUDTL "" CACHE FILEPATH "Override path to V8 icudtl.dat")
    set(_v8_icudtl_dat "${FXE_V8_ICUDTL}")
    if(NOT _v8_icudtl_dat)
        find_file(
            _v8_icudtl_dat
            icudtl.dat
            HINTS
            ENV V8_DIR
            /opt/homebrew/opt/v8/libexec
            /opt/homebrew/share/v8
            /usr/local/opt/v8/libexec
            PATH_SUFFIXES libexec share
        )
        if(NOT _v8_icudtl_dat AND EXISTS /opt/homebrew/Cellar/v8)
            file(
                GLOB _v8_brew_cells
                "/opt/homebrew/Cellar/v8/*/libexec/icudtl.dat"
            )
            list(GET _v8_brew_cells 0 _v8_icudtl_dat)
        endif()
    endif()
    if(_v8_icudtl_dat)
        message(STATUS "fxe: using V8 icudtl.dat at ${_v8_icudtl_dat}")
    else()
        message(
            WARNING
            "fxe: icudtl.dat not located; ICU-dependent JS may fail at runtime. Set FXE_V8_ICUDTL or FXE_V8_ICUDTL env var."
        )
    endif()

    add_executable(
        fxe_run
        src/runner/fxe_run.cpp
        src/runner/cpu_profile_native.cpp
        src/runner/cpu_profile_merge.cpp
    )
    target_link_libraries(fxe_run PRIVATE fxe_js)
    target_link_libraries(fxe_js PUBLIC fxe_debug)
    if(TARGET fxe_wgpu)
        # When wgpu is enabled, fxe_wgpu provides create_renderer +
        # offscreen_renderer + pipeline. Tests and fxe_run that link
        # fxe_js need these symbols transitively.
        target_link_libraries(fxe_js PUBLIC fxe_wgpu)
    endif()
    if(TARGET fxe_wgpu)
        # When wgpu is enabled, fxe_wgpu provides create_renderer (defines
        # FXE_HAS_WGPU on fxe_window). Pull it in so fxe_run gets a real GPU
        # backend instead of the null_renderer fallback.
        target_link_libraries(fxe_run PRIVATE fxe_wgpu)
    endif()
    if(_v8_icudtl_dat)
        target_compile_definitions(
            fxe_run
            PRIVATE FXE_V8_ICUDTL_PATH="${_v8_icudtl_dat}"
        )
        add_custom_command(
            TARGET fxe_run
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "${_v8_icudtl_dat}"
                "$<TARGET_FILE_DIR:fxe_run>/icudtl.dat"
            COMMENT "Staging icudtl.dat next to fxe_run"
        )
    endif()
    install(TARGETS fxe_run RUNTIME DESTINATION bin)
    include(cmake/install.cmake)
    install(FILES types/fxe.d.ts DESTINATION share/fxe/types)
endif()
