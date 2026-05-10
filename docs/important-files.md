# Important Files

- `CMakeLists.txt`, `CMakePresets.json` — build definitions and preset matrix
- `vcpkg.json` — pinned manifest (libuv, libsodium, mbedtls; freetype / harfbuzz / fontconfig pulled per `FXE_FONT_BACKEND`)
- `cmake/deps.cmake` — FetchContent (glm, glfw, stb, nlohmann_json) + `find_package` for SQLite3, V8, Dawn, libcurl, ZLIB, nghttp2, X11/Xss, D-Bus, Breakpad
- `justfile` — canonical task runner; check first when adding workflows
- `types/fxe.d.ts`, `types/fxe-ui.d.ts` — TypeScript public APIs; keep in lockstep with `src/js/bind_*.cpp` and `packages/fxe-ui/src/`
- `src/js/fxe_run.cpp` — CLI entry, debug-flag parsing, module detection
- `src/js/v8_host.cpp` — isolate / context / module loader / HMR / source maps / debug evaluation
- `src/debug/dispatch.cpp` — debug protocol method table (extend new domains here)
- `src/runtime/uv_loop.cpp`, `src/runtime/node_compat.cpp` — runtime loop + Node compat surface
- `include/fxe/renderer.hpp`, `include/fxe/primitives.hpp`, `include/fxe/window.hpp`, `include/fxe/font.hpp` — C++ public surface
- `clients/python/fxe_debug/` — debug-protocol client (`launcher.py`, `client.py`, `page.py`, `protocol.py`, `transport.py`, `trace.py`, `cli.py`)
- `tools/fxe-pack/` — packaging tool (DMG / MSI / MSIX / AppImage / plain)
- `.github/workflows/ci.yml` — Linux/macOS/Windows matrix; uses `FXE_FETCH_DEPS=ON`, plus `-DFXE_ENABLE_WGPU=OFF -DFXE_ENABLE_V8=OFF` for core-only smoke
- `TODO.md` — running roadmap and per-module gap audit
