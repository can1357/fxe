# FXE

FXE is a cross-platform application platform for building desktop apps in
TypeScript or JavaScript on top of a native immediate-mode GPU renderer. Think
"Electron without Chromium": an embedded V8 isolate runs your TS/JS, your UI is
drawn directly through a 2D/3D command buffer (Dawn/WebGPU), and the OS-level
plumbing (windows, menus, tray, dialogs, fs, networking, IPC, audio) is exposed
as small, focused JS APIs.

The repository ships:

- **`fxe_run`** — the runtime executable that loads a `.ts`/`.mts`/`.cts`/`.js`
  entry script and runs it.
- **`fxe-ui`** — a React-like JSX/TSX framework with a fiber reconciler,
  flexbox layout, hooks, and a component library (`View`, `Text`, `Pressable`,
  `ScrollView`, `TextInput`, `Button`, `Image`, `VirtualList`).
- **`fxe-pack`** — a packaging tool that bundles a TS entry plus assets into a
  shippable binary, DMG, MSI, MSIX, or AppImage.
- **`fxe-debug-client`** (Python, stdlib-only) — a Puppeteer-style SDK that
  drives running instances over an NDJSON / CDP-over-WebSocket debug protocol.
- A native C++ core (`fxe_core`, `fxe_window`, `fxe_wgpu`, `fxe_font`,
  `fxe_debug`, `fxe_runtime`, `fxe_net`, `fxe_audio`, `fxe_os`, `fxe_js`) that
  is usable standalone for embedded GPU graphics without V8.

## Status

Pre-1.0. Public C++ headers under `include/fxe/` and the TypeScript surface in
`types/fxe.d.ts` / `types/fxe-ui.d.ts` are the source of truth; everything
under `src/` is internal. See `TODO.md` for the running roadmap and known
gaps.

## Architecture

Layered. Each upper layer is optional.

```
fxe_core      pure C++ primitives, command buffer, sprites
   │
fxe_window    GLFW window + input + native handle extraction
   │
fxe_wgpu      Dawn/WebGPU renderer (optional; null fallback for headless tests)
   │
fxe_font      FreeType / HarfBuzz / CoreText / Fontconfig (atlas, shaping)
   │
fxe_net       libcurl HTTP, mbedTLS, nghttp2, RFC 6455 WebSockets, cookie jar
   │
fxe_runtime   libuv event loop, fs FDs, fs watchers, Node.js compat, updater
   │
fxe_audio     miniaudio playback + capture
   │
fxe_os        per-platform shims (macOS / Windows / Linux)
   │
fxe_debug     NDJSON-over-TCP and CDP-over-WS debug server
   │
fxe_js        embedded V8 isolate, TS transpile, ~32 bind_*.cpp APIs
   │
fxe_run       CLI: parse args, launch host/window/renderer, run user script
```

**Frame data flow** (TS app → pixels):

```
TS source
  → V8 + embedded tsc (transpile only; no type-check)
  → JS calls Primitives.fillRect(cb, …)
  → C++ binding writes opcodes into CommandBuffer
  → Renderer.endFrame() uploads vertex/index buffers
  → Dawn queue submission
  → GPU
  → optional Page.screenshot read-back over the debug protocol
```

**Threading.** V8 and the GPU are pinned to the main thread. The debug server
uses an accept thread plus a session thread per connection; commands are
posted onto a render-thread task pump that drains between frames. libuv runs
on the main thread and handles async fs/net I/O. Network workers (HTTP,
WebSocket, native TLS) own their own threads internally and post results
back through the loop.

## Build

CMake ≥ 3.24 with the Ninja generator and vcpkg manifest mode are required.
The repository pins a vcpkg checkout under `./vcpkg/`.

```sh
just bootstrap                      # one-time: build in-tree vcpkg
just build dev                      # core only (no V8, no Dawn)
just build dev-wgpu                 # + Dawn/WebGPU
just build dev-v8                   # + embedded V8
just build dev-v8-wgpu              # + V8 + Dawn (full runtime)
just test dev                       # run CTest for a preset
just ts hello                       # build dev-v8-wgpu and run examples/js/hello.ts
```

Direct CMake equivalents:

