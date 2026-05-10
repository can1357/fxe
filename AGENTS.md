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

Driven by `just` (CMake under the hood). The `dev` and `release` presets
enable the full runtime (V8 + Dawn + node compat + native TLS/HTTP2).

```bash
bun install                    # install JS devDeps (Biome, TS, @types/node)
bun run bootstrap              # one-time: clone + bootstrap in-tree vcpkg
bun run configure              # cmake --preset dev
bun run configure:release      # cmake --preset release
bun run build                  # cmake --build --preset dev
bun run build:release          # cmake --build --preset release
bun run test                   # ctest --preset dev --output-on-failure
bun run test:core              # ./build/dev/fxe_core_tests
bun run test:py                # unittest discover under clients/python/tests
bun run js ui_demo             # build (release, only if needed) + run examples/js/ui_demo
bun run js --dev hello         # same on the dev preset
bun run js --rebuild ui_demo   # force a cmake rebuild before running
bun run js ui_demo --debug=9333 --debug-pause   # any unknown flag forwards to fxe_run
bun run watch ui_demo          # rebuild + re-run on file changes (watchexec)
bun run typecheck              # tsc --noEmit -p tsconfig.json
bun run fmt / fmt:check        # biome over JS/TS
bun run fmt:cpp / fmt:cpp:check  # clang-format over tracked C/C++ sources
bun run lint / fix             # biome lint / biome check --write
bun run check                  # fmt:check + fmt:cpp:check + typecheck + lint
bun run ci                     # check + test
bun run ci:quick               # check (no build/test)
bun run doctor                 # report which dev tools are installed
```

Direct CMake equivalents work too:

```bash
cmake --preset release && cmake --build --preset release
ctest --preset release --output-on-failure
./build/release/fxe_run examples/js/hello.ts
```

## Code Conventions & Common Patterns

- **C++ formatting:** LLVM base, 2-space indent, 100 col, left pointer
  alignment, namespaces indented (`All`). Run `bun run fmt:cpp` before committing.
- **Naming:** C++ uses `snake_case` for functions/types matching gfw/gfwx
  heritage (`fillRect`, `command_buffer`, `texture_info`); JS bindings expose
  `camelCase` (`fillRect`, `beginFrame`). Opcode constants are
  `OP_FILL_RECT`-style.
- **JS/TS object-identity caches:** Prefer a module-scope `Symbol('…')` with
  `Reflect.get` / `Reflect.set` on the carrier object rather than `WeakMap`
  keyed by that object (examples: class-component tagging in
  `packages/fxe-ui/src/jsx-runtime.ts`, layout memo attachment in
  `packages/fxe-ui/src/components/View.ts`). Symbol keys stay off
  `Object.keys`, avoid a separate GC-managed table, and keep the cache on the
  key; use ordinary maps when carriers may be sealed, frozen, or
  non-extensible, or when you must not touch user-owned instances.
