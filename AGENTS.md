# Repository Guidelines

## Project Overview


> **Investigating runtime behavior?** Reach for the Python SDK, not source-reading.
> See [Debugging FXE Apps with the Python SDK](#debugging-fxe-apps-with-the-python-sdk).
FXE is an immediate-mode application platform — an alternative to Electron with real GPU graphics. The core is a hookless, platform-agnostic 2D/3D renderer with optional Dawn/WebGPU backend and an embedded V8 + TypeScript runtime, so applications are authored in TS/JS and rendered through a native command buffer instead of a browser. A Python debug client (`fxe-cli`) drives running instances over an NDJSON debug protocol (Puppeteer-style: evaluate, click, type, screenshot).

## Architecture & Data Flow

Layered. Each upper layer is optional, and the lower ones link cleanly without
the rest:

1. **Core** (`fxe_core`) — Pure C++ primitives, command buffer, spritesheet,
   render-stats, font-stack glue. No GPU, no JS. Deps: `glm`, `glfw`, `stb`,
   `fxe_font`, `fxe_shaders`.
2. **Window** (`fxe_window`, `src/window/glfw_window.cpp`) — GLFW window,
   input, IME, drag-drop, clipboard, custom title-bar plumbing, native handle
   extraction.
3. **WGPU** (`fxe_wgpu`, optional, `FXE_ENABLE_WGPU`) — Dawn/WebGPU renderer,
   pipeline cache, offscreen targets, frame capture, blur post-process. WGSL
   under `src/wgpu/shaders/`. A null fallback (`renderer_wgpu.cpp`) keeps
   command-buffer accounting alive for headless tests.
4. **Font** (`fxe_font`) — FreeType / HarfBuzz / CoreText / Fontconfig matrix
   selected by `FXE_FONT_BACKEND`. R8 mask + BGRA color emoji atlas pages,
   shelf packer, OpenType feature/variation support, per-platform discovery.
5. **Net** (`fxe_net`) — libcurl HTTP client (cookies, multipart, proxy),
   nghttp2 HTTP/2 client+server, RFC 6455 WebSocket client (incl. `wss://`
   via mbedTLS), mbedTLS client+server with session resumption, persistent
   cookie jar.
6. **Audio** (`fxe_audio`) — miniaudio engine, in-memory decode, mic capture
   via `ma_device` with main-thread queue drain.
7. **OS** (`fxe_os`) — per-platform shims for App lifecycle, dialogs,
   notifications, menus, tray, power, recent-documents, single-instance,
   crash dumps. macOS uses AppKit/UserNotifications, Windows uses Win32 +
   Toast XML, Linux uses D-Bus + libnotify (gated by `FXE_OS_DBUS`).
8. **Runtime** (`fxe_runtime`) — libuv event loop, fs FDs, fs watchers
   (inotify / FSEvents / `ReadDirectoryChangesW`), `node_compat` module
   loader with unenv adapters, `fxe_native` (Node-shaped bindings), updater
   (signed feeds, channels, rollback, signing-authority checks),
   `bundle_loader` for packaged apps.
9. **Debug** (`fxe_debug`) — NDJSON-over-TCP and CDP-over-WebSocket servers,
   hand-written JSON parser, base64, screenshot encoder, dispatch table for
   `System.*`, `Console.*`, `Runtime.*`, `Page.*`, `Input.*`, `Window.*`,
   `Debugger.*`, `Schema.*`, `Reconciler.*`, `Profiler.*`, `HeapProfiler.*`,
   `Fetch.*`, `Fs.*`.
10. **JS host** (`fxe_js`, optional, `FXE_ENABLE_V8`) — V8 isolate, embedded
    `tsc` transpile, ES module loader (incl. `fxe-ui`, `fxe:sqlite`,
    `fxe:ipc` synthetic modules), source maps for stack traces, ~32
    `bind_*.cpp` files exposing renderer, window, app, fs, fetch, websocket,
    storage, audio, font, sqlite, ipc, timers, performance, menu, tray,
    dialog, shell, notifications, power, process, image, spritesheet,
    pipeline, offscreen, print, path, url, render-stats, global-shortcut,
    crash. HMR via `__fxe_hmr`.
11. **Runner** (`fxe_run`, `src/js/fxe_run.cpp`) — CLI: parses
    `--debug` / `--debug-pause`, initializes V8, creates host + window +
    renderer, runs `.ts` / `.mts` / `.cts` / `.js` script.

**Data flow (TS app frame):** TS source → V8 + embedded `tsc` (transpile only,
no type-check) → JS calls `Primitives.fillRect(cb, …)` → C++ binding writes
opcodes into `CommandBuffer` → `Renderer.endFrame()` uploads vertex/index
buffers and submits Dawn queue → GPU → optional `Page.screenshot` reads back
framebuffer over the debug protocol.

**Threading.** V8 and the GPU are pinned to the main thread. The debug server
uses an accept thread plus a session thread per connection; commands are
posted onto a render-thread task pump that drains between frames. libuv runs
on the main thread (microtask checkpoint after every `uv_run(UV_RUN_NOWAIT)`).
Network workers (HTTP, WebSocket, native TLS) own their own threads and post
results back through the loop. fs watchers own a dedicated thread per
platform. The audio engine handles its own threads internally.

## Key Directories

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

## Development Commands

Driven by `just` (CMake under the hood). Default preset is `dev` (no V8/Dawn).
Presets: `dev`, `release`, `dev-wgpu`, `dev-v8`, `dev-v8-wgpu` (the last
additionally turns on `FXE_ENABLE_NODE_COMPAT`).

```bash
just bootstrap                 # one-time: build in-tree vcpkg
just configure [preset]        # cmake --preset
just build [preset] [-- args]  # alias: just b
just test [preset]             # ctest --preset; alias: just t
just test-core [preset]        # run fxe_core_tests directly
just example hello_sprite      # build+run native example; alias: just r
just ts ui_demo [-- args]      # build dev-v8-wgpu, run examples/js/ui_demo.ts
just debug ui_demo 9333        # run paused with debug protocol on port 9333
just ts-check                  # tsc --noEmit -p tsconfig.json
just format / format-check     # clang-format over tracked C++ sources; alias: just f
just format-js / format-js-check / lint-js / fix-js      # biome over JS/TS
just format-py / format-cmake / format-shell / spell     # optional, soft-skip
just ci                        # format-check + test + ts-check
just ci-quick                  # format/lint/typecheck only (no build)
just pycli ...                 # python -m fxe_debug.cli (Python debug CLI)
just pytest                    # unittest discover under clients/python/tests
just doctor                    # report which dev tools are installed
```

Direct CMake equivalents work too:

```bash
cmake --preset dev-v8-wgpu && cmake --build --preset dev-v8-wgpu
ctest --preset dev-v8-wgpu --output-on-failure
./build/dev-v8-wgpu/fxe_run examples/js/hello.ts
```

## Code Conventions & Common Patterns

- **C++ formatting:** LLVM base, 2-space indent, 100 col, left pointer
  alignment, namespaces indented (`All`). Run `just format` before committing.
- **Naming:** C++ uses `snake_case` for functions/types matching gfw/gfwx
  heritage (`fillRect`, `command_buffer`, `texture_info`); JS bindings expose
  `camelCase` (`fillRect`, `beginFrame`). Opcode constants are
  `OP_FILL_RECT`-style.
- **Headers:** `include/fxe/*.hpp` is the only stable surface; everything in
  `src/` is internal.
- **Dependencies:** `fxe_core` links only `glm`/`glfw`/`stb`/`fxe_font`. Do
  **not** add a heavy dependency to `fxe_core`. Dawn and V8 are guarded by
  `FXE_ENABLE_WGPU` / `FXE_ENABLE_V8` and live in their own libraries
  (`fxe_wgpu`, `fxe_js`). libuv, mbedTLS, nghttp2, libcurl all go through
  `fxe_net` / `fxe_runtime`.
- **Bindings:** One file per class/namespace in `src/js/bind_*.cpp`. GC
  finalizers free heap-owned C++ objects. Every new JS API **must** be
  mirrored in `types/fxe.d.ts` (and `types/fxe-ui.d.ts` for UI additions).
- **Hot loops:** For high-volume scenes, batch via `Primitives.drain()`
  (opcode/parameter arrays) rather than per-call V8 trampolines. V8 may
  inline through static namespaces — prototype methods are virtual and
  intercept reliably.
- **Memory:** Command buffers reuse a vertex/index allocator across frames
  (`epoch()`, `allocate()`, `clear()`).
- **JSON & protocol:** Hand-written, no external JSON dep on the debug path.
  Extend `src/debug/json.cpp` and register handlers in `src/debug/dispatch.cpp`
  keyed by `Domain.method`. Expose new domains in `Schema.getDomains` and add
  a corresponding helper to the Python SDK.
- **Shaders:** WGSL only, in `src/wgpu/shaders/*.wgsl`, embedded as headers
  via `embed_wgsl()` in `cmake/shaders.cmake`. Optionally validated with
  `tint` when `FXE_WGSL_VALIDATOR=/path/to/tint` is set.
- **Pipelines:** All Dawn pipelines go through `pipeline_cache` keyed on
  `{vs_entry, fs_entry, color_format, depth_format, blend_mode, topology,
  sample_count}` — never create raw `RenderPipeline` objects in renderer
  paths.
- **Errors:** Bindings throw Node-shaped errors `{code, errno, syscall, path}`
  where applicable (`EAUDIO_DECODE`, `ERR_FXE_UPDATE_*`, etc.). Async fs
  surfaces `AbortError` for AbortSignal cancellation.

## fxe-ui — UI toolkit

`fxe-ui` is the single JSX/TSX UI package shipped under `packages/fxe-ui/`.
It owns the reconciler (`Layer`, `Draw`, hooks, scheduler, signals,
external-store), a TypeScript flexbox layout solver, CSS-like `Style` objects,
theme context, components, paint pipeline, mount pipeline, and `Animated`
timing/spring helpers.

- Import from `fxe-ui` and use `/** @jsxImportSource fxe-ui */` for TSX.
- Default layout is Yoga / React Native style: `flexDirection: 'column'`,
  not CSS `row`.
- `StyleSheet.create()` freezes stable style objects. Prefer it for styles
  captured in hook dependency arrays.
- Core components: `View`, `Text`, `Image`, `Pressable`, `Button`,
  `ScrollView`, `TextInput`, `VirtualList`.
- Hooks: `useState`, `useReducer`, `useRef`, `useEffect`, `useMemo`,
  `useContext`, `useId`, `useFrame`, `useEvent`, `useDeferredValue`,
  `useTransition`.
- `mount(root, window)` wires layout, paint, hit-testing, hover/press/focus,
  cursor, and keyboard dispatch. It returns a disposer for listener cleanup.
- Layout primitives push entries to a built-in `recordLayout` sink when
  layout tracing is enabled — see the SDK's `page.layout_trace_*` helpers.

```tsx
/** @jsxImportSource fxe-ui */
import { Window } from 'fxe';
import { Button, StyleSheet, Text, View, mount, useState } from 'fxe-ui';

const s = StyleSheet.create({
  root: { width: '100%', height: '100%', padding: 24, gap: 12, backgroundColor: 0x0f172aff },
  title: { height: 28, color: 0xffffffff, fontSize: 22 },
});

function App() {
  const [count, setCount] = useState(0);
  return (
    <View style={s.root}>
      <Text style={s.title}>Count {count}</Text>
      <Button title="Increment" onPress={() => setCount((n) => n + 1)} />
    </View>
  );
}

mount(<App />, new Window({ width: 480, height: 320 }));
```

## Important Files

- `CMakeLists.txt`, `CMakePresets.json` — build definitions and preset matrix
- `vcpkg.json` — pinned manifest (libuv, libsodium, mbedtls; freetype /
  harfbuzz / fontconfig pulled per `FXE_FONT_BACKEND`)
- `cmake/deps.cmake` — FetchContent (glm, glfw, stb, nlohmann_json) +
  `find_package` for SQLite3, V8, Dawn, libcurl, ZLIB, nghttp2, X11/Xss,
  D-Bus, Breakpad
- `justfile` — canonical task runner; check first when adding workflows
- `types/fxe.d.ts`, `types/fxe-ui.d.ts` — TypeScript public APIs; keep in
  lockstep with `src/js/bind_*.cpp` and `packages/fxe-ui/src/`
- `src/js/fxe_run.cpp` — CLI entry, debug-flag parsing, module detection
- `src/js/v8_host.cpp` — isolate / context / module loader / HMR / source
  maps / debug evaluation
- `src/debug/dispatch.cpp` — debug protocol method table (extend new
  domains here)
- `src/runtime/uv_loop.cpp`, `src/runtime/node_compat.cpp` — runtime loop
  + Node compat surface
- `include/fxe/renderer.hpp`, `include/fxe/primitives.hpp`,
  `include/fxe/window.hpp`, `include/fxe/font.hpp` — C++ public surface
- `clients/python/fxe_debug/` — debug-protocol client (`launcher.py`,
  `client.py`, `page.py`, `protocol.py`, `transport.py`, `trace.py`,
  `cli.py`)
- `tools/fxe-pack/` — packaging tool (DMG / MSI / MSIX / AppImage / plain)
- `.github/workflows/ci.yml` — Linux/macOS/Windows matrix; uses
  `FXE_FETCH_DEPS=ON`, `FXE_ENABLE_WGPU=OFF` for core-only smoke
- `TODO.md` — running roadmap and per-module gap audit

## Runtime/Tooling Preferences

- **Build:** CMake ≥ 3.24, Ninja generator, vcpkg manifest mode.
  `just bootstrap` builds the in-tree vcpkg.
- **C++:** C++20. Compilers: clang/gcc/MSVC matched in CI.
- **TypeScript:** `tsc` (npm devDep `typescript ^5.9.3`) for type-checking
  only — `tsconfig.json` targets ES2022, Node16 modules. Runtime
  transpilation is performed inside V8 and does **not** type-check; always
  run `just ts-check` before relying on TS examples.
- **Node/npm:** used solely as a `tsc` carrier (plus `@biomejs/biome` and
  `@types/node` devDeps). No application runtime depends on Node — the JS
  runtime is V8 embedded directly. Do not introduce npm runtime deps.
- **V8:** prefer system/distro V8; otherwise vendor with
  `scripts/build_v8.sh` (or `.ps1`) which uses depot_tools and installs to
  `.vendor/v8-install/`. Configure with `-DFXE_ENABLE_V8=ON` and export
  `V8_ROOT` or `V8_DIR`. `--expose_gc` is enabled at host init for the
  HeapProfiler.
- **Dawn:** supply externally; configure with `-DDawn_DIR=…` or
  `CMAKE_PREFIX_PATH`. Not auto-fetched.
- **libuv / mbedTLS / nghttp2:** via vcpkg manifest. `FXE_ENABLE_LIBUV` is
  `ON` by default (required for async fs/net);
  `FXE_ENABLE_NATIVE_TLS_HTTP2` is gated `OFF` and turns on the native
  HTTPS / HTTP/2 transport in `src/runtime/native_*.cpp`.
- **Python:** ≥ 3.10, stdlib only. Do not add runtime dependencies to
  `clients/python/`.
- **Biome:** JS/TS format + lint over `examples/js`, `tests`, `packages`,
  `types`. Required in CI (`format-js-check`, `lint-js`).
- **IDE:** `.clangd` points to `build/dev/compile_commands.json`; configure
  that preset first for IntelliSense.

## Testing & QA

- **Framework:** CTest with a hand-written assert harness (no gtest /
  catch2) to keep test binaries small.
- **C++ targets:**
  - `fxe_core_tests` — core API, no GPU/V8 (always built)
  - `fxe_font_tests`, `fxe_font_render_tests` — font library / face / shaper
    / collection / discovery; render → atlas roundtrip; ligatures, kerning,
    color emoji
  - `fxe_uv_loop_tests`, `fxe_uv_microtask_flush_tests` — libuv loop and
    V8 microtask flush
  - `fxe_net_http_advanced_tests` — HTTP cookie jar, redirects, headers,
    timeouts (libcurl)
  - `fxe_native_tls_tests`, `fxe_ws_deflate_tests` — mbedTLS client/server,
    WebSocket per-message-deflate
  - `fxe_wgpu_pipeline_cache_tests`, `fxe_wgpu_blur_smoke` — pipeline
    cache identity, blur post-process
  - `fxe_os_linux_smoke_tests` — D-Bus / power / crash on non-Apple Unix
  - `fxe_debug_tests`, `fxe_debug_cdp_ws_tests` — JSON, base64, dispatch,
    CDP WebSocket adapter
  - Examples are registered as smoke tests with labels `examples;smoke`.
- **TypeScript targets:** `tests/*_test.ts` and `tests/*_test.tsx` are
  auto-registered via `fxe_add_v8_ts_test()` and run inside `fxe_run`.
  `typescript_smoke.ts` + `typescript_modules_smoke.ts` always run first.
  Coverage spans `bind_*` (every JS binding), `node_compat_*` (every
  `node:*` adapter), `ui_*` (fxe-ui components / layout / events / focus
  / scroll / text-wrap / animated / reconciler), `hmr_*`,
  `worker_threads_*`, messaging, native runtime, perf, I/O, auto-update,
  clipboard, stdin, window chrome, packager contract.
- **Run:** `just test` (full preset), `just test-core` (core exe directly),
  `just pytest` (Python SDK).
- **Golden tests:** deterministic FNV-1a hashes over command buffers in
  `core_tests.cpp` (showcase + text/sprite). Pixel goldens under
  `tests/golden/` are reserved for Dawn-backed frame capture; not yet wired.
- **Type checks:** `just ts-check` (or `npm run typecheck`) — required
  before merging TS-touching changes.
- **Format gate:** `just format-check` + `just format-js-check` — CI fails
  on diffs.
- **Local CI parity:** `just ci` (= format-check + format-js-check +
  ts-check + lint-js + test).
- **Headless Linux:** CI uses `xvfb`; replicate with `xvfb-run -a just test`
  if no display is available.
- **Python SDK:** `just pytest` runs `unittest discover` under
  `clients/python/tests`.

## Debugging FXE Apps with the Python SDK

When you (the agent) need to verify behavior of a JS/TS example or a user
project — e.g. "does my new primitive draw correctly?", "did this script
actually call `endFrame`?", "what does `console.log` print at frame 30?" —
drive `fxe_run` through the Python SDK rather than reading source and
guessing. The SDK is Puppeteer-style: launch the app, attach, evaluate,
screenshot, inject input.

**Prerequisite:** `just build dev-v8-wgpu` (debug protocol + GPU backend).

### Quick recipes

Always launch with `pause=True` so you can install setup hooks before the
render loop starts; call `page.resume()` once setup is done. Wrap the page
in a `try/finally` so `page.close()` always runs — the SDK doesn't yet
support `async with`.

```python
# clients/python/examples/screenshot.py — golden-image diff a script
import asyncio, sys
sys.path.insert(0, "clients/python")  # only when running outside the package
from fxe_debug import launch

async def main():
    page = await launch("examples/js/hello.ts", pause=True)
    try:
        await page.resume()
        await asyncio.sleep(0.3)        # let the script paint at least once
        await page.screenshot("hello.png")
        w, h = await page.framebuffer_size()
        print(f"captured {w}x{h}")
    finally:
        await page.close()

asyncio.run(main())
```

### What you can do without modifying the script

- `await page.evaluate("expr")` — run any expression in the live V8 isolate.
  The result is JSON-serialised. Use this to read script-side state without
  adding `console.log` lines.
- `await page.globals()` — list every own property on the global; cheap way
  to discover what the script exposes.
- `await page.screenshot(path)` — RGBA8 PNG of the most recent frame, decoded
  from base64. **First call after launch arms capture and returns an error
  ("retry after the next render"); the SDK does not auto-retry — sleep for a
  frame and call again.** A small helper:
  ```python
  async def shot(page, path, retries=3):
      from fxe_debug import ProtocolError
      for _ in range(retries):
          try:
              return await page.screenshot(path)
          except ProtocolError as e:
              if e.code == -32001: await asyncio.sleep(0.05); continue
              raise
      raise RuntimeError("no frame captured after retries")
  ```
- `page.console_messages` — async iterator of `Console.messageAdded` events
  (`level`, `text`, `ts`). Lazily calls `Console.enable` on first iteration.
  Use to assert `console.log("loaded fonts")` actually fires.
- `await page.mouse.click(x, y)` / `mouse.move` / `mouse.wheel` — synthesised
  GLFW input. Hits the script's `window.on("mouse_button"|"cursor_pos", ...)`
  handlers exactly as a real OS event would.
- `await page.keyboard.press("Enter")` / `keyboard.type("hello")` — same
  semantics for keys and char input. Named keys: `Enter`, `Escape`, `Tab`,
  `Backspace`, `ArrowLeft/Right/Up/Down`, `Space`.
- `await page.pause()` / `resume()` / `step()` — gate the render loop. `step`
  resumes for exactly one frame then re-pauses, useful for deterministic
  capture of frame N.
- `await page.close()` — sends `Window.close` and tears the child down.

### Investigation patterns

**"Does the script set `window.foo` correctly after init?"**
```python
page = await launch("user.ts")
try:
    await asyncio.sleep(0.1)
    print(await page.evaluate("JSON.stringify(window.foo)"))
finally:
    await page.close()
```

**"Reproduce a click bug"**
```python
page = await launch("user.ts")
try:
    msgs = []
    async def collect():
        async for m in page.console_messages: msgs.append(m)
    asyncio.create_task(collect())
    await page.mouse.click(120, 80)
    await asyncio.sleep(0.3)
    for m in msgs: print(m.level, m.text)
finally:
    await page.close()
```

**"Capture frame N for golden comparison"**
```python
page = await launch("user.ts", pause=True)
try:
    for i in range(N): await page.step()
    await page.screenshot(f"frame_{N}.png")
finally:
    await page.close()
```

**Driving an unknown user project (no source read first)**
```python
page = await launch("/path/to/user/script.ts")
try:
    print("globals:", await page.globals())
    print("size:", await page.framebuffer_size())
    print("eval:", await page.evaluate("Object.keys(globalThis)"))
finally:
    await page.close()
```

### CLI alternative (one-shot, no Python)

```bash
just debug ui_demo 9333          # spawn paused on port 9333 (separate shell)
just pycli inspect --port 9333   # handshake + globals + framebuffer
just pycli screenshot --port 9333 --out shot.png
just pycli eval --port 9333 'window.foo'
just pycli mouse click --port 9333 100 100
just pycli console --port 9333   # tail console.* messages until Ctrl-C
just pycli resume --port 9333
just pycli close --port 9333
```

### Live function tracing (no source edits)

When debugging "what arguments did this helper see while the bug
reproduced?", reach for `page.trace_install(target, capture)` instead of
adding `console.log` and rebuilding. The wrapper, ring buffer, and
drain helpers live entirely in the running V8 isolate (under
`globalThis.__fxeTrace`); the SDK just ferries small JS snippets through
`Runtime.evaluate`.

- `target` is a dotted path resolved against `globalThis`. Use
  `Foo.prototype.bar` for instance methods.
- `capture` is a JS *expression* with `args`, `self`, `result`, `error`,
  `phase` in scope. Whatever it returns is JSON-serialised and pushed
  onto the (bounded) ring buffer. Default capture is `args`.
- `phases` selects the call phases that record a sample (`"call"`,
  `"return"`, `"throw"`). Default is `("call",)`.

```python
tid = await page.trace_install(
    "Primitives.fillRect",
    "{x: args[1], y: args[2], w: args[3], h: args[4]}",
    limit=50,
)
await asyncio.sleep(0.5)              # let the bug reproduce
for sample in await page.trace_drain(tid): print(sample)
await page.trace_uninstall(tid)
```

Trace handles survive across runtime changes; always `trace_uninstall`
when done so the wrapper doesn't outlive your investigation.

Caveat: V8 may inline calls through static namespaces (e.g.
`Primitives.fillRect`) after a function is hot, in which case replacing
the property won't intercept already-optimised callsites. Prototype
methods (`Renderer.prototype.beginFrame`, `View.prototype.paint`) and
freshly-installed traces against cold paths intercept reliably; if
you're tracing a hot static helper, also `trace_install` its caller (a
prototype method or component render) where dispatch is virtual.