```sh
cmake --preset dev-v8-wgpu
cmake --build --preset dev-v8-wgpu
ctest --preset dev-v8-wgpu --output-on-failure
./build/dev-v8-wgpu/fxe_run examples/js/hello.ts
```

### CMake options

| Option | Default | Effect |
|---|---|---|
| `FXE_BUILD_EXAMPLES` | ON | Build native C++ examples |
| `FXE_BUILD_TESTS` | ON | Build CTest targets |
| `FXE_FETCH_DEPS` | ON | Fetch glm/glfw/stb/nlohmann_json via FetchContent if missing |
| `FXE_ENABLE_WGPU` | OFF | Build Dawn/WebGPU backend (`fxe_wgpu`); requires Dawn |
| `FXE_ENABLE_V8` | OFF | Build embedded V8 host + bindings (`fxe_js`, `fxe_run`) |
| `FXE_ENABLE_NODE_COMPAT` | OFF | Generate vendored unenv assets for Node compat shims |
| `FXE_ENABLE_LIBUV` | ON | Build libuv-backed runtime loop (required for async fs/net) |
| `FXE_ENABLE_NATIVE_TLS_HTTP2` | OFF | Wire mbedTLS + nghttp2 for native HTTPS/HTTP2 |
| `FXE_OS_DBUS` | ON on Linux | D-Bus desktop integrations (notifications, tray) |
| `FXE_FONT_BACKEND` | auto | One of `freetype`, `fontconfig_freetype`, `freetype_windows`, `coretext`, `coretext_freetype`, `coretext_harfbuzz` |
| `FXE_ENABLE_WARNINGS` | ON | Project compiler warnings |
| `FXE_WARNINGS_AS_ERRORS` | OFF | Promote warnings to errors |
| `FXE_WGSL_VALIDATOR` | unset | Path to `tint`; validates embedded WGSL at build time |

Presets in `CMakePresets.json`: `dev`, `release`, `dev-wgpu`, `dev-v8`,
`dev-v8-wgpu`. `dev-v8-wgpu` additionally turns on `FXE_ENABLE_NODE_COMPAT`.

### Dependencies

**Required (vcpkg manifest):** `libuv`, `libsodium`, `mbedtls`. Optional
font deps (`freetype`, `harfbuzz`, `fontconfig`) are pulled when the
selected `FXE_FONT_BACKEND` needs them.

**Fetched when `FXE_FETCH_DEPS=ON`:** `glm` 1.0.1, `glfw` 3.4,
`nlohmann/json` 3.12.0, `stb` (image read/write/resize),
`miniaudio` 0.11.25 (single-header audio). When
`FXE_ENABLE_NODE_COMPAT=ON`, `unenv` v2.0.0-rc.24 and `pathe` 2.0.3
are also fetched and embedded.

**Externally supplied (not auto-fetched):**

- **Dawn** — required for `FXE_ENABLE_WGPU=ON`. Configure with `-DDawn_DIR=…`
  or an equivalent `CMAKE_PREFIX_PATH`.
- **V8** — required for `FXE_ENABLE_V8=ON`. Prefer a system package
  exposing headers plus `libv8`, `libv8_libbase`, `libv8_libplatform`. To
  vendor V8, run `scripts/build_v8.sh` (or `.ps1`) — uses depot_tools and
  installs to `.vendor/v8-install/`. Then export `V8_ROOT` or `V8_DIR`.
- **Optional system packages:** `CURL`, `ZLIB`, `SQLite3`, `nghttp2`, `X11
  + Xss` (Linux idle), `libdbus-1` (Linux desktop integrations), Breakpad
  / Crashpad (enhanced minidumps).

Run `just doctor` to check what's installed locally.

## Quick start

### Hello triangle (TypeScript)

```ts
// hello.ts
import { Window, Renderer, Primitives } from 'fxe';

const win = new Window({ width: 480, height: 320, title: 'hello fxe' });
const r = new Renderer(win);

win.run(() => {
  r.beginFrame();
  Primitives.fillRect(r, 32, 32, 200, 80, 0x4f8df1ff);
  Primitives.drawText(r, 32, 140, 'Hello, FXE', { fontSize: 24, color: 0xffffffff });
  r.endFrame();
});
```

```sh
just ts hello
```

