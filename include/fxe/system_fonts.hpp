#pragma once

#include <fxe/types.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fxe {
  // A handful of "preferred system font" candidate paths for the current OS,
  // in fall-through order. None of these files are vendored by fxe — we read
  // them at runtime from the OS's own font directories. macOS Apple fonts are
  // *the* target, hence the priority on SFNS / Helvetica / Geneva.
  //
  // Paths that don't exist on the running machine are skipped by
  // try_load_first_available(); if no candidate loads, it emits one bounded
  // stderr diagnostic with each attempted path and failure reason. Order is
  // opinionated:
  //   macOS  : SFNS (San Francisco) → Helvetica → Menlo (mono) → Geneva
  //   Linux  : DejaVu Sans → Liberation Sans → Noto Sans
  //   Windows: Segoe UI → Arial → Tahoma
  [[nodiscard]] std::vector<std::filesystem::path> system_font_paths();

  // Reads the first existing path from `candidates` into a byte buffer. Returns
  // nullopt if none are readable, after emitting one bounded stderr diagnostic.
  // If `loaded_path` is non-null, the chosen path is written there for diagnostics.
  [[nodiscard]] std::optional<std::vector<u8>>
  try_load_first_available(std::span<const std::filesystem::path> candidates,
                           std::filesystem::path* loaded_path = nullptr);

  // Convenience: combine the two above. Returns the bytes of the first
  // available system font, or nullopt if no candidate could be opened.
  [[nodiscard]] std::optional<std::vector<u8>>
  load_default_system_font(std::filesystem::path* loaded_path = nullptr);
} // namespace fxe
