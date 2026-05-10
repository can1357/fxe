# Architecture & Data Flow

Layered. Each upper layer is optional, and the lower ones link cleanly without the rest.

## Module layers

1. **Core** (`fxe_core`) — pure C++ primitives, command buffer, spritesheet, render-stats, font-stack glue. No GPU, no JS. Deps: `glm`, `glfw`, `stb`, `fxe_font`, `fxe_shaders`.
2. **Window** (`fxe_window`, `src/window/glfw_window.cpp`) — GLFW window, input, IME, drag-drop, clipboard, custom title-bar plumbing, native handle extraction.
3. **WGPU** (`fxe_wgpu`, optional, `FXE_ENABLE_WGPU`) — Dawn/WebGPU renderer, pipeline cache, offscreen targets, frame capture, blur post-process. WGSL under `src/wgpu/shaders/`. A null fallback (`renderer_wgpu.cpp`) keeps command-buffer accounting alive for headless tests.
4. **Font** (`fxe_font`) — FreeType / HarfBuzz / CoreText / Fontconfig matrix selected by `FXE_FONT_BACKEND`. R8 mask + BGRA color emoji atlas pages, shelf packer, OpenType feature/variation support, per-platform discovery.
5. **Net** (`fxe_net`) — libcurl HTTP client (cookies, multipart, proxy), nghttp2 HTTP/2 client+server, RFC 6455 WebSocket client (incl. `wss://` via mbedTLS), mbedTLS client+server with session resumption, persistent cookie jar.
6. **Audio** (`fxe_audio`) — miniaudio engine, in-memory decode, mic capture via `ma_device` with main-thread queue drain.
7. **OS** (`fxe_os`) — per-platform shims for App lifecycle, dialogs, notifications, menus, tray, power, recent-documents, single-instance, crash dumps. macOS uses AppKit/UserNotifications, Windows uses Win32 + Toast XML, Linux uses D-Bus + libnotify (gated by `FXE_OS_DBUS`).
8. **Runtime** (`fxe_runtime`) — libuv event loop, fs FDs, fs watchers (inotify / FSEvents / `ReadDirectoryChangesW`), `node_compat` module loader with unenv adapters, `fxe_native` (Node-shaped bindings), updater (signed feeds, channels, rollback, signing-authority checks), `bundle_loader` for packaged apps.
9. **Debug** (`fxe_debug`) — NDJSON-over-TCP and CDP-over-WebSocket servers, hand-written JSON parser, base64, screenshot encoder, dispatch table for `System.*`, `Console.*`, `Runtime.*`, `Page.*`, `Input.*`, `Window.*`, `Debugger.*`, `Schema.*`, `Reconciler.*`, `Profiler.*`, `HeapProfiler.*`, `Fetch.*`, `Fs.*`.
10. **JS host** (`fxe_js`, optional, `FXE_ENABLE_V8`) — V8 isolate, embedded `tsc` transpile, ES module loader (incl. `fxe-ui`, `fxe:sqlite`, `fxe:ipc` synthetic modules), source maps for stack traces, ~32 `bind_*.cpp` files exposing renderer, window, app, fs, fetch, websocket, storage, audio, font, sqlite, ipc, timers, performance, menu, tray, dialog, shell, notifications, power, process, image, spritesheet, pipeline, offscreen, print, path, url, render-stats, global-shortcut, crash. HMR via `__fxe_hmr`.
11. **Runner** (`fxe_run`, `src/js/fxe_run.cpp`) — CLI: parses `--debug` / `--debug-pause`, initializes V8, creates host + window + renderer, runs `.ts` / `.mts` / `.cts` / `.js` script.

## Data flow (TS app frame)

TS source → V8 + embedded `tsc` (transpile only, no type-check) → JS calls `Primitives.fillRect(cb, …)` → C++ binding writes opcodes into `CommandBuffer` → `Renderer.endFrame()` uploads vertex/index buffers and submits Dawn queue → GPU → optional `Page.screenshot` reads back framebuffer over the debug protocol.

## Threading

V8 and the GPU are pinned to the main thread. The debug server uses an accept thread plus a session thread per connection; commands are posted onto a render-thread task pump that drains between frames. libuv runs on the main thread (microtask checkpoint after every `uv_run(UV_RUN_NOWAIT)`). Network workers (HTTP, WebSocket, native TLS) own their own threads and post results back through the loop. fs watchers own a dedicated thread per platform. The audio engine handles its own threads internally.