### Counter app (JSX with fxe-ui)

```tsx
/** @jsxImportSource fxe-ui */
import { Window } from 'fxe';
import { Button, StyleSheet, Text, View, mount, useState } from 'fxe-ui';

const s = StyleSheet.create({
  root: { width: '100%', height: '100%', padding: 24, gap: 12, backgroundColor: 0x0f172aff },
  title: { color: 0xffffffff, fontSize: 22 },
});

function App() {
  const [count, setCount] = useState(0);
  return (
    <View style={s.root}>
      <Text style={s.title}>Count {count}</Text>
      <Button title="Increment" onPress={() => setCount(n => n + 1)} />
    </View>
  );
}

mount(<App />, new Window({ width: 480, height: 320 }));
```

`mount(root, window)` wires layout, paint, hit-testing, hover/press/focus, and
keyboard dispatch. It returns a disposer for listener cleanup.

`fxe-ui` defaults to React Native / Yoga semantics: `flexDirection: 'column'`,
not CSS `row`. Stable styles should go through `StyleSheet.create()` so they
remain referentially stable across renders.

## JavaScript / TypeScript API surface

Authoritative declarations live in `types/fxe.d.ts` (95 KB) and
`types/fxe-ui.d.ts`. `package.json` is a dev-only carrier for `tsc` and
`@biomejs/biome`; **no application runtime depends on Node** — the JS engine
is V8 embedded directly.

| Module / global | Provided by | Notes |
|---|---|---|
| `Window` | `bind_window.cpp` | size/title/bounds/visibility, IME compose, drag-drop, clipboard, custom title-bar |
| `Renderer`, `OffscreenRenderer`, `CommandBuffer` | `bind_renderer/offscreen/command_buffer.cpp` | Inherit a common opcode buffer; `OffscreenRenderer` for textures |
| `Primitives` | `bind_primitives.cpp` | `fillRect`, `drawText`, `drawSprite`, `drawPath`, gradients, blur, batched `drain()` |
| `Pipeline` | `bind_pipeline.cpp` | Custom WGSL pipelines with vertex/uniform/texture binding |
| `Spritesheet`, `Image` | `bind_spritesheet/image.cpp` | Atlas packing, animated sprites, `Image.fromBytes` |
| `Font` | `bind_font.cpp` | `Font.load`, `Font.system`, `Font.builtin('default')`, OpenType features/variations |
| `App` | `bind_app.cpp` | lifecycle, windows, single-instance, deep-link, recent docs, bookmarks, update |
| `Menu`, `Tray`, `Notification`, `dialog`, `shell`, `globalShortcut` | per-platform shims | Native UI integration |
| `fs`, `path`, `process`, `performance` | `bind_fs/path/process/performance.cpp` | Node-shaped APIs |
| `fetch`, `Headers`, `Request`, `Response`, `AbortController`, `Blob`, `URL`, `URLSearchParams` | `bind_fetch/url.cpp` | WHATWG subset, libcurl-backed |
| `WebSocket` | `bind_websocket.cpp` | RFC 6455, `wss://` via mbedTLS when native TLS is enabled |
| `localStorage`, `sessionStorage` | `bind_storage.cpp` | SQLite-backed |
| `fxe:sqlite` | `bind_sqlite.cpp` | `Database`, `Statement` |
| `fxe:ipc` | `bind_ipc.cpp` | `Worker`, `MessagePort`, `MessageChannel`, `BroadcastChannel` |
| `Audio`, `Sound`, `CaptureSession` | `bind_audio.cpp` | miniaudio engine + mic capture |
| `powerMonitor`, `Notification`, `Tray` | `bind_power/notification/tray.cpp` | OS event sources |
| `Print` | `bind_print.cpp` | Render command buffers to PDF pages |
| `setTimeout`/`setInterval`/`requestAnimationFrame`/`queueMicrotask` | `bind_timers.cpp` | Node + browser semantics |
| `node:*` | `runtime/node_compat*` | Adapters for `events`, `buffer`, `stream`, `path`, `url`, `util`, `process`, `os`, `net`, `dns`, `https`, `http2`, `tls`, `crypto`, `child_process`, `worker_threads`, `dgram`, … |

