#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 <format> <build-dir>" >&2
  exit 2
}

[ "$#" -eq 2 ] || usage

format="$1"
build_dir="$2"
entry="examples/js/hello.ts"
pack_bin="$build_dir/fxe-pack"
app_name="SmokeApp"
manufacturer="FXE Smoke"
out_dir="$(mktemp -d)"
mount_point=''

cleanup() {
  if [ -n "$mount_point" ]; then
    hdiutil detach "$mount_point" >/dev/null 2>&1 || true
  fi
  rm -rf "$out_dir"
}
trap cleanup EXIT

[ -x "$pack_bin" ] || {
  echo "fxe-pack not found or not executable: $pack_bin" >&2
  exit 1
}
[ -f "$entry" ] || {
  echo "entry script not found: $entry" >&2
  exit 1
}

run_pack() {
  "$pack_bin" "$entry" \
    --name "$app_name" \
    --out "$1" \
    --platform "$2" \
    --installer "$3" \
    --version 0.0.1 \
    --manufacturer "$manufacturer" \
    --signing-policy unsigned-dev \
    "${@:4}"
}

case "$format" in
  dmg)
    out="$out_dir/$app_name.dmg"
    run_pack "$out" macos dmg
    mount_point="$(hdiutil attach -nobrowse -readonly "$out" | awk 'END { print $NF }')"
    if [ -z "$mount_point" ]; then
      echo "failed to resolve mounted DMG path" >&2
      exit 1
    fi
    [ -d "$mount_point/$app_name.app" ]
    ;;
  pkg)
    out="$out_dir/$app_name.pkg"
    expand_dir="$out_dir/pkg-expand"
    run_pack "$out" macos pkg
    pkgutil --expand "$out" "$expand_dir"
    [ -f "$expand_dir/Distribution" ] || [ -f "$expand_dir/Distribution.xml" ]
    ;;
  appimage)
    out="$out_dir/$app_name.AppImage"
    extract_dir="$out_dir/appimage-extract"
    run_pack "$out" linux appimage
    chmod +x "$out"
    mkdir -p "$extract_dir"
    (
      cd "$extract_dir"
      "$out" --appimage-extract >/dev/null
    )
    [ -d "$extract_dir/squashfs-root" ]
    if command -v xvfb-run >/dev/null 2>&1; then
      timeout 20s xvfb-run -a "$out" --version >/dev/null 2>&1 || true
    fi
    ;;
  snap)
    if ! command -v snapcraft >/dev/null 2>&1; then
      echo "::warning::snap smoke skipped; snapcraft is unavailable"
      exit 0
    fi
    out="$out_dir/$app_name.snap"
    run_pack "$out" linux snap
    if command -v unsquashfs >/dev/null 2>&1; then
      unsquashfs -l "$out" >/dev/null
    else
      echo "::warning::snap smoke produced $out but unsquashfs is unavailable for archive inspection"
    fi
    ;;
  flatpak)
    if ! command -v flatpak-builder >/dev/null 2>&1 || ! command -v flatpak >/dev/null 2>&1; then
      echo "::warning::flatpak smoke skipped; flatpak-builder/flatpak unavailable"
      exit 0
    fi
    out="$out_dir/$app_name.flatpak"
    run_pack "$out" linux flatpak --flatpak-app-id dev.fxe.smoke
    repo_dir="$out_dir/flatpak-repo"
    mkdir -p "$repo_dir"
    flatpak_info="$(flatpak build-import-bundle --info "$repo_dir" "$out")"
    case "$flatpak_info" in
      *"dev.fxe.smoke"*) ;;
      *)
        echo "flatpak bundle missing expected app id" >&2
        echo "$flatpak_info" >&2
        exit 1
        ;;
    esac
    ;;
  *)
    echo "unknown format: $format" >&2
    exit 2
    ;;
esac

echo "smoke-install $format OK ($out)"
