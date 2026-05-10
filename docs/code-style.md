# Code Style & Conventions

For the inviolable MUST-use surface (helpers, integer types) see [AGENTS.md](../AGENTS.md). This file holds the prose: rationale, examples, and the patterns that don't fit on one line.

## Formatting & naming

- **C++ formatting:** LLVM base, 2-space indent, 100 col, left pointer alignment, namespaces indented (`All`). Run `bun run fmt:cpp` before committing.
- **Naming:** C++ uses `snake_case` for functions/types matching gfw/gfwx heritage (`fillRect`, `command_buffer`, `texture_info`); JS bindings expose `camelCase` (`fillRect`, `beginFrame`). Opcode constants are `OP_FILL_RECT`-style.

## Headers & deps

- **Headers:** `include/fxe/*.hpp` is the only stable surface; everything in `src/` is internal.
- **Dependencies:** `fxe_core` links only `glm`/`glfw`/`stb`/`fxe_font`. Do **not** add a heavy dependency to `fxe_core`. Dawn and V8 are guarded by `FXE_ENABLE_WGPU` / `FXE_ENABLE_V8` and live in their own libraries (`fxe_wgpu`, `fxe_js`). libuv, mbedTLS, nghttp2, libcurl all go through `fxe_net` / `fxe_runtime`.

## Bindings

- **One file per class/namespace** in `src/js/bind_*.cpp`. GC finalizers free heap-owned C++ objects.
- **Every new JS API MUST be mirrored** in `types/fxe.d.ts` (and `types/fxe-ui.d.ts` for UI additions).
- **Hot loops:** for high-volume scenes, batch via `Primitives.drain()` (opcode/parameter arrays) rather than per-call V8 trampolines. V8 may inline through static namespaces — prototype methods are virtual and intercept reliably.
- **Memory:** command buffers reuse a vertex/index allocator across frames (`epoch()`, `allocate()`, `clear()`).
- **Errors:** bindings throw Node-shaped errors `{code, errno, syscall, path}` where applicable (`EAUDIO_DECODE`, `ERR_FXE_UPDATE_*`, etc.). Async fs surfaces `AbortError` for AbortSignal cancellation.

## JSON & protocol

Hand-written, no external JSON dep on the debug path. Extend `src/debug/json.cpp` and register handlers in `src/debug/dispatch.cpp` keyed by `Domain.method`. Expose new domains in `Schema.getDomains` and add a corresponding helper to the Python SDK.

## Shaders & pipelines

- **Shaders:** WGSL only, in `src/wgpu/shaders/*.wgsl`, embedded as headers via `embed_wgsl()` in `cmake/shaders.cmake`. Optionally validated with `tint` when `FXE_WGSL_VALIDATOR=/path/to/tint` is set.
- **Pipelines:** all Dawn pipelines go through `pipeline_cache` keyed on `{vs_entry, fs_entry, color_format, depth_format, blend_mode, topology, sample_count}` — never create raw `RenderPipeline` objects in renderer paths.

## JS/TS object-identity caches

Prefer a module-scope `Symbol('…')` with `Reflect.get` / `Reflect.set` on the carrier object rather than `WeakMap` keyed by that object. Examples: class-component tagging in `packages/fxe-ui/src/jsx-runtime.ts`, layout memo attachment in `packages/fxe-ui/src/components/View.ts`. Symbol keys stay off `Object.keys`, avoid a separate GC-managed table, and keep the cache on the key. Use ordinary maps when carriers may be sealed, frozen, or non-extensible, or when you must not touch user-owned instances.

## No `globalThis as { … }` casts

If you reach for `(globalThis as { foo: T }).foo` you are working around missing types, not adding new behaviour. The fix is always to declare the global in `types/fxe.d.ts` (runtime / native bridge / web-platform globals: `Image`, `Spritesheet`, `Markdown`, `Menu`, `process`, `navigator`, `performance`, `requestAnimationFrame`, `WebSocket`, `crypto`, `__fxe_native`, `FXE_DEBUG_PORT`, `__FXE_TYPECHECK_ONLY__`, …) or in the `declare global` block of `types/fxe-ui.d.ts` (UI-internal slots: `__FXE_DEV`, `__fxeUiEnsureFrameLoop`, `__fxeReconcilerSnapshot`, `__fxeFrameProfile`, `__fxeLayoutTrace`, `__fxeA11y`, `__fxe_devtools`).

Globals that tests need to reassign for mocking **must** be declared with `declare var` (or the `interface X` + `declare var X: { prototype; new(...) }` pattern for classes) so `globalThis.X = mock` typechecks without a cast. Do not redefine the global's shape locally with a parallel `interface ImageNamespaceWithMipHint` / `SpritesheetConstructor` / `MutableImageNamespace` type — extend the canonical declaration once and import / reference it from every consumer.