The runtime accepts `.ts`, `.mts`, `.cts`, and `.js` entry points.
Transpilation runs inside V8 via embedded `tsc`; it does **not** type-check.
Always run `just ts-check` (`tsc --noEmit -p tsconfig.json`) before relying on
TS examples.

## Examples

Native C++ (`examples/`):

| Example | Purpose |
|---|---|
| `hello_triangle.cpp` | Smallest demo: one triangle, paint-on-demand |
| `hello_sprite.cpp` | Sprite rendering |
| `primitives_showcase.cpp` | Rects, lines, triangles, text, sprites in one window |

TypeScript / JSX (`examples/js/`):

| Example | Purpose |
|---|---|
| `hello.ts` | Triangle via Renderer + Primitives |
| `showcase.ts` | Multiple rect/text primitives |
| `bench.ts` | CommandBuffer allocate/drain benchmark |
| `loop.ts` | Lazy vs animated event-driven main loops |
| `sprite_demo.ts` | `Image.fromBytes`, `Spritesheet.add`, `drawSprite` |
| `custom_pipeline.ts` | Custom WGSL pipeline with vertex/attr binding |
| `custom_titlebar.ts` | Frameless window with a custom title bar |
| `transparent_demo.ts` | Transparent window |
| `two_windows.ts` | Multi-window via `App.run` |
| `window_chat.ts` | `BroadcastChannel` cross-window IPC |
| `audio_demo.ts` | Audio API typecheck (guarded) |
| `git_log.ts` | `node:child_process` spawning `git log` |
| `jsx_demo.tsx` | JSX with fxe-ui counter + hooks |
| `ui_demo.ts` | Custom-canvas UI with hit testing |
| `ui_kit_demo.tsx` | Button / ScrollView / StyleSheet / form |
| `ui_reconciler_demo.ts` | Low-level reconciler `Layer` / `Draw` API |
| `login_form.tsx` | Form layout with fxe-ui |

Run any TS example with `just ts <name>` (resolves `examples/js/<name>.{ts,tsx,mts,cts,js}`).

## fxe-ui

Single JSX/TSX UI package shipped under `packages/fxe-ui/`. It owns:

- **Reconciler:** fiber tree, hooks, signals, scheduler, frame loop, devtools
  hooks (`reconciler/`).
- **Layout solver:** Yoga-style flexbox in TypeScript (`layout/solver.ts`,
  `layout/measure.ts`).
- **Style system:** CSS-like `Style` objects, `StyleSheet.create()`, color
  parsing (`style/`).
- **Components:** `View`, `Text`, `Image`, `Pressable`, `Button`,
  `ScrollView`, `TextInput`, `VirtualList` (`components/`).
- **Painting:** `view_painter`, `text_painter`, `image_painter`, `clip`
  (`paint/`).
- **Mount pipeline:** `mount(root, window)` wires layout, paint, hit-test,
  hover/press, focus, cursor (`mount/`).
- **Theme:** provider + text context (`theme/`).
- **Animated:** `Animated.timing`, `Animated.spring` (`animated/`).
- **Hooks:** `useState`, `useReducer`, `useRef`, `useEffect`, `useMemo`,
  `useContext`, `useId`, `useFrame`, `useEvent`, `useDeferredValue`,
  `useTransition`.

JSX is shipped via `jsx-runtime.ts`; use `/** @jsxImportSource fxe-ui */`.

## Debugging running apps

The debug protocol (NDJSON-over-TCP + CDP-over-WebSocket) covers Puppeteer-style
flows: evaluate JS, screenshot, mouse/keyboard injection, console tail,
pause/resume/step, fiber inspection, heap snapshots, CPU profiling.

```sh
just debug ui_demo 9333          # spawn paused on port 9333
just pycli inspect --port 9333   # handshake + globals + framebuffer
just pycli screenshot --port 9333 --out shot.png
just pycli eval --port 9333 'window.foo'
just pycli console --port 9333   # tail console.* until Ctrl-C
just pycli resume --port 9333
```

Programmatic access via the Python SDK in `clients/python/`:

```python
import asyncio
from fxe_debug import launch

async def main():
    page = await launch("examples/js/hello.ts", pause=True)
    try:
        await page.resume()
        await asyncio.sleep(0.3)
        await page.screenshot("hello.png")
        print(await page.evaluate("Object.keys(globalThis).length"))
    finally:
        await page.close()

asyncio.run(main())
```