### Layout tracing (fxe-ui)

For layout-specific bugs ("why is this rect at x=-250?"), `page.trace_install`
is the wrong tool — it depends on V8 dispatch staying uninlined, and you'd
have to instrument every layout-aware component. Instead, fxe-ui has a
built-in `recordLayout` sink that every layout primitive (`View`, eventually
`Text`, `Button`, …) pushes to when tracing is enabled. Defining-side cost
is one boolean check per render, so it's safe to leave wired in.

```python
await page.layout_trace_enable(limit=200)
await page.evaluate("App.windows()[0].setSize(W+1, H+1)")  # invalidate caches
await asyncio.sleep(0.2)
await page.layout_trace_disable()
for entry in await page.layout_trace_drain():
    r = entry["rect"]
    print(f"{entry['component']:5s} sw={entry['styleWidth']!r} -> "
          f"{r['x']},{r['y']} {r['width']}x{r['height']}")
```

Each entry carries `{component, rect, hasParentLayout, styleWidth,
styleHeight, tag?}`. Layer caching means re-renders skip clean subtrees;
bumping the window size by 1px (or any deps-invalidating change) is the
easiest way to force a full layout pass into the buffer.

### When NOT to use the SDK

- Modifying the script under test — just edit + re-run `just ts ...`.
- Pure C++ unit tests — use `fxe_core_tests` / `fxe_debug_tests`.
- Type errors — `just ts-check` is faster than launching V8.

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `LaunchError: FXE_DEBUG_PORT not detected` | binary missing or build stale | `just build dev-v8-wgpu` |
| `ProtocolError(-32002, "V8 host not attached")` | called Runtime.* before the host loaded | wait for handshake / `await asyncio.sleep(0.05)` |
| `ProtocolError(-32001, "capture armed; retry after the next render")` | first screenshot call | retry after a short sleep (see helper above) |
| `ProtocolError(-32002, "window not attached")` / `"renderer not attached"` | the script hasn't run `new Window`/`new Renderer` yet | the SDK should `await page.resume()` if launched paused; otherwise wait |
| Hangs on close | child blocked in `win.run` | the SDK's `__aexit__` sends `Window.close`; if your script ignores close, also call `await page.evaluate("window.close()")` |
| Screenshot all transparent | the renderer hasn't rendered yet — first arming returns nothing | second call after the next `endFrame` will succeed |

## Dev Tooling

Beyond the C++ build/test recipes, `justfile` exposes a fuller dev console.
Run `just doctor` to see which tools are present and `just --list` for the
full grouped recipe index.

- **Required:** `cmake`, `ninja`, `clang-format`, `tsc`, `python3`.
- **Optional (soft-skip if missing):**
  - `biome` — JS/TS format + lint (`just format-js`, `just lint-js`, `just fix-js`).
  - `ruff` — Python format + lint (`just format-py`, `just lint-py`).
  - `shfmt` / `shellcheck` — shell formatting + linting (`just format-shell`, `just lint-shell`).
  - `gersemi` — CMake formatting (`just format-cmake`, `just format-cmake-check`).
  - `typos` — spell check (`just spell`, `just spell-fix`).
  - `tint` — WGSL validation (`just lint-shaders`; honours `FXE_WGSL_VALIDATOR`).
  - `watchexec` / `fswatch` — `just watch <example>` rebuild loop.

Aggregate recipes: `just format-all` (writes), `just lint` (read-only),
`just ci-quick` (no build), `just ci` (full local pipeline). The CI workflow
runs `ci-quick` on every PR; the existing matrix build still runs the
native test smoke.
