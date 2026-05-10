# Runtime / Tooling Preferences

- **Build:** CMake ≥ 3.24, Ninja generator, vcpkg manifest mode. `bun run bootstrap` builds the in-tree vcpkg.
- **C++:** C++20. Compilers: clang/gcc/MSVC matched in CI.
- **TypeScript:** `tsc` (devDep `typescript ^5.9.3`) for type-checking only — `tsconfig.json` targets ES2022, Node16 modules. Runtime transpilation is performed inside V8 and does **not** type-check; always run `bun run typecheck` before relying on TS examples.
- **Bun/Node:** Bun is the package manager and script runner (`bun install`, `bun run X`). The JS toolchain (Biome, tsc, @types/node) lives in `package.json` devDeps. No application runtime depends on Node — the JS runtime is V8 embedded directly. Do not introduce JS runtime deps.
- **V8:** prefer system/distro V8; otherwise vendor with `scripts/build_v8.sh` (or `.ps1`) which uses depot_tools and installs to `.vendor/v8-install/`. Configure with `-DFXE_ENABLE_V8=ON` and export `V8_ROOT` or `V8_DIR`. `--expose_gc` is enabled at host init for the HeapProfiler.
- **Dawn:** supply externally; configure with `-DDawn_DIR=…` or `CMAKE_PREFIX_PATH`. Not auto-fetched.
- **libuv / mbedTLS / nghttp2:** via vcpkg manifest. `FXE_ENABLE_LIBUV` is `ON` by default (required for async fs/net); `FXE_ENABLE_NATIVE_TLS_HTTP2` is `ON` by default and enables the native HTTPS / HTTP/2 transport in `src/runtime/v8/native/*.cpp`.
- **Python:** ≥ 3.10, stdlib only. Do not add runtime dependencies to `clients/python/`.
- **Biome:** JS/TS format + lint over `examples/js`, `tests`, `packages`, `types`. Required in CI (`format-js-check`, `lint-js`).
- **IDE:** `.clangd` points to `build/dev/compile_commands.json`; configure that preset first for IntelliSense.
