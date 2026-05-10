# Dev Tooling

Beyond the C++ build/test recipes, `package.json` defines a small set of scripts. Run `bun run doctor` to see which tools are present.

## Required

`bun`, `cmake`, `ninja`, `clang-format`, `python3` (`tsc` is installed locally via `bun install`).

## Optional (soft-skip if missing)

- `ruff` — Python format + lint (call directly: `ruff format clients/python`, `ruff check clients/python`).
- `shfmt` / `shellcheck` — shell formatting + linting.
- `gersemi` — CMake formatting.
- `tint` — WGSL validation (set `FXE_WGSL_VALIDATOR` or have it on PATH).
- `watchexec` — `bun run watch <example>` rebuild loop.

## Aggregate scripts

- `bun run fmt` — write all formats.
- `bun run lint` — read-only lint pass.
- `bun run check` — `fmt:check` + `fmt:cpp:check` + `typecheck` + `lint`.
- `bun run ci:quick` — `check` only (no build/test).
- `bun run ci` — `check` + `test`.

The CI workflow runs `ci:quick` on every PR; the matrix build still runs the native test smoke.
