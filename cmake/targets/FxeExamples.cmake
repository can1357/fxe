if(FXE_BUILD_EXAMPLES)
    foreach(example hello_triangle hello_sprite primitives_showcase)
        add_executable(${example} examples/${example}.cpp)
        if(TARGET fxe_wgpu)
            target_link_libraries(${example} PRIVATE fxe_wgpu)
        else()
            target_link_libraries(${example} PRIVATE fxe_window)
        endif()
    endforeach()
    install(
        FILES
            examples/js/hello.ts
            examples/js/showcase.ts
            examples/js/loop.ts
            examples/js/bench.ts
            examples/js/ui_demo.ts
            examples/js/jsx_demo.tsx
            examples/js/ui_reconciler_demo.ts
            examples/js/ui_kit_demo.tsx
            examples/js/login_form.tsx
            examples/js/two_windows.ts
            examples/js/window_chat.ts
            examples/js/git_log.ts
        DESTINATION share/fxe/examples/js
    )
endif()
