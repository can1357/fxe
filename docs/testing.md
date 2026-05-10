# Testing & QA

- **Framework:** CTest with a hand-written assert harness (no gtest / catch2) to keep test binaries small.

## C++ targets

- `fxe_core_tests` — core API, no GPU/V8 (always built)
- `fxe_font_tests`, `fxe_font_render_tests` — font library / face / shaper / collection / discovery; render → atlas roundtrip; ligatures, kerning, color emoji
- `fxe_uv_loop_tests`, `fxe_uv_microtask_flush_tests` — libuv loop and V8 microtask flush
- `fxe_net_http_advanced_tests` — HTTP cookie jar, redirects, headers, timeouts (libcurl)
- `fxe_native_tls_tests`, `fxe_ws_deflate_tests` — mbedTLS client/server, WebSocket per-message-deflate
- `fxe_wgpu_pipeline_cache_tests`, `fxe_wgpu_blur_smoke` — pipeline cache identity, blur post-process
- `fxe_os_linux_smoke_tests` — D-Bus / power / crash on non-Apple Unix
- `fxe_debug_tests`, `fxe_debug_cdp_ws_tests` — JSON, base64, dispatch, CDP WebSocket adapter
- Examples are registered as smoke tests with labels `examples;smoke`.

## TypeScript targets

`tests/*_test.ts` and `tests/*_test.tsx` are auto-registered via `fxe_add_v8_ts_test()` and run inside `fxe_run`. `typescript_smoke.ts` + `typescript_modules_smoke.ts` always run first. Coverage spans `bind_*` (every JS binding), `node_compat_*` (every `node:*` adapter), `ui_*` (fxe-ui components / layout / events / focus / scroll / text-wrap / animated / reconciler), `hmr_*`, `worker_threads_*`, messaging, native runtime, perf, I/O, auto-update, clipboard, stdin, window chrome, packager contract.

## Run

- `bun run test` — full preset
- `bun run test:core` — core executable directly
- `bun run test:py` — Python SDK (`unittest discover` under `clients/python/tests`)

## Quality gates

- **Golden tests:** deterministic FNV-1a hashes over command buffers in `core_tests.cpp` (showcase + text/sprite). Pixel goldens under `tests/golden/` are reserved for Dawn-backed frame capture; not yet wired.
- **Type checks:** `bun run typecheck` — required before merging TS-touching changes.
- **Format gate:** `bun run fmt:cpp:check` + `bun run fmt:check` — CI fails on diffs.
- **Local CI parity:** `bun run ci` (= `check` + `test`; `check` is `fmt:check` + `fmt:cpp:check` + `typecheck` + `lint`).
- **Headless Linux:** CI uses `xvfb`; replicate with `xvfb-run -a bun run test` if no display is available.
