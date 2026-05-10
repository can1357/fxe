# Repository Guidelines

FXE is an immediate-mode application platform — alternative to Electron with real GPU graphics. C++20 core, embedded V8 + TypeScript runtime, optional Dawn/WebGPU. Apps render through a native command buffer; a Python SDK drives running instances over an NDJSON debug protocol.

> **Debugging running apps:** drive `fxe_run` through the [Python SDK](docs/python-sdk.md). When a JS/TS example or user project misbehaves, you **MUST** `launch()` + `evaluate` / `screenshot` / `mouse` / `keyboard` / `trace_install` rather than guess from source. Source-reading is for static questions; runtime questions ("did this fire?", "what args?", "what's on screen at frame N?") go through the SDK.

## Reference docs

- [Architecture & data flow](docs/architecture.md) — module layers, frame pipeline, threading.
- [Key directories](docs/key-directories.md) — what lives where.
- [Development commands](docs/development.md) — `bun run` recipes and CMake equivalents.
- [Code style & conventions](docs/code-style.md) — formatting, naming, JS/TS object-identity caches, globals discipline, headers, dependencies, hot loops, JSON, shaders, pipelines, errors.
- [V8 helpers (`<fxe/v8_helpers.hpp>`)](docs/v8-helpers.md) — full catalogue of `to_v8` / `from_v8` / property I/O / function installers.
- [V8 bindings](docs/v8-bindings.md) — literals, weak callbacks, template caches.
- [fxe-ui toolkit](docs/fxe-ui.md) — JSX components, hooks, `mount()`.
- [Important files](docs/important-files.md) — the load-bearing paths.
- [Runtime / tooling preferences](docs/runtime-tooling.md) — V8, Dawn, libuv, Bun, Python.
- [Testing & QA](docs/testing.md) — C++ + TS test targets, gates.
- [Debugging FXE apps with the Python SDK](docs/python-sdk.md) — `launch`, `evaluate`, `screenshot`, trace helpers.
- [Dev tooling](docs/dev-tooling.md) — required + optional CLI tools.

## Style guide — MUST-use surface

The inviolable bits. Full rationale + examples live in the docs above; this section is the cheat sheet.

### Debugging running apps

Use the [Python SDK](docs/python-sdk.md) — `launch(script, pause=True)`, then `await page.evaluate("…")` / `page.screenshot(path)` / `page.mouse.click(x,y)` / `page.trace_install(target, capture)`. The SDK is Puppeteer-style and runs against the live V8 isolate over the NDJSON debug protocol. **MUST NOT** add `console.log` lines and rebuild when the SDK can answer the question without touching the script. CLI fallback for one-shot probes: `bun run pycli {inspect|screenshot|eval|mouse|console|resume}`.

### Integer types (`<fxe/types.hpp>`)

| Alias | Concrete |
|---|---|
| `i8` / `i16` / `i32` / `i64` | signed integers |
| `u8` / `u16` / `u32` / `u64` | unsigned integers |
| `usize` / `isize` | pointer-sized (`size_t` / `ptrdiff_t`) |
| `f32` / `f64` | floating-point |

**MUST** use these aliases instead of `std::uint32_t`, `uint32_t`, `size_t`, `float`, `double`, etc.

### Logging (`<fxe/log.hpp>`)

- `FXE_TRACE("category", fmt, …)` — hot-loop diagnostics.
- `FXE_DEBUG("category", fmt, …)` — opt-in per-frame streams.
- `FXE_INFO("category", fmt, …)` — one-shot startup messages.
- `FXE_WARN("category", fmt, …)` — recoverable surprises.
- `FXE_ERROR("category", fmt, …)` — failures.
- `FXE_CRITICAL("category", fmt, …)` — fatal.

`category` **MUST** be a string literal so the call-site cache collapses to a single static logger handle. Reuse existing dotted names (`font.atlas`, `font.cache`, `wgpu.renderer`, `js.host`, `debug.dispatch`, …). **MUST NOT** use `printf` / `fprintf` / `std::cout` / `std::cerr` / `std::clog` / raw `spdlog::*` in new code — they bypass the level gate.

### V8 binding helpers (`<fxe/v8_helpers.hpp>`)

External / native wrapping:

- `make_external(iso, ptr) -> Local<External>` — wrap any `T*`.
- `external_ptr<T>(value|ext|data) -> T*` — recover `T*` (no checks).
- `internal_ptr<T>(obj, slot=0) -> T*` — recover `T*` from internal field.
- `set_native(iso, obj, ptr, tag)` — internal field 0 = External(ptr), field 1 = Uint32 tag.

Throwers (return `false`; format-string overloads accept `std::format_string<…>` + args):

- `throw_error(iso, msg|fmt, …) -> bool` — `new Error`.
- `throw_type_error(iso, msg|fmt, …) -> bool` — `new TypeError`.
- `throw_range_error(iso, msg|fmt, …) -> bool` — `new RangeError`.
- `throw_coded_error(iso, ctx, code, msg|fmt, …)` — `Error` with `.code`.
- `throw_named(iso, ctx, name, msg|fmt, …)` — `Error` with `.name`.
- `throw_exception(iso, ctx, code, name, msg|fmt, …)` — `Error` with both.

