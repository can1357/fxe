set shell := ['bash', '-eu', '-o', 'pipefail', '-c']
set windows-shell := ['powershell.exe', '-NoLogo', '-NoProfile', '-Command']
set positional-arguments
set unstable

preset := 'dev'
ts_paths := 'examples/js tests packages types'

# ANSI styling — rendered only when the consumer interprets the escapes (printf does).
bold := '\033[1m'
green := '\033[32m'
yellow := '\033[33m'
red := '\033[31m'
reset := '\033[0m'

alias b := build
alias t := test
alias f := format
alias r := example
alias l := lint
alias fa := format-all
alias fj := format-js
alias d := doctor
alias w := watch
alias js := ts

[default]
[doc('List available recipes')]
[group('meta')]
list:
    @just --justfile '{{ justfile() }}' --list --unsorted

[doc('Diagnose required and optional dev tools (see scripts/doctor.sh)')]
[group('meta')]
doctor:
    @bash scripts/doctor.sh

[doc('Show resolved versions of common tools')]
[group('meta')]
versions:
    @printf '{{ bold }}fxe toolchain{{ reset }}\n'
    @printf '  uname     : %s\n' "$(uname -srm)"
    @printf '  cmake     : %s\n' "$(cmake --version 2>/dev/null | head -n1 || echo missing)"
    @printf '  ninja     : %s\n' "$(ninja --version 2>/dev/null || echo missing)"
    @printf '  clang-fmt : %s\n' "$(clang-format --version 2>/dev/null | head -n1 || echo missing)"
    @printf '  tsc       : %s\n' "$(tsc --version 2>/dev/null || echo missing)"
    @printf '  biome     : %s\n' "$(biome --version 2>/dev/null | head -n1 || echo missing)"
    @printf '  ruff      : %s\n' "$(ruff --version 2>/dev/null || echo missing)"
    @printf '  shfmt     : %s\n' "$(shfmt --version 2>/dev/null || echo missing)"
    @printf '  shellcheck: %s\n' "$(shellcheck --version 2>/dev/null | sed -n 's/^version: //p' || echo missing)"
    @printf '  gersemi   : %s\n' "$(gersemi --version 2>/dev/null | head -n1 || echo missing)"
    @printf '  typos     : %s\n' "$(typos --version 2>/dev/null || echo missing)"
    @printf '  python    : %s\n' "$(python3 --version 2>/dev/null || echo missing)"

[doc('Install/refresh dev tooling (npm devDeps + system suggestions)')]
[group('meta')]
tools-install:
    npm install
    @echo
    @echo "System tools (install via your package manager if missing):"
    @echo "  required: cmake ninja clang-format python3 tsc"
    @echo "  optional: biome ruff shfmt shellcheck gersemi typos watchexec tint"
    @echo
    @echo "Run 'just doctor' to see what's currently detected."

# Emit the C++/C source files that should be subject to clang-format,
# excluding vendor/build trees. NUL-terminated to handle exotic paths.
[private]
_cpp_files:
    @git ls-files -z --cached --others --exclude-standard \
        '*.cpp' '*.hpp' '*.cc' '*.cxx' '*.h' '*.hh' '*.hxx' \
        | tr '\0' '\n' \
        | grep -Ev '^(third_party|vendor|vcpkg|build|build-[^/]*|node_modules)/' \
        | tr '\n' '\0'

[doc('Format C++/C headers and sources with the repo clang-format style')]
[group('format')]
format:
    @just --justfile '{{ justfile() }}' _cpp_files | xargs -0r clang-format -i

[doc('Check C++/C formatting without modifying files')]
[group('format')]
format-check:
    @just --justfile '{{ justfile() }}' _cpp_files | xargs -0r clang-format --dry-run --Werror

[doc('Format JS/TS via biome (writes)')]
[group('format')]
format-js *ARGS:
    biome format --write {{ ts_paths }} {{ ARGS }}

[doc('Check JS/TS formatting (no writes); CI gate')]
[group('format')]
format-js-check *ARGS:
    biome format {{ ts_paths }} {{ ARGS }}

[doc('Format Python via ruff (soft-skips when ruff is missing)')]
[group('format')]
format-py *ARGS:
    @if command -v ruff >/dev/null 2>&1; then \
        ruff format clients/python {{ ARGS }}; \
    else \
        printf '{{ yellow }}skip:{{ reset }} ruff not found (see `just doctor`)\n' >&2; \
    fi

[doc('Format shell scripts via shfmt (scripts/ only; soft-skips when missing)')]
[group('format')]
format-shell:
    @if command -v shfmt >/dev/null 2>&1; then \
        shfmt -w -i 2 -ci -bn scripts; \
    else \
        printf '{{ yellow }}skip:{{ reset }} shfmt not found (see `just doctor`)\n' >&2; \
    fi

