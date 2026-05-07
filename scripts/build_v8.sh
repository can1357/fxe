#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Build and install V8 for fxe using depot_tools.

Required tools:
  - git, python3, ninja
  - depot_tools on PATH (fetch, gclient, gn)

Usage:
  scripts/build_v8.sh [--work-dir DIR] [--install-prefix DIR] [--target-cpu arm64|x64] [--debug]

Environment overrides:
  DEPOT_TOOLS_DIR  Path to an existing depot_tools checkout; prepended to PATH.

After install:
  export V8_ROOT=/absolute/install/prefix
  cmake -S . -B build -DFXE_ENABLE_V8=ON
USAGE
}

work_dir="${PWD}/.vendor/v8"
install_prefix="${PWD}/.vendor/v8-install"
target_cpu="$(uname -m)"
is_debug=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --work-dir)
      work_dir="$2"
      shift 2
      ;;
    --install-prefix)
      install_prefix="$2"
      shift 2
      ;;
    --target-cpu)
      target_cpu="$2"
      shift 2
      ;;
    --debug)
      is_debug=true
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$target_cpu" in
  arm64 | aarch64) target_cpu="arm64" ;;
  x86_64 | amd64 | x64) target_cpu="x64" ;;
  *)
    echo "unsupported target cpu: $target_cpu" >&2
    exit 2
    ;;
esac

if [[ -n "${DEPOT_TOOLS_DIR:-}" ]]; then
  export PATH="${DEPOT_TOOLS_DIR}:${PATH}"
fi

for tool in git python3 fetch gclient gn ninja; do
  command -v "$tool" >/dev/null || {
    echo "required tool not found on PATH: $tool" >&2
    exit 1
  }
done

mkdir -p "$work_dir"
cd "$work_dir"

if [[ ! -d v8 ]]; then
  fetch v8
else
  cd v8
  gclient sync -D --with_branch_heads --with_tags
  cd ..
fi

cd v8
build_dir="out/fxe-${target_cpu}-$([[ "$is_debug" == true ]] && echo debug || echo release)"

cat >/tmp/fxe-v8-args.gn <<EOF
is_debug = ${is_debug}
target_cpu = "${target_cpu}"
is_component_build = false
v8_monolithic = false
v8_use_external_startup_data = false
v8_enable_i18n_support = true
v8_enable_pointer_compression = true
v8_enable_sandbox = true
treat_warnings_as_errors = false
EOF

gn gen "$build_dir" --args="$(tr '\n' ' ' </tmp/fxe-v8-args.gn)"
ninja -C "$build_dir" v8 v8_libbase v8_libplatform

mkdir -p "$install_prefix/include" "$install_prefix/lib" "$install_prefix/share/v8"
rsync -a --delete include/ "$install_prefix/include/"

shopt -s nullglob
libs=(
  "$build_dir"/obj/libv8*.a
  "$build_dir"/obj/libv8*.dylib
  "$build_dir"/libv8*.a
  "$build_dir"/libv8*.dylib
)
if ((${#libs[@]} == 0)); then
  echo "no V8 libraries were found under $build_dir; inspect the V8 GN targets for this revision" >&2
  exit 1
fi
cp -f "${libs[@]}" "$install_prefix/lib/"

if [[ -f "$build_dir/icudtl.dat" ]]; then
  cp -f "$build_dir/icudtl.dat" "$install_prefix/share/v8/icudtl.dat"
fi

cat >"$install_prefix/fxe-v8-env.sh" <<EOF
export V8_ROOT="$install_prefix"
export V8_DIR="$install_prefix"
EOF

cat <<EOF
V8 installed to: $install_prefix
Use it with:
  source "$install_prefix/fxe-v8-env.sh"
  cmake -S . -B build -DFXE_ENABLE_V8=ON
EOF
