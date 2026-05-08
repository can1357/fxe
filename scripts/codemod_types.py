#!/usr/bin/env python3
"""Codemod: migrate fixed-width int + size aliases to <fxe/types.hpp> shorthands.

Replacements (longest first; order matters):
  std::uint8_t / uint8_t   -> u8
  std::uint16_t / uint16_t -> u16
  std::uint32_t / uint32_t -> u32
  std::uint64_t / uint64_t -> u64
  std::int8_t  / int8_t    -> i8
  std::int16_t / int16_t   -> i16
  std::int32_t / int32_t   -> i32
  std::int64_t / int64_t   -> i64
  std::size_t   / size_t   -> usize
  std::ptrdiff_t / ptrdiff_t -> isize

Deliberately NOT replaced (too risky for a blind codemod):
  float / double            -- collide with WGSL string bodies, V8/miniaudio
                               APIs, and float-suffixed literals. Migrate
                               those by hand in code paths you control.

Files touched: src/, include/, tests/, examples/, tools/ (.cpp/.cc/.cxx/
.hpp/.h/.hh/.mm/.ipp). Skips include/fxe/types.hpp, build*/, vendor/,
third_party/, .vendor/.

Adds `#include <fxe/types.hpp>` to any file that ends up referencing the
new aliases and didn't already include it. Insertion point: after the last
contiguous `#include` line at the top of the file (skipping `#pragma once`,
license/comment header). Falls back to right after `#pragma once` if the
file has no other includes.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCAN_DIRS = ["src", "include", "tests", "examples", "tools"]
SKIP_REL = {
    Path("include/fxe/types.hpp"),
}
SKIP_DIR_PARTS = {"build", "build-debug", "build-release", "build-dev",
                  "vendor", "third_party", ".vendor", "out"}
EXTS = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".hh", ".mm", ".ipp"}

# Order matters: longest token first, std:: before bare so the std::-stripping
# pass is unambiguous. We use \b on both ends; for the std:: forms we additionally
# require the std:: prefix so we don't double-rewrite.
REPLACEMENTS: list[tuple[re.Pattern[str], str]] = [
    # std:: forms
    (re.compile(r"\bstd::uint8_t\b"),  "u8"),
    (re.compile(r"\bstd::uint16_t\b"), "u16"),
    (re.compile(r"\bstd::uint32_t\b"), "u32"),
    (re.compile(r"\bstd::uint64_t\b"), "u64"),
    (re.compile(r"\bstd::int8_t\b"),   "i8"),
    (re.compile(r"\bstd::int16_t\b"),  "i16"),
    (re.compile(r"\bstd::int32_t\b"),  "i32"),
    (re.compile(r"\bstd::int64_t\b"),  "i64"),
    (re.compile(r"\bstd::size_t\b"),    "usize"),
    (re.compile(r"\bstd::ptrdiff_t\b"), "isize"),
    # Bare forms — must NOT be preceded by `::` (would mean some other ns)
    # nor by an identifier char (already excluded by \b semantics, but ::
    # contains no word char so \b alone won't help).
    (re.compile(r"(?<!::)\buint8_t\b"),   "u8"),
    (re.compile(r"(?<!::)\buint16_t\b"),  "u16"),
    (re.compile(r"(?<!::)\buint32_t\b"),  "u32"),
    (re.compile(r"(?<!::)\buint64_t\b"),  "u64"),
    (re.compile(r"(?<!::)\bint8_t\b"),    "i8"),
    (re.compile(r"(?<!::)\bint16_t\b"),   "i16"),
    (re.compile(r"(?<!::)\bint32_t\b"),   "i32"),
    (re.compile(r"(?<!::)\bint64_t\b"),   "i64"),
    (re.compile(r"(?<!::)\bsize_t\b"),    "usize"),
    (re.compile(r"(?<!::)\bptrdiff_t\b"), "isize"),
]

ALIAS_TOKEN = re.compile(r"\b(?:u8|u16|u32|u64|i8|i16|i32|i64|usize|isize)\b")
INCLUDE_TYPES_RE = re.compile(r'^\s*#\s*include\s*[<"]fxe/types\.hpp[>"]', re.MULTILINE)
INCLUDE_LINE_RE = re.compile(r'^\s*#\s*include\b')
PRAGMA_ONCE_RE = re.compile(r'^\s*#\s*pragma\s+once\b')


def should_skip(path: Path) -> bool:
    rel = path.relative_to(ROOT)
    if rel in SKIP_REL:
        return True
    return any(part in SKIP_DIR_PARTS for part in rel.parts)


def gather_files() -> list[Path]:
    out: list[Path] = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if not p.is_file():
                continue
            if p.suffix not in EXTS:
                continue
            if should_skip(p):
                continue
            out.append(p)
    return out


def apply_replacements(text: str) -> tuple[str, int]:
    total = 0
    for pat, repl in REPLACEMENTS:
        text, n = pat.subn(repl, text)
        total += n
    return text, total


def ensure_include(text: str) -> tuple[str, bool]:
    """Add `#include <fxe/types.hpp>` if the file uses an alias and doesn't
    already include the header. Returns (new_text, inserted)."""
    if not ALIAS_TOKEN.search(text):
        return text, False
    if INCLUDE_TYPES_RE.search(text):
        return text, False

    lines = text.splitlines(keepends=True)
    last_include_idx = -1
    pragma_once_idx = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped:
            continue
        if PRAGMA_ONCE_RE.match(line):
            pragma_once_idx = i
            continue
        if INCLUDE_LINE_RE.match(line):
            last_include_idx = i
            continue
        # Any other preprocessor directive (#if/#ifdef/#define/#endif/...)
        # ends the leading include block — content past it may be
        # conditional and unsafe to land inside.
        if stripped.startswith('#'):
            break
        if stripped.startswith(('//', '/*', '*')):
            continue
        break

    insert_at: int
    if last_include_idx >= 0:
        insert_at = last_include_idx + 1
    elif pragma_once_idx >= 0:
        insert_at = pragma_once_idx + 1
        # Insert blank line first if next line isn't blank.
        if insert_at < len(lines) and lines[insert_at].strip() != "":
            lines.insert(insert_at, "\n")
            insert_at += 1
    else:
        insert_at = 0

    lines.insert(insert_at, "#include <fxe/types.hpp>\n")
    return "".join(lines), True


def process(path: Path, *, dry_run: bool) -> tuple[int, bool]:
    original = path.read_text(encoding="utf-8")
    rewritten, n = apply_replacements(original)
    rewritten, inserted = ensure_include(rewritten)
    if rewritten != original and not dry_run:
        path.write_text(rewritten, encoding="utf-8")
    return n, inserted


def main(argv: list[str]) -> int:
    dry_run = "--dry-run" in argv
    files = gather_files()
    total_subs = 0
    total_files = 0
    total_includes = 0
    for f in files:
        n, inserted = process(f, dry_run=dry_run)
        if n or inserted:
            total_files += 1
            total_subs += n
            total_includes += int(inserted)
            print(f"{f.relative_to(ROOT)}: {n} subs"
                  + (" +#include" if inserted else ""))
    print(f"\n{total_subs} replacements across {total_files} files; "
          f"{total_includes} include insertions"
          + (" (dry run)" if dry_run else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