[doc('Check shell-script formatting (no writes; soft-skips when missing)')]
[group('format')]
format-shell-check:
    @if command -v shfmt >/dev/null 2>&1; then \
        shfmt -d -i 2 -ci -bn scripts; \
    else \
        printf '{{ yellow }}skip:{{ reset }} shfmt not found (see `just doctor`)\n' >&2; \
    fi

[doc('Format CMakeLists.txt and cmake/*.cmake via gersemi (soft-skips when missing)')]
[group('format')]
format-cmake:
    @if command -v gersemi >/dev/null 2>&1; then \
        gersemi -i CMakeLists.txt cmake/*.cmake; \
    else \
        printf '{{ yellow }}skip:{{ reset }} gersemi not found (see `just doctor`)\n' >&2; \
    fi

[doc('Check CMake formatting (no writes; soft-skips when missing)')]
[group('format')]
format-cmake-check:
    @if command -v gersemi >/dev/null 2>&1; then \
        gersemi --check CMakeLists.txt cmake/*.cmake; \
    else \
        printf '{{ yellow }}skip:{{ reset }} gersemi not found (see `just doctor`)\n' >&2; \
    fi

[doc('Format the justfile itself')]
[group('format')]
format-just:
    just --unstable --justfile '{{ justfile() }}' --fmt

[doc('Check justfile formatting')]
[group('format')]
format-just-check:
    just --unstable --justfile '{{ justfile() }}' --fmt --check

[doc('Apply typos auto-fixes (soft-skips when typos is missing)')]
[group('format')]
spell-fix:
    @if command -v typos >/dev/null 2>&1; then \
        typos --write-changes; \
    else \
        printf '{{ yellow }}skip:{{ reset }} typos not found (see `just doctor`)\n' >&2; \
    fi

[doc('Run every formatter (writes); skips optional tools that are missing')]
[group('format')]
format-all: format format-js format-py format-shell format-cmake format-just spell-fix

[doc('Lint JS/TS via biome; CI gate')]
[group('lint')]
[no-exit-message]
lint-js *ARGS:
    biome lint {{ ts_paths }} {{ ARGS }}

[doc('Biome check (lint+format) with --write; one-shot fixer')]
[group('lint')]
fix-js *ARGS:
    biome check --write {{ ts_paths }} {{ ARGS }}

[doc('Lint Python via ruff (soft-skips when ruff is missing)')]
[group('lint')]
[no-exit-message]
lint-py *ARGS:
    @if command -v ruff >/dev/null 2>&1; then \
        ruff check clients/python {{ ARGS }}; \
    else \
        printf '{{ yellow }}skip:{{ reset }} ruff not found (see `just doctor`)\n' >&2; \
    fi

[doc('Shellcheck the standalone scripts (soft-skips when missing)')]
[group('lint')]
[no-exit-message]
lint-shell:
    @if command -v shellcheck >/dev/null 2>&1; then \
        shellcheck scripts/*.sh; \
    else \
        printf '{{ yellow }}skip:{{ reset }} shellcheck not found (see `just doctor`)\n' >&2; \
    fi

[doc('Validate WGSL shaders with tint (set FXE_WGSL_VALIDATOR or have tint on PATH)')]
[group('lint')]
lint-shaders:
    @bin="${FXE_WGSL_VALIDATOR:-$(command -v tint || true)}"; \
        if [ -z "$bin" ]; then \
            printf '{{ yellow }}skip:{{ reset }} tint not found; set FXE_WGSL_VALIDATOR or install dawn tint\n' >&2; \
            exit 0; \
        fi; \
        shopt -s nullglob; \
        for f in src/wgpu/shaders/*.wgsl; do "$bin" --validate "$f"; done

[doc('Spell-check sources with typos (soft-skips when missing)')]
[group('lint')]
[no-exit-message]
spell *ARGS:
    @if command -v typos >/dev/null 2>&1; then \
        typos {{ ARGS }}; \
    else \
        printf '{{ yellow }}skip:{{ reset }} typos not found (see `just doctor`)\n' >&2; \
    fi

[doc('Run every linter (read-only); soft-skips optional tools')]
[group('lint')]
lint: lint-js lint-py lint-shell lint-shaders spell

[doc('Bootstrap the in-tree vcpkg checkout (idempotent)')]
[group('build')]
bootstrap:
    @if [ ! -x ./vcpkg/vcpkg ]; then \
        if [ ! -d ./vcpkg/.git ]; then git clone --depth=1 https://github.com/microsoft/vcpkg.git vcpkg; fi; \
        ./vcpkg/bootstrap-vcpkg.sh -disableMetrics; \
    fi

[doc('Configure a CMake preset (default: dev). Presets: dev, release, dev-wgpu, dev-v8')]
[group('build')]
configure preset=preset: bootstrap
    cmake --preset '{{ preset }}'

[doc('Build a CMake preset (default: dev)')]
[group('build')]
build preset=preset *ARGS: (configure preset)
    cmake --build --preset '{{ preset }}' {{ ARGS }}

[doc('Wipe a preset build dir')]
[group('build')]
clean preset=preset:
    rm -rf 'build/{{ preset }}'

[confirm('Delete every build/ and build-*/ directory. Continue?')]
[doc('Wipe ALL build directories (build/, build-*/)')]
[group('build')]
clean-all:
    rm -rf build build-*