Conversions:

- `to_v8(iso, value) -> Local<…>` — single C++→V8 entry point; dispatches via `v8_value_of<T>`.
- `from_v8<T>(ctx, value) -> optional<T>` — single V8→C++ entry point; strict, returns `nullopt` on shape mismatch.
- `to_v8_string(iso, …)` / `to_v8_string_internalized(iso, …)` — explicit string variants when you don't have a value type to dispatch on.
- `to_v8_undefined(iso)` / `to_v8_null(iso)` — primitives.
- `to_std_string(iso, v)` — lossy UTF-8 (calls JS `ToString` on non-strings).
- `to_std_string_strict(iso, v)` — empty unless `v->IsString()`.

Property I/O (keys flow through `to_v8`):

- `set_prop(ctx, obj, key, value)` — write.
- `get_prop<T>(ctx, obj, key) -> optional<T>` — read.
- `get_prop_or<T>(ctx, obj, key, fallback) -> T` — read with default.
- `set_index(ctx, obj, u32, value)` / `get_index<T>(ctx, obj, u32)` — indexed mirror.
- `define_prop(ctx, obj, key, value, attrs=None)` — `DefineOwnProperty` w/ `PropertyAttribute` bits.

Function installers:

- `add_function(ctx, obj, name, cb, data={}) -> Local<Function>` — `Function::New` + `set_prop`.
- `add_function<&Fn>(ctx, obj, data={})` — auto-named from C++ identifier.

**MUST NOT** declare local `utf8 / to_str / int_option / bool_option / string_option / set_fn / throw_msg / …` helpers — extend the header instead. Full catalogue + the `v8_value_of` / `v8_value_from` customisation points: [docs/v8-helpers.md](docs/v8-helpers.md).

### V8 literals (`<fxe/v8_literals.hpp>`)

- `"foo"_v8(iso)` — internalized cached `Local<String>`.
- `42_v8(iso)`, `1.5_v8(iso)` — cached `Local<Number>`.
- Inside `set_prop` / `get_prop` / `set_index` / `get_index` / `add_function` etc., **drop `(iso)`** — the literal type has a `v8_value_of` specialisation.
- `s == "flex"_v8` — string equality via cached `StringEquals`.

### Per-isolate template caches (`<fxe/v8_template_cache.hpp>`)

- `struct foo_tag {}; using foo_tpl_cache = template_isolate_cache<foo_tag>;` — declare once.
- `foo_tpl_cache::install(iso, tpl)` — stash per-isolate.
- `foo_tpl_cache::resolve(iso) -> Local<FunctionTemplate>` — recover.
- `foo_tpl_cache::table()` — raw `unordered_map` (rare).

**MUST NOT** roll the `xxx_tpl_table()` / `xxx_reset_for_isolate` / registrar boilerplate by hand — the helper auto-registers a template resetter on first use. See [docs/v8-bindings.md](docs/v8-bindings.md).

### V8 weak callbacks

Store a `v8::Global<v8::Object>* persistent` (or `v8::Global<v8::Object> self`) on the holder. In the finalizer, `Reset()` the persistent before `delete`-ing the holder — V8 aborts if the first-pass weak callback neither resets nor schedules a second pass. Worked example: [docs/v8-bindings.md#v8-weak-callbacks-globaltsetweak](docs/v8-bindings.md#v8-weak-callbacks-globaltsetweak).

### Bindings discipline

- One file per class/namespace in `src/js/bind_*.cpp`. GC finalizers free heap-owned C++ objects.
- Every new JS API **MUST** be mirrored in `types/fxe.d.ts` (and `types/fxe-ui.d.ts` for UI additions).
- Bindings throw Node-shaped errors `{code, errno, syscall, path}` where applicable.

### Other invariants

- **`fxe_core` deps:** only `glm` / `glfw` / `stb` / `fxe_font`. No heavy deps. Dawn + V8 stay behind `FXE_ENABLE_WGPU` / `FXE_ENABLE_V8`.
- **Public surface:** `include/fxe/*.hpp` only; everything in `src/` is internal.
- **No `globalThis as { … }` casts** — declare in `types/fxe.d.ts` or the `declare global` block of `types/fxe-ui.d.ts` instead. See [docs/code-style.md](docs/code-style.md#no-globalthis-as---casts).
- **JS/TS object-identity caches:** prefer module-scope `Symbol('…')` + `Reflect.get` / `Reflect.set` over `WeakMap` keyed by the carrier. See [docs/code-style.md](docs/code-style.md#jsts-object-identity-caches).
- **All Dawn pipelines** go through `pipeline_cache`; never instantiate `RenderPipeline` directly.
- **Shaders** are WGSL only, embedded via `embed_wgsl()`.