- **No `globalThis as { … }` casts.** If you reach for `(globalThis as { foo:
  T }).foo` you are working around missing types, not adding new behaviour. The
  fix is always to declare the global in `types/fxe.d.ts` (runtime / native
  bridge / web-platform globals: `Image`, `Spritesheet`, `Markdown`, `Menu`,
  `process`, `navigator`, `performance`, `requestAnimationFrame`, `WebSocket`,
  `crypto`, `__fxe_native`, `FXE_DEBUG_PORT`, `__FXE_TYPECHECK_ONLY__`, …) or in
  the `declare global` block of `types/fxe-ui.d.ts` (UI-internal slots:
  `__FXE_DEV`, `__fxeUiEnsureFrameLoop`, `__fxeReconcilerSnapshot`,
  `__fxeFrameProfile`, `__fxeLayoutTrace`, `__fxeA11y`, `__fxe_devtools`).
  Globals that tests need to reassign for mocking **must** be declared with
  `declare var` (or the `interface X` + `declare var X: { prototype; new(...) }`
  pattern for classes) so `globalThis.X = mock` typechecks without a cast. Do
  not redefine the global's shape locally with a parallel `interface
  ImageNamespaceWithMipHint` / `SpritesheetConstructor` / `MutableImageNamespace`
  type — extend the canonical declaration once and import / reference it from
  every consumer.
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
- **Per-isolate template caches (`<fxe/v8_template_cache.hpp>`):** Bindings
  that memoize a `Global<FunctionTemplate>` per isolate **MUST** use
  `fxe::js::template_isolate_cache<Tag>` instead of hand-rolling the
  `xxx_tpl_table()` map / `xxx_reset_for_isolate` thunk / `xxx_resetter_register`
  + static instance quartet. Each `Tag` instantiation auto-registers its
  resetter on first table use, so isolate teardown stays correct without
  per-binding boilerplate.

  ```cpp
  // bind_foo.cpp
  namespace {
    struct foo_tag {};
    using foo_tpl_cache = template_isolate_cache<foo_tag>;
  }

  void install_foo_template(Isolate* iso, Local<ObjectTemplate> global) {
    auto tpl = FunctionTemplate::New(iso, foo_ctor);
    /* … configure tpl … */
    global->Set(iso, "Foo", tpl);
    foo_tpl_cache::install(iso, tpl);          // stash for this isolate
  }

  Local<Object> make_foo_object(Isolate* iso, Local<Context> ctx, foo* native) {
    return wrap(iso, ctx, foo_tpl_cache::resolve(iso), native, TAG_FOO);
  }
  ```

  Use `::install(iso, tpl)` and `::resolve(iso)` for the common write/read
  paths; reach for `::table()` only when you need the underlying
  `unordered_map` (rare — diagnostic dumps, multi-template fetches).
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
- **Logging (`<fxe/log.hpp>`):** All C++ diagnostics **MUST** route through
  the `FXE_TRACE` / `FXE_DEBUG` / `FXE_INFO` / `FXE_WARN` / `FXE_ERROR` /
  `FXE_CRITICAL` macros. Do **not** use `printf`, `fprintf(stderr, …)`,
  `std::cout` / `std::cerr`, `std::clog`, or raw `spdlog::*` calls in new
  code — they bypass the per-category level gate and the SPDLOG_LEVEL /
  FXE_LOG_LEVELS env-var routing. Pick a dotted category that matches the
  subsystem (`font.atlas`, `font.cache`, `wgpu.renderer`, `js.host`,
  `debug.dispatch`, …) and reuse existing categories rather than coining
  new ones for the same area. Hot-loop diagnostics go at `trace`; opt-in
  per-frame streams at `debug`; one-shot startup messages at `info`;
  unexpected-but-recoverable states at `warn`; failures at `err`. The
  `category` argument **MUST** be a string literal so the call-site
  cache collapses to a single static logger handle. When the level is
  off, argument formatting is skipped — write `FXE_TRACE("font.cache",
  "miss gid={}", gid)` rather than pre-building strings.
- **V8 binding helpers (`<fxe/v8_helpers.hpp>`):** Bindings **MUST** route
  through these instead of hand-rolled equivalents. The header is the single
  source of truth for every "convert / throw / wrap / set / get" V8 idiom.
  Adding a local `utf8(...)` / `to_str(...)` / `s(iso, ...)` / `set_fn(...)`
  / `int_option(...)` / `throw_msg(...)` helper to a binding is rejected on
  review — extend `v8_helpers.hpp` (or a `v8_value_of<T>` specialisation)
  instead. Catalogue:

  **External / native wrapping**
  | Signature | Purpose |
  |---|---|
  | `make_external(iso, ptr) -> Local<External>` | Wrap any `T*` into an `External` with the default external-pointer tag. |
  | `external_ptr<T>(value\|ext\|data) -> T*` | Recover `T*` from a `Local<Value>` / `Local<External>` / `Local<Data>` (no checks). |
  | `internal_ptr<T>(obj, slot=0) -> T*` | Recover `T*` from `obj->GetInternalField(slot)`; caller validates the slot/tag. |
  | `set_native(iso, obj, ptr, tag) -> void` | Assign internal field 0 = `External(ptr)`, field 1 = `Uint32(tag)` in one call. |

  **Throwers** (return `false`; format-string overloads accept `std::format_string<…>` + args)
  | Signature | Purpose |
  |---|---|
  | `throw_error(iso, msg \| fmt, args…) -> bool` | Throw `new Error(msg)`; always returns `false`. |
  | `throw_type_error(iso, msg \| fmt, args…) -> bool` | Throw `new TypeError(msg)`; always returns `false`. |
  | `throw_range_error(iso, msg \| fmt, args…) -> bool` | Throw `new RangeError(msg)`; always returns `false`. |
  | `throw_coded_error(iso, ctx, code, msg \| fmt, args…) -> void` | Throw `Error` with `err.code = code` (Node-style `{code, …}`). |
  | `throw_named(iso, ctx, name, msg \| fmt, args…) -> void` | Throw `Error` with `err.name = name` (DOM-style `AbortError`, `NoHandler`, …). |
  | `throw_exception(iso, ctx, code, name, msg \| fmt, args…) -> void` | Throw `Error` with both `code` and `name` set. |

  **Primitives**
  | Signature | Purpose |
  |---|---|
  | `to_v8_undefined(iso) -> Local<Primitive>` | `v8::Undefined(iso)`. |
  | `to_v8_null(iso) -> Local<Primitive>` | `v8::Null(iso)`. |

  **C++ → V8 (`to_v8`)** — single entry point dispatched via `v8_value_of<T>`
  | Signature | Purpose |
  |---|---|
  | `to_v8(iso, value) -> Local<…>` | Convert any `T` with a `v8_value_of<T>` specialisation; result type is whatever the specialisation returns. |
  | `template<> struct v8_value_of<T>` | Customisation point — define `static Local<X> to(Isolate*, T)` to plug a new C++ type into `to_v8`, `set_prop`, `set_index`, `add_function`, `get_prop`/`set_prop` keys, container element conversion, and `optional` wrapping. |
  | `to_v8_string(iso, string_view) -> Local<String>` | UTF-8 `String::NewFromUtf8` with `kNormal` for dynamic strings. |
  | `to_v8_string(iso, u16string_view) -> Local<String>` | UTF-16 `String::NewFromTwoByte` for `std::u16string{,_view}` / `char16_t*`. |
  | `to_v8_string_internalized(iso, const char*, length=-1) -> Local<String>` | UTF-8 with `kInternalized` so V8's string table dedupes static literals. |

  Built-in `v8_value_of` specialisations (use directly via `to_v8` — do not duplicate):
  - `bool` → `Boolean`
  - signed integral ≤ 32-bit → `Integer` (`Integer::New`)
  - unsigned integral ≤ 32-bit → `Integer` (`Integer::NewFromUnsigned`)
  - `i64` / `u64` → `Number` (precision lost above 2^53; reach for `BigInt::New{,FromUnsigned}` directly when 64-bit precision is required)
  - `f32` / `f64` → `Number`
  - `nullptr_t`, `nullopt_t` → `Null`
  - `std::optional<T>` → `Null` when empty, else `to_v8(*opt)`
  - `std::string`, `std::string_view`, `char*` → `String` (`kNormal`)
  - `const char*`, `char[N]` → `String` (`kInternalized`; pure win for literals)
  - `std::u16string{,_view}`, `const char16_t*`, `char16_t[N]` → `String` (UTF-16)
  - `std::span<T>` / `std::vector<T>` / `std::array<T,N>` / raw `T[N]` → `Array` (each element re-enters `to_v8`)
  - `v8::Local<T>` → passthrough
  - `v8::Global<T>`, `v8::Eternal<T>` → auto `.Get(iso)`
  - `"foo"_v8`, `42_v8`, `1.5_v8` (literals from `<fxe/v8_literals.hpp>`) → cached `Eternal` lookup

  **V8 → C++ (`from_v8`)** — strict mirror of `to_v8`, returns `std::optional<T>`
  | Signature | Purpose |
  |---|---|
  | `from_v8<T>(ctx, value) -> optional<T>` | Convert a `Local<Value>` to `T` via `v8_value_from<T>`; returns `nullopt` when shape is wrong. |
  | `template<> struct v8_value_from<T>` | Customisation point — define `static optional<T> from(Local<Context>, Local<Value>)`. |
  | `to_std_string(iso, v) -> string` | **Lossy.** Runs `Utf8Value` (calls JS `ToString` on non-strings); empty on failure. Replaces every per-TU `utf8()` / `to_str()` / `string_arg()` helper. |
  | `to_std_string_strict(iso, v) -> string` | Returns empty unless `v->IsString()`; use when you reject coercion. |

  Built-in `v8_value_from` specialisations:
  - `bool` (strict, `IsBoolean()` only)
  - signed integral ≤ 32-bit (`Int32Value`)
  - unsigned integral ≤ 32-bit (`Uint32Value`)
  - `i64` / `u64` (`IntegerValue`, cast)
  - `f32` / `f64` (`NumberValue`)
  - `std::string` (UTF-8 via `Utf8Value`)
  - `std::u16string` (UTF-16 via `WriteV2`)
  - `std::optional<T>` (null/undefined → empty, else recurse)
  - `std::vector<T>` (Array; each element re-enters `from_v8`)
  - `Local<Value>` (passthrough)
  - `Local<Object|String|Number|Integer|Boolean|Array|Function|ArrayBuffer|Uint8Array|Map|Set|Promise|Date|RegExp|Symbol|BigInt|External>` (strict, gated by the matching `IsX()`)

  **Property I/O** — keys flow through `to_v8`, so any of `"foo"_v8`, `const char*`, `string_view`, `Local<Name>`, `Global<Name>` all work.
  | Signature | Purpose |
  |---|---|
  | `set_prop(ctx, obj, key, value)` | `obj->Set(ctx, to_v8(key), to_v8(value))`. Replaces every raw `obj->Set(ctx, "X"_v8(iso), …)`. |
  | `set_index(ctx, obj, u32, value)` | Indexed `obj->Set(ctx, idx, to_v8(value))`. |
  | `define_prop(ctx, obj, key, value, attrs=None)` | `Object::DefineOwnProperty` — `set_prop` with `PropertyAttribute` bits (`v8::DontEnum`, `v8::ReadOnly`, `v8::DontDelete`). Use for `__fxe_native`-style hidden slots and read-only constants. |
  | `get_prop<T>(ctx, obj, key) -> optional<T>` | `Get` + `from_v8<T>`; `nullopt` on missing or wrong shape. |
  | `get_prop_or<T>(ctx, obj, key, fallback) -> T` | `get_prop<T>(...).value_or(fallback)`. Replaces every `int_option` / `bool_option` / `string_option` / `object_*_prop` / `get_optional_*` helper. |
  | `get_index<T>(ctx, obj, u32) -> optional<T>` | Indexed mirror of `get_prop`. |

  **Function installers**
  | Signature | Purpose |
  |---|---|
  | `add_function(ctx, obj, name, cb, data={}) -> Local<Function>` | `Function::New(ctx, cb, data).ToLocalChecked()` + `set_prop(ctx, obj, name, fn)` in one call. |
  | `add_function<&Fn>(ctx, obj, data={}) -> Local<Function>` | Auto-named: derives the JS property name from the C++ identifier of `&Fn` via `xstd::const_tag` (e.g. `&power_inhibit_sleep` → `"power_inhibit_sleep"`). Use the string overload above when JS name should differ. |
  | `function_name<&Fn>() -> string_view` | Compile-time unqualified name of `Fn` (strips `&` + namespace prefix); building block for `add_function<&Fn>`. |

  **Idioms enforced on review**
  - `get_prop` / `get_index` default `T = v8::Local<v8::Value>`, so
    `if (auto v = get_prop(ctx, obj, "k"_v8))` works without angle brackets
    when you just want the raw value.
  - **Inside helpers (`set_prop`, `get_prop`, `set_index`, `get_index`,
    `add_function`), drop `(iso)` from `_v8` literals.** The literal types
    (`v8_string_literal`, `v8_integer_literal`, `v8_float_literal`) have
    `v8_value_of` specialisations, so the helper's `to_v8` call materialises
    the cached `Local<String>` itself. Write `set_prop(ctx, obj, "k"_v8,
    v)`, **not** `set_prop(ctx, obj, "k"_v8(iso), v)`. The `(iso)` form is
    still required at the rare raw-V8 boundary (`Exception::Error("msg"_v8(iso))`,
    `iso->ThrowError("..."_v8(iso))`, `obj->StringEquals("k"_v8(iso))` when
    `Isolate::GetCurrent()` isn't valid).
  - Use `to_v8(iso, x)` for any value crossing into V8 — bindings should never call `Integer::New` / `Number::New` / `Boolean::New` / `String::NewFromUtf8` directly. The only legitimate exception is `BigInt::New{,FromUnsigned}` for exact 64-bit values.
  - Argument coercion at call entry (`info[i]->Int32Value(ctx).FromMaybe(0)`) stays raw — it's intentionally lax JS semantics. Option-bag reads (`obj.foo`) go through `get_prop_or<T>`.
  - Never declare a local `utf8 / to_str / to_string / string_arg / utf8_arg / s / str / js_string / s8 / set_fn / add_native_fn / int_option / bool_option / string_option / get_optional_string / get_optional_bool / int_prop / bool_prop / string_prop / object_*_prop / throw_msg / throw_js_error / throw_native_error` helper. Each of these has a header equivalent above; if behaviour genuinely differs, extend the header.
- **V8 literals (`_v8(iso)`):** Use the user-defined literals from
  `<fxe/v8_literals.hpp>` for values passed across the V8 boundary —
  `obj->Get(ctx, "width"_v8(iso))`, `obj->Set(ctx, "name"_v8(iso), value)`,
  `obj->Set(ctx, "fd"_v8(iso), 0_v8(iso))`. String literals return a per-isolate
  internalized `v8::Local<v8::String>` cached in an `Eternal` slot, so
  repeated calls are a hash lookup, not a fresh `String::NewFromUtf8`.
  Don't write `String::NewFromUtf8(iso, "width").ToLocalChecked()` in
  bindings — it's slower and noisier. The cache is installed/uninstalled
  per isolate via `install_string_cache` / `uninstall_string_cache`; new
  isolates spun up outside the host must call `install_string_cache`
  before any binding code runs (a missing cache falls back to an
  uncached internalized string, never an empty handle).
  **String equality:** compare a dynamic `v8::Local<v8::String>` to a
  literal with `s == "flex"_v8` (also ` "flex"_v8 == s`). Do not reintroduce
  `s->StringEquals("flex"_v8(iso))` for routine checks — the `operator==` /
  `operator!=` in `<fxe/v8_literals.hpp>` materialises the same cached
  internalized literal via `v8::Isolate::GetCurrent()` and delegates to
  `StringEquals`. If `GetCurrent()` is not valid for the call site (not on
  the entered isolate), use the explicit `s->StringEquals("…"_v8(iso))`
  form instead.
- **V8 weak callbacks (`Global<T>::SetWeak`):** V8 requires the
  first-pass weak callback to either `Reset()` the persistent that
  triggered it or call `SetSecondPassCallback()`. Doing neither aborts
  the process with `Handle not reset in first callback` during the next
  GC. The repo convention: store the persistent on the holder so the
  finalizer can reset and free it.

  ```cpp
  struct foo_holder {
    /* … real fields … */
    v8::Global<v8::Object>* persistent = nullptr;
  };

  void foo_finalizer(const v8::WeakCallbackInfo<foo_holder>& info) {
    auto* h = info.GetParameter();
    if (h && h->persistent) {
      h->persistent->Reset();
      delete h->persistent;
    }
    delete h;
  }

  // wrap site
  auto* persistent = new v8::Global<v8::Object>(iso, obj);
  h->persistent = persistent;
  persistent->SetWeak(h, foo_finalizer, v8::WeakCallbackType::kParameter);
  ```

  Equivalent alternative used in a few bindings: store
  `v8::Global<v8::Object> self;` directly on the holder, call
  `h->self.SetWeak(h, finalizer, kParameter)`, and `h->self.Reset()` in
  the finalizer. Either pattern is fine; never `delete info.GetParameter()`
  on its own without resetting the persistent.

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
- Object-identity memoization (layouts keyed on props object identity, AST
  handles in demos/tests, etc.) should use `Symbol` + `Reflect.get` /
  `Reflect.set`, not `WeakMap` — see **JS/TS object-identity caches** under
  [Code Conventions](#code-conventions--common-patterns).
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
  `FXE_FETCH_DEPS=ON`, plus `-DFXE_ENABLE_WGPU=OFF -DFXE_ENABLE_V8=OFF` for core-only smoke
- `TODO.md` — running roadmap and per-module gap audit

## Runtime/Tooling Preferences

- **Build:** CMake ≥ 3.24, Ninja generator, vcpkg manifest mode.
  `bun run bootstrap` builds the in-tree vcpkg.
- **C++:** C++20. Compilers: clang/gcc/MSVC matched in CI.
- **TypeScript:** `tsc` (devDep `typescript ^5.9.3`) for type-checking only —
  `tsconfig.json` targets ES2022, Node16 modules. Runtime transpilation is
  performed inside V8 and does **not** type-check; always run `bun run
  typecheck` before relying on TS examples.
- **Bun/Node:** Bun is the package manager and script runner (`bun install`,
  `bun run X`). The JS toolchain (Biome, tsc, @types/node) lives in `package.json`
  devDeps. No application runtime depends on Node — the JS runtime is V8
  embedded directly. Do not introduce JS runtime deps.
- **V8:** prefer system/distro V8; otherwise vendor with
  `scripts/build_v8.sh` (or `.ps1`) which uses depot_tools and installs to
  `.vendor/v8-install/`. Configure with `-DFXE_ENABLE_V8=ON` and export
  `V8_ROOT` or `V8_DIR`. `--expose_gc` is enabled at host init for the
  HeapProfiler.
- **Dawn:** supply externally; configure with `-DDawn_DIR=…` or
  `CMAKE_PREFIX_PATH`. Not auto-fetched.
- **libuv / mbedTLS / nghttp2:** via vcpkg manifest. `FXE_ENABLE_LIBUV` is
  `ON` by default (required for async fs/net);
  `FXE_ENABLE_NATIVE_TLS_HTTP2` is `ON` by default and enables the native
  HTTPS / HTTP/2 transport in `src/runtime/v8/native/*.cpp`.
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
- **Run:** `bun run test` (full preset), `bun run test:core` (core exe directly),
  `bun run test:py` (Python SDK).
- **Golden tests:** deterministic FNV-1a hashes over command buffers in
  `core_tests.cpp` (showcase + text/sprite). Pixel goldens under
  `tests/golden/` are reserved for Dawn-backed frame capture; not yet wired.
- **Type checks:** `bun run typecheck` — required
  before merging TS-touching changes.
- **Format gate:** `bun run fmt:cpp:check` + `bun run fmt:check` — CI fails
  on diffs.
- **Local CI parity:** `bun run ci` (= `check` + `test`; `check` is
  `fmt:check` + `fmt:cpp:check` + `typecheck` + `lint`).
- **Headless Linux:** CI uses `xvfb`; replicate with `xvfb-run -a bun run test`
  if no display is available.
- **Python SDK:** `bun run test:py` runs `unittest discover` under
  `clients/python/tests`.

## Debugging FXE Apps with the Python SDK

When you (the agent) need to verify behavior of a JS/TS example or a user
project — e.g. "does my new primitive draw correctly?", "did this script
actually call `endFrame`?", "what does `console.log` print at frame 30?" —
drive `fxe_run` through the Python SDK rather than reading source and
guessing. The SDK is Puppeteer-style: launch the app, attach, evaluate,
screenshot, inject input.

**Prerequisite:** `bun run build` (default `dev` preset includes V8 + Dawn).

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
  ("retry after the next render"); while readback is still pending it may return
  "capture in progress; retry shortly". The SDK does not auto-retry — sleep for a
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
bun run js --dev hello --debug=9333 --debug-pause   # spawn paused on port 9333
bun run pycli inspect --port 9333    # handshake + globals + framebuffer
bun run pycli screenshot --port 9333 --out shot.png
bun run pycli eval --port 9333 'window.foo'
bun run pycli mouse click --port 9333 100 100
bun run pycli console --port 9333    # tail console.* messages until Ctrl-C
bun run pycli resume --port 9333
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

### Memo trace (fxe-ui reconciler)

When a `memo()`-wrapped component is rebuilding more than expected ("why did
`Sidebar` re-render when nothing visible changed?"), `page.memo_trace_*`
surfaces the exact bail decision the reconciler made each render. Every
memoised component is bucketed by displayName into one of:

- `hit` — bail succeeded (cache reused)
- `dirty` — fiber explicitly marked dirty (setState in a child, etc.)
- `noCache` — first render or cache discarded
- `noLastProps` — first render with this fiber identity
- `epoch` — atlas repacked under the cache; forced rebuild for correctness
- `propsDiff` — `areEqual(prev, next)` returned false

The first observed `propsDiff` per component also captures `{last, next,
lastKeys, nextKeys}` so you can see exactly which prop changed. Cost when
disabled: one nullable-load + branch per memoised component per render.

```python
await page.memo_trace_enable()
await asyncio.sleep(1.0)              # let the suspect frames go by
snap = await page.memo_trace_snapshot()
await page.memo_trace_disable()

for name, slot in sorted(snap["byName"].items(), key=lambda kv: -kv[1]["propsDiff"]):
    if slot["propsDiff"] == 0: continue
    print(f"{name:20s} total={slot['total']:5d} hit={slot['hit']:5d} "
          f"propsDiff={slot['propsDiff']:5d} dirty={slot['dirty']:5d}")
    dump = snap["propsDump"].get(name)
    if dump:
        added = set(dump["nextKeys"]) - set(dump["lastKeys"])
        removed = set(dump["lastKeys"]) - set(dump["nextKeys"])
        print(f"  +keys={sorted(added)} -keys={sorted(removed)}")
```

`memo_trace_reset()` zeros the counters without disabling, useful for "snap
before / snap after a single user gesture". Calling on an app that hasn't
imported `fxe-ui` raises (the devtools module installs the global on import);
that's intentional — memo tracing only makes sense for UI apps.

### When NOT to use the SDK

- Modifying the script under test — just edit + re-run `bun run js …`.
- Pure C++ unit tests — use `fxe_core_tests` / `fxe_debug_tests`.
- Type errors — `bun run typecheck` is faster than launching V8.

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `LaunchError: FXE_DEBUG_PORT not detected` | binary missing or build stale | `bun run build` |
| `ProtocolError(-32002, "V8 host not attached")` | called Runtime.* before the host loaded | wait for handshake / `await asyncio.sleep(0.05)` |
| `ProtocolError(-32001, "capture armed; retry after the next render")` or `"capture in progress; retry shortly"` | screenshot capture is not ready yet | retry after a short sleep (see helper above) |
| `ProtocolError(-32002, "window not attached")` / `"renderer not attached"` | the script hasn't run `new Window`/`new Renderer` yet | the SDK should `await page.resume()` if launched paused; otherwise wait |
| Hangs on close | child blocked in `win.run` | the SDK's `__aexit__` sends `Window.close`; if your script ignores close, also call `await page.evaluate("window.close()")` |
| Screenshot all transparent | the renderer hasn't rendered yet — first arming returns nothing | second call after the next `endFrame` will succeed |

## Dev Tooling

Beyond the C++ build/test recipes, `package.json` defines a small set of
scripts. Run `bun run doctor` to see which tools are present.

- **Required:** `bun`, `cmake`, `ninja`, `clang-format`, `python3` (`tsc` is
  installed locally via `bun install`).
- **Optional (soft-skip if missing):**
  - `ruff` — Python format + lint (call directly: `ruff format clients/python`,
    `ruff check clients/python`).
  - `shfmt` / `shellcheck` — shell formatting + linting.
  - `gersemi` — CMake formatting.
  - `tint` — WGSL validation (set `FXE_WGSL_VALIDATOR` or have it on PATH).
  - `watchexec` — `bun run watch <example>` rebuild loop.

Aggregate scripts: `bun run fmt` (writes), `bun run lint` (read-only),
`bun run check` (fmt-check + cpp-fmt-check + typecheck + lint),
`bun run ci:quick` (= `check`), `bun run ci` (= `check` + `test`). The CI
workflow runs `ci:quick` on every PR; the matrix build still runs the
native test smoke.
