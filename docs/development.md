# Development Commands

Driven by `bun run` (CMake under the hood). The `dev` and `release` presets enable the full runtime (V8 + Dawn + node compat + native TLS/HTTP2).

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