The SDK exposes `Page` (evaluate / screenshot / console_messages / pause /
resume / step / close / framebuffer_size / globals), `Mouse` (move / click /
wheel), `Keyboard` (down / up / press / type), function tracing
(`trace_install` / `trace_drain`), layout tracing
(`layout_trace_enable` / `layout_trace_drain`), reconciler snapshots, and
heap snapshots. See `AGENTS.md` for in-depth recipes and troubleshooting
tables.

## Packaging

`tools/fxe-pack/` bundles a TS entry plus assets onto a copy of `fxe_run` and
emits one of:

- plain executable
- macOS DMG (via `hdiutil`)
- Windows MSI (via WiX, template `tools/fxe-pack/templates/wix_product.wxs.in`)
- Windows MSIX (`AppxManifest.xml.in`)
- Linux AppImage (`AppRun.in`)

Code-sign + notarize automation, signed bundle archives (`.fxa`), and binary
delta updates are tracked under Phase 8 in `TODO.md`.

## Testing

Hand-written assert harness on top of CTest — no gtest, no catch2 — chosen to
keep test binaries small.

```sh
just test dev                      # full preset
just test-core dev                 # fxe_core_tests directly
just pytest                        # Python SDK unit tests (stdlib only)
just ts-check                      # tsc --noEmit
just ci                            # format-check + test + ts-check
just ci-quick                      # format/lint/typecheck (no build)
```

C++ test targets (selection): `fxe_core_tests`, `fxe_font_tests`,
`fxe_font_render_tests`, `fxe_uv_loop_tests`, `fxe_uv_microtask_flush_tests`,
`fxe_net_http_advanced_tests`, `fxe_native_tls_tests`,
`fxe_ws_deflate_tests`, `fxe_wgpu_pipeline_cache_tests`,
`fxe_wgpu_blur_smoke`, `fxe_os_linux_smoke_tests`, `fxe_debug_tests`,
`fxe_debug_cdp_ws_tests`. TypeScript tests under `tests/*_test.ts` are
auto-registered via `fxe_add_v8_ts_test()` and run inside `fxe_run`.

CI (`.github/workflows/ci.yml`) runs `ci-quick` on every PR and a build
matrix across `ubuntu-24.04` / `macos-14` / `windows-2022` with
`FXE_FETCH_DEPS=ON FXE_ENABLE_WGPU=OFF` for a core-only smoke. Headless
Linux uses `xvfb-run`.

## Repository layout

```
include/fxe/         public C++ headers (renderer, primitives, window, font/, …)
src/core/            primitives, command buffer, spritesheet, fonts, stb impl
src/window/          GLFW window, input, IME, drag-drop
src/wgpu/            Dawn renderer, offscreen, pipeline cache, WGSL shaders
src/font/            FreeType / HarfBuzz / CoreText / Fontconfig + atlas
src/net/             HTTP, HTTP/2, WebSocket, mbedTLS client+server, cookies
src/audio/           miniaudio engine + capture
src/os/              macOS / Windows / Linux platform shims, crash handling
src/runtime/         libuv loop, fs FDs, fs watchers, node_compat, updater,
                     fxe_native (Node-like bindings), bundle_loader
src/debug/           NDJSON/TCP server, CDP-over-WS, dispatch, screenshot
src/js/              V8 host, TS transpile bridge, ~32 bind_*.cpp, fxe_run
tests/               *.cpp + *_test.ts + golden/ fixtures
examples/            native C++ demos
examples/js/         TS/TSX demos
packages/fxe-ui/     JSX UI framework (TypeScript)
clients/python/      fxe-debug-client SDK (stdlib only) + CLI + tests
tools/fxe-pack/      packaging tool + platform templates
types/               fxe.d.ts, fxe-ui.d.ts
cmake/               deps.cmake, shaders.cmake, embed_*.cmake, FindV8.cmake
scripts/             doctor.sh, build_v8.sh, build_v8.ps1
third_party/         miniaudio
vendor/              unenv (Node compat shims, generated)
```

## License

See [`LICENSE`](LICENSE).
