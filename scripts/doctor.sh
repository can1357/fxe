#!/usr/bin/env bash
# scripts/doctor.sh — probe required + optional dev tools and report status.
#
# Required tools cause a non-zero exit. Optional tools surface as informational.
# Output is a colored table; colors auto-disable when stdout is not a TTY.

set -u
set -o pipefail

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  c_bold=$'\033[1m'
  c_green=$'\033[32m'
  c_yellow=$'\033[33m'
  c_red=$'\033[31m'
  c_dim=$'\033[2m'
  c_reset=$'\033[0m'
else
  c_bold=''
  c_green=''
  c_yellow=''
  c_red=''
  c_dim=''
  c_reset=''
fi

missing_required=0
optional_missing=()

# Print one row of the report.
# Args: status name path-or-version
row() {
  local status="$1" name="$2" detail="$3" color
  case "$status" in
    OK) color="$c_green" ;;
    MISSING) color="$c_red" ;;
    optional) color="$c_yellow" ;;
    *) color="" ;;
  esac
  printf '  %s%-9s%s  %-14s  %s%s%s\n' \
    "$color" "$status" "$c_reset" "$name" "$c_dim" "$detail" "$c_reset"
}

# Probe a tool; first arg is "required" or "optional", second is the tool name,
# remaining args form a version probe (defaults to `--version`).
probe() {
  local kind="$1" name="$2"
  shift 2
  local version_cmd=("$@")
  if [ "${#version_cmd[@]}" -eq 0 ]; then
    version_cmd=("$name" --version)
  fi

  local path
  if path="$(command -v "$name" 2>/dev/null)"; then
    local version
    version="$("${version_cmd[@]}" 2>/dev/null | head -n1 || true)"
    if [ -z "$version" ]; then version="(version probe failed)"; fi
    row OK "$name" "$path  $version"
    return 0
  fi

  if [ "$kind" = "required" ]; then
    row MISSING "$name" "(required)"
    missing_required=$((missing_required + 1))
  else
    row optional "$name" "(optional — see install hints below)"
    optional_missing+=("$name")
  fi
  return 1
}

# Probe one of several alternates (used for watch tools).
# Args: kind label "tool1 tool2 ..."
probe_any() {
  local kind="$1" label="$2"
  shift 2
  local tools=("$@")
  for tool in "${tools[@]}"; do
    if command -v "$tool" >/dev/null 2>&1; then
      local path version
      path="$(command -v "$tool")"
      version="$("$tool" --version 2>/dev/null | head -n1 || true)"
      [ -z "$version" ] && version="(version probe failed)"
      row OK "$label" "$path  $version"
      return 0
    fi
  done
  if [ "$kind" = "required" ]; then
    row MISSING "$label" "(required: one of ${tools[*]})"
    missing_required=$((missing_required + 1))
  else
    row optional "$label" "(optional: one of ${tools[*]})"
    optional_missing+=("$label")
  fi
}

printf '%sfxe doctor%s\n' "$c_bold" "$c_reset"
printf '  uname: %s\n\n' "$(uname -srm)"

printf '%sRequired%s\n' "$c_bold" "$c_reset"
probe required cmake || true
probe required ninja || true
probe required clang-format || true
# tsc may be installed globally or as a npm devDep; either is fine.
if command -v tsc >/dev/null 2>&1; then
  probe required tsc || true
elif command -v npx >/dev/null 2>&1; then
  if npx --no-install tsc --version >/dev/null 2>&1; then
    row OK tsc "(via npx --no-install)  $(npx --no-install tsc --version)"
  else
    row MISSING tsc "(install via 'npm install' or globally)"
    missing_required=$((missing_required + 1))
  fi
else
  row MISSING tsc "(install via 'npm install' or globally)"
  missing_required=$((missing_required + 1))
fi
probe required python3 || true

printf '\n%sOptional%s\n' "$c_bold" "$c_reset"
probe optional biome || true
probe optional ruff || true
probe optional shfmt || true
probe optional shellcheck || true
probe optional gersemi || true
probe optional typos || true
probe_any optional 'watcher' watchexec fswatch entr
# tint may be supplied via FXE_WGSL_VALIDATOR env var.
if [ -n "${FXE_WGSL_VALIDATOR:-}" ] && [ -x "$FXE_WGSL_VALIDATOR" ]; then
  row OK tint "$FXE_WGSL_VALIDATOR  (FXE_WGSL_VALIDATOR)"
elif command -v tint >/dev/null 2>&1; then
  probe optional tint || true
else
  row optional tint "(set FXE_WGSL_VALIDATOR or install dawn tint)"
  optional_missing+=(tint)
fi

if [ ${#optional_missing[@]} -gt 0 ]; then
  printf '\n%sInstall hints (optional)%s\n' "$c_bold" "$c_reset"
  for tool in "${optional_missing[@]}"; do
    case "$tool" in
      biome) printf '  biome      → brew install biome  |  npm install --save-dev @biomejs/biome\n' ;;
      ruff) printf '  ruff       → brew install ruff  |  pipx install ruff\n' ;;
      shfmt) printf '  shfmt      → brew install shfmt\n' ;;
      shellcheck) printf '  shellcheck → brew install shellcheck\n' ;;
      gersemi) printf '  gersemi    → pipx install gersemi  |  pip install gersemi\n' ;;
      typos) printf '  typos      → brew install typos-cli  |  cargo install typos-cli\n' ;;
      watcher) printf '  watcher    → brew install watchexec (preferred) | brew install fswatch\n' ;;
      tint) printf '  tint       → build dawn (https://dawn.googlesource.com/dawn) and set FXE_WGSL_VALIDATOR\n' ;;
    esac
  done
fi

printf '\n'
if [ "$missing_required" -gt 0 ]; then
  printf '%s✗ %d required tool(s) missing%s\n' "$c_red" "$missing_required" "$c_reset" >&2
  exit 1
fi
printf '%s✓ all required tools present%s\n' "$c_green" "$c_reset"
exit 0
