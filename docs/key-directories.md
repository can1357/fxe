# Key Directories

| Path | Purpose |
|---|---|
| `include/fxe/` | Public C++ headers (`primitives.hpp`, `renderer.hpp`, `window.hpp`, `command_buffer.hpp`, `spritesheet.hpp`, `color.hpp`, `math.hpp`, `font.hpp` + `font/*.hpp`, `v8_host.hpp`, `typescript.hpp`, `debug.hpp`, `crash.hpp`, `power.hpp`) |
| `src/core/` | `primitives.cpp`, `command_buffer.cpp`, `spritesheet.cpp`, `system_fonts.cpp`, `print_pdf.cpp`, `render_stats.cpp`, `stb_impl.cpp` |
| `src/window/` | `glfw_window.cpp` (90 KB; window/input/IME/drag-drop/clipboard) |
| `src/wgpu/` | `renderer_dawn.cpp`, `renderer_wgpu.cpp` (null), `pipeline.cpp` (cache), `offscreen.cpp`, `shaders/main.wgsl` |
| `src/font/` | FreeType/CoreText face impls, HarfBuzz/CoreText shapers, Fontconfig/CoreText/Win32 discovery, glyph cache, atlas |
| `src/net/` | `http_client.cpp`, `http2_client/server.cpp`, `websocket_client.cpp`, `tls_client/server.cpp`, `cookie_jar.cpp` |
| `src/audio/` | miniaudio wrapper, capture queue |
| `src/os/` | `macos/`, `win32/`, `linux/` platform shims; `crash_*` + `power_*` + `single_instance.cpp` |
| `src/runtime/` | `uv_loop.cpp`, `fs_fd.cpp`, `fs_watcher_*.cpp`, `node_compat.cpp` (+ `node_compat_js/` JS adapters), `fxe_native.cpp` (185 KB), `updater.cpp`, `bundle_loader.cpp`, `native_tls/http2/https.cpp`, `capabilities.cpp` |
| `src/debug/` | `server.cpp`, `dispatch.cpp`, `cdp_ws.cpp`, `json.cpp`, `base64.cpp`, `screenshot.cpp`, `host_stub.cpp` |
| `src/js/` | `v8_host.cpp` (80 KB), `typescript.cpp`, `source_map.cpp`, `dispatch_runtime.cpp`, ~32 `bind_*.cpp`, `fxe_run.cpp` |
| `tests/` | C++ tests (`*_tests.cpp`, `*_test.cpp`) + TS tests (`bind_*_test.ts`, `node_compat_*_test.ts`, `ui_*_test.ts`, `hmr_*_test.ts`, …) + `golden/` |
| `examples/` | Native C++ demos (`hello_triangle.cpp`, `hello_sprite.cpp`, `primitives_showcase.cpp`) |
| `examples/js/` | TS / TSX demos (`hello.ts`, `ui_kit_demo.tsx`, `login_form.tsx`, `bench.ts`, `custom_pipeline.ts`, `window_chat.ts`, `two_windows.ts`, `sprite_demo.ts`, `transparent_demo.ts`, `custom_titlebar.ts`, `audio_demo.ts`, `git_log.ts`, `loop.ts`, `showcase.ts`, `ui_demo.ts`, `ui_reconciler_demo.ts`, `jsx_demo.tsx`) |
| `packages/fxe-ui/` | JSX UI framework: `reconciler/`, `layout/`, `paint/`, `mount/`, `style/`, `theme/`, `animated/`, `components/`, `jsx-runtime.ts` |
| `types/` | `fxe.d.ts` (~96 KB; runtime), `fxe-ui.d.ts` (~21 KB; UI framework), `fxe_typecheck_globals.d.ts` |
| `clients/python/` | `fxe-debug-client` SDK (stdlib only): `launcher.py`, `client.py`, `page.py`, `protocol.py`, `transport.py`, `trace.py`, `cli.py`, `examples/`, `tests/` |
| `tools/fxe-pack/` | Packaging tool: `main.cpp`, `bundle.cpp/.hpp`, `templates/` (AppxManifest, wix_product, AppRun, Info.plist) |
| `cmake/` | `deps.cmake`, `shaders.cmake` (`embed_wgsl`), `embed_text_header.cmake`, `embed_unenv.cmake`, `install.cmake`, `FindV8.cmake` |
| `scripts/` | `doctor.sh`, `build_v8.sh`, `build_v8.ps1` |
| `vendor/` | `unenv/` (Node compat shims, generated when `FXE_ENABLE_NODE_COMPAT=ON`) |
| `third_party/` | `miniaudio/` (single-header audio) |
