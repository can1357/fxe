if(FXE_BUILD_TESTS)
    enable_testing()
    if(TARGET fxe-pack AND TARGET fxe_run)
        set(_fxe_packager_contract_entry
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/packager_contract_test.ts"
        )
        add_test(
            NAME fxe_packager_plain_contract_tests
            COMMAND
                $<TARGET_FILE:fxe-pack> "${_fxe_packager_contract_entry}" --out
                "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.bin"
        )
        set_tests_properties(
            fxe_packager_plain_contract_tests
            PROPERTIES ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
        )
        if(APPLE)
            find_program(_fxe_hdiutil hdiutil)
            if(_fxe_hdiutil)
                add_test(
                    NAME fxe_packager_dmg_contract_tests
                    COMMAND
                        $<TARGET_FILE:fxe-pack>
                        "${_fxe_packager_contract_entry}" --platform macos --out
                        "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.dmg"
                )
                set_tests_properties(
                    fxe_packager_dmg_contract_tests
                    PROPERTIES ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
                )
            endif()
            add_executable(
                fxe_pack_webauthn_entitlement_tests
                tests/fxe_pack_webauthn_entitlement_test.cpp
            )
            target_compile_features(
                fxe_pack_webauthn_entitlement_tests
                PRIVATE cxx_std_20
            )
            add_dependencies(fxe_pack_webauthn_entitlement_tests fxe-pack)
            add_test(
                NAME fxe_pack_webauthn_entitlement_tests
                COMMAND fxe_pack_webauthn_entitlement_tests
            )
            set_tests_properties(
                fxe_pack_webauthn_entitlement_tests
                PROPERTIES
                    ENVIRONMENT
                        "FXE_PACK=$<TARGET_FILE:fxe-pack>;FXE_RUN=$<TARGET_FILE:fxe_run>"
            )
        endif()
        if(NOT WIN32)
            add_test(
                NAME fxe_packager_msi_requires_windows_tests
                COMMAND
                    $<TARGET_FILE:fxe-pack> "${_fxe_packager_contract_entry}"
                    --platform win --out
                    "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.msi"
            )
            set_tests_properties(
                fxe_packager_msi_requires_windows_tests
                PROPERTIES
                    ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
                    WILL_FAIL TRUE
                    PASS_REGULAR_EXPRESSION
                        "\\.msi output requires WiX tooling and can only be built on Windows hosts"
            )
            add_test(
                NAME fxe_packager_msix_requires_cert_tests
                COMMAND
                    $<TARGET_FILE:fxe-pack> "${_fxe_packager_contract_entry}"
                    --platform win --out
                    "${CMAKE_CURRENT_BINARY_DIR}/fxe-packager-contract.msix"
            )
            set_tests_properties(
                fxe_packager_msix_requires_cert_tests
                PROPERTIES
                    ENVIRONMENT "FXE_RUN=$<TARGET_FILE:fxe_run>"
                    WILL_FAIL TRUE
                    PASS_REGULAR_EXPRESSION
                        "\\.msix output requires --cert because MSIX packages must be signed"
            )
        endif()
    endif()
    # ---------------------------------------------------------------------
    # Test helpers.
    # ---------------------------------------------------------------------
    function(fxe_add_cpp_test target source)
        add_executable(${target} ${source})
        if(ARGN)
            target_link_libraries(${target} PRIVATE ${ARGN})
        endif()
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    # typescript_tests.cpp runner; always prepends typescript_smoke.ts and
    # typescript_modules_smoke.ts to the script list. Extra script paths are
    # appended (relative to CMAKE_CURRENT_SOURCE_DIR).
    function(fxe_add_v8_ts_test target)
        if(NOT TARGET fxe_js)
            return()
        endif()
        add_executable(${target} tests/typescript_tests.cpp)
        target_link_libraries(${target} PRIVATE fxe_js)
        if(TARGET fxe_wgpu)
            target_link_libraries(${target} PRIVATE fxe_wgpu)
        endif()
        if(_v8_icudtl_dat)
            target_compile_definitions(
                ${target}
                PRIVATE FXE_V8_ICUDTL_PATH="${_v8_icudtl_dat}"
            )
        endif()
        set(_scripts
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/typescript_smoke.ts"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/typescript_modules_smoke.ts"
        )
        foreach(_s IN LISTS ARGN)
            list(APPEND _scripts "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
        endforeach()
        add_test(NAME ${target} COMMAND ${target} ${_scripts})
    endfunction()

    # ---------------------------------------------------------------------
    # C++ tests.
    # ---------------------------------------------------------------------
    fxe_add_cpp_test(fxe_core_tests tests/core_tests.cpp fxe_core)
    fxe_add_cpp_test(fxe_font_tests tests/font_module_test.cpp fxe_font)
    fxe_add_cpp_test(fxe_font_render_tests tests/font_render_test.cpp fxe_font)
    fxe_add_cpp_test(fxe_uv_loop_tests tests/uv_loop_test.cpp fxe_runtime)
    fxe_add_cpp_test(fxe_update_manifest_v2_tests tests/update_manifest_v2_test.cpp fxe_runtime fxe_os)
    fxe_add_cpp_test(fxe_cbor_canonical_tests tests/cbor_canonical_test.cpp fxe_runtime)
    fxe_add_cpp_test(fxe_markdown_parse_tests tests/markdown_parse_test.cpp fxe_markdown)
    if(TARGET fxe_webauthn)
        fxe_add_cpp_test(
            fxe_webauthn_auth_data_tests
            tests/webauthn_auth_data_test.cpp
            fxe_webauthn
            fxe_runtime
        )
        fxe_add_cpp_test(
            fxe_webauthn_attestation_tests
            tests/webauthn_attestation_test.cpp
            fxe_webauthn
            fxe_runtime
        )
        fxe_add_cpp_test(
            fxe_webauthn_virtual_tests
            tests/webauthn_virtual_test.cpp
            fxe_webauthn
            fxe_runtime
        )
        fxe_add_cpp_test(fxe_webauthn_jar_tests tests/webauthn_jar_test.cpp fxe_webauthn fxe_runtime)
    endif()
    if(UNIX)
        fxe_add_cpp_test(fxe_net_http_advanced_tests tests/net_http_advanced_test.cpp fxe_net fxe_runtime)
    endif()
    if(TARGET fxe_wgpu)
        fxe_add_cpp_test(fxe_wgpu_pipeline_cache_tests tests/wgpu_pipeline_cache_test.cpp fxe_wgpu)
        fxe_add_cpp_test(fxe_wgpu_blur_smoke tests/wgpu_blur_smoke.cpp fxe_wgpu)
    endif()
    if(TARGET fxe_js)
        fxe_add_cpp_test(
            fxe_uv_microtask_flush_tests
            tests/uv_microtask_flush_test.cpp
            fxe_js
        )
        if(_v8_icudtl_dat)
            target_compile_definitions(
                fxe_uv_microtask_flush_tests
                PRIVATE FXE_V8_ICUDTL_PATH="${_v8_icudtl_dat}"
            )
        endif()
    endif()
    if(UNIX AND NOT APPLE)
        fxe_add_cpp_test(
            fxe_os_linux_smoke_tests
            tests/os_linux_smoke.cpp
            fxe_os
        )
    endif()
    if(FXE_ENABLE_NATIVE_TLS_HTTP2)
        fxe_add_cpp_test(
            fxe_native_tls_tests
            tests/native_tls_test.cpp
            fxe_net
            MbedTLS::mbedtls
            MbedTLS::mbedx509
            MbedTLS::mbedcrypto
        )
        fxe_add_cpp_test(
            fxe_ws_deflate_tests
            tests/ws_deflate_test.cpp
            fxe_net
            ZLIB::ZLIB
        )
        target_sources(fxe_ws_deflate_tests PRIVATE src/runtime/uv_loop.cpp)
    endif()

    # Debug-protocol C++ tests prefer fxe_js (Runtime.* handlers) when V8 is
    # enabled and fall back to fxe_debug otherwise.
    foreach(
        _entry
        IN
        ITEMS
            "fxe_debug_tests=tests/debug_protocol_tests.cpp"
            "fxe_debug_cdp_ws_tests=tests/debug_cdp_ws_test.cpp"
    )
        string(REPLACE "=" ";" _pair "${_entry}")
        list(GET _pair 0 _name)
        list(GET _pair 1 _src)
        add_executable(${_name} ${_src})
        if(TARGET fxe_js)
            target_link_libraries(${_name} PRIVATE fxe_js)
        else()
            target_link_libraries(${_name} PRIVATE fxe_debug)
        endif()
        if(TARGET fxe_wgpu)
            target_link_libraries(${_name} PRIVATE fxe_wgpu)
        endif()
        add_test(NAME ${_name} COMMAND ${_name})
    endforeach()

    if(TARGET fxe_run AND TARGET fxe_wgpu)
        add_test(
            NAME fxe_run_cpu_prof_screenshot_exit
            COMMAND
                fxe_run
                --cpu-prof=${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_screenshot.cpuprofile
                --cpu-prof-md=${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_screenshot.md
                --cpu-prof-hz=500
                --screenshot=${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_screenshot.png
                --screenshot-delay=50
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/js/hello.ts
        )
        if(UNIX)
            add_test(
                NAME fxe_run_cpu_prof_sigterm_flush
                COMMAND
                    /bin/sh -c
                    "set -eu; run=\"$1\"; prof=\"$2\"; md=\"$3\"; script=\"$4\"; rm -f \"$prof\" \"$md\"; \"$run\" --cpu-prof=\"$prof\" --cpu-prof-md=\"$md\" --cpu-prof-hz=500 \"$script\" & pid=$!; sleep 0.5; kill -TERM \"$pid\"; code=0; wait \"$pid\" || code=$?; test \"$code\" -eq 143; test -s \"$prof\"; test -s \"$md\""
                    sh $<TARGET_FILE:fxe_run>
                    ${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_sigterm.cpuprofile
                    ${CMAKE_CURRENT_BINARY_DIR}/fxe_run_cpu_prof_sigterm.md
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/js/login_form.tsx
            )
        endif()
    endif()

    # ---------------------------------------------------------------------
    # V8 TypeScript tests.
    # Convention: every tests/*_test.ts becomes a target fxe_<stem> running
    # that single script (the smokes are prepended by the helper). To bundle
    # multiple scripts under one target, drop the per-script auto-name and
    # add an explicit fxe_add_v8_ts_test() call.
    # ---------------------------------------------------------------------
    if(TARGET fxe_js)
        fxe_add_v8_ts_test(fxe_ts_tests)
        fxe_add_v8_ts_test(fxe_ui_box_shadow_blur_test tests/ui_box_shadow_blur_test.tsx)
        file(
            GLOB _v8_ts_test_scripts
            RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
            CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.ts"
        )
        if(NOT FXE_ENABLE_NATIVE_TLS_HTTP2)
            # node_compat_http2_test.ts requires the native HTTP/2 stack.
            list(
                FILTER _v8_ts_test_scripts
                EXCLUDE
                REGEX "node_compat_http2_test\\.ts$"
            )
        endif()
        foreach(_script IN LISTS _v8_ts_test_scripts)
            get_filename_component(_stem "${_script}" NAME_WE)
            fxe_add_v8_ts_test("fxe_${_stem}" "${_script}")
        endforeach()
    endif()
    foreach(example hello_triangle hello_sprite primitives_showcase)
        if(TARGET ${example})
            add_test(NAME fxe_example_${example} COMMAND ${example})
            set_tests_properties(
                fxe_example_${example}
                PROPERTIES LABELS "examples;smoke"
            )
        endif()
    endforeach()
endif()