[doc('Run the CTest suite for a preset (default: dev)')]
[group('test')]
test preset=preset *ARGS: (build preset)
    ctest --preset '{{ preset }}' {{ ARGS }}

[doc('Run the core unit-test executable directly')]
[group('test')]
test-core preset=preset: (build preset)
    './build/{{ preset }}/fxe_core_tests'

[doc('Run the Python SDK unit tests (zero deps, stdlib only)')]
[group('test')]
pytest:
    python3 -m unittest discover -s clients/python/tests -v

[doc('List launchable native examples')]
[group('examples')]
examples:
    @printf '%s\n' hello_triangle hello_sprite primitives_showcase

[doc('Build and launch a native example: just example hello_sprite')]
[group('examples')]
example name='hello_triangle' preset=preset: (build preset)
    './build/{{ preset }}/{{ name }}'

[doc('Type-check TS examples')]
[group('examples')]
ts-check:
    tsc --noEmit -p tsconfig.json

[doc('Run a JS/TS example through fxe_run with Dawn/WebGPU. Extra args go through to fxe_run BEFORE the script path: just ts ui_demo --debug')]
[group('examples')]
ts name='ui_demo' *ARGS: (build 'dev-v8-wgpu')
    @script=$(just --justfile '{{ justfile() }}' _resolve_js_script '{{ name }}'); \
        echo "./build/dev-v8-wgpu/fxe_run {{ ARGS }} $script"; \
        './build/dev-v8-wgpu/fxe_run' {{ ARGS }} "$script"

# Resolve a JS/TS example name to a real script path under examples/js/.
# Accepts: bare names (probe known extensions), names with extensions,
# or explicit paths (used as-is when the file exists).
[private]
_resolve_js_script name:
    @set -eu; \
        name='{{ name }}'; \
        if [ -f "$name" ]; then echo "$name"; exit 0; fi; \
        base="examples/js/$name"; \
        if [ -f "$base" ]; then echo "$base"; exit 0; fi; \
        for ext in ts tsx mts cts mjs jsx js; do \
            if [ -f "$base.$ext" ]; then echo "$base.$ext"; exit 0; fi; \
        done; \
        echo "fxe: no script found for '$name' (tried $name, $base, $base.{ts,tsx,mts,cts,mjs,jsx,js})" >&2; \
        exit 1

[doc('Launch fxe_run with the debug protocol enabled (paused) on PORT. Inspect via the TS client.')]
[group('debug')]
debug name='ui_demo' port='9333' *ARGS: (build 'dev-v8-wgpu')
    @script=$(just --justfile '{{ justfile() }}' _resolve_js_script '{{ name }}'); \
        echo "./build/dev-v8-wgpu/fxe_run --debug={{ port }} --debug-pause {{ ARGS }} $script"; \
        './build/dev-v8-wgpu/fxe_run' '--debug={{ port }}' --debug-pause {{ ARGS }} "$script"

[doc('Run the Python debug CLI from clients/python/')]
[group('debug')]
pycli *ARGS:
    cd clients/python && python3 -m fxe_debug.cli {{ ARGS }}

[doc('Rebuild + run a TS example on file change. Requires watchexec or fswatch.')]
[group('dev')]
watch name='ui_demo' *ARGS:
    @if command -v watchexec >/dev/null 2>&1; then \
        exec watchexec --restart --exts ts,tsx,cpp,hpp,wgsl,h -- just ts '{{ name }}' {{ ARGS }}; \
    elif command -v fswatch >/dev/null 2>&1; then \
        while true; do \
            just ts '{{ name }}' {{ ARGS }} || true; \
            fswatch -1 -r src include examples/js tests >/dev/null; \
        done; \
    else \
        echo "install watchexec (brew install watchexec) or fswatch" >&2; exit 1; \
    fi

[doc('One-shot dev loop: format JS, type-check, run example')]
[group('dev')]
dev name='ui_demo' *ARGS: format-js ts-check (ts name ARGS)

# Aggregator for the cheap pre-build checks. Sequential: just 1.50.0 has no
# native parallel-dep attribute and these complete in well under a second total.
[group('ci')]
[private]
_ci-checks: format-check format-js-check format-cmake-check ts-check lint-js

[doc('Local smoke check: format, lint, build, tests, TS types')]
[group('ci')]
ci: _ci-checks test
    @printf '{{ green }}{{ bold }}✓ CI passed{{ reset }}\n'

[doc('Fast local check: format-check + format-js-check + ts-check + lint-js (no build)')]
[group('ci')]
ci-quick: format-check format-js-check ts-check lint-js
    @printf '{{ green }}✓ ci-quick passed{{ reset }}\n'
