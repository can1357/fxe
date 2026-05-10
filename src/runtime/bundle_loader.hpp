#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fxa_archive.hpp"
#include <fxe/types.hpp>
namespace fxe::runtime {

  // Inspect the host binary at `argv0` for an appended FXA archive trailer.
  // On success, mounts the bundle as the process-wide virtual filesystem root
  // and returns true. Subsequent calls are no-ops.
  bool mount_bundle_from_argv0(const char* argv0);

  // True iff a bundle is mounted.
  bool bundle_mounted();

  // True iff the mounted bundle carried a verified archive signature.
  bool bundle_signature_verified();

  // The archive name designated as the entry point. Empty if none / unmounted.
  std::string bundle_entry();
  struct bundle_font_entry {
    std::string virtual_name;
    std::span<const u8> bytes_view{};
  };

  // Read a virtual file. Names match those embedded by `fxe-pack`. The lookup
  // also tries a few common normalizations: leading "./" stripped, and (when
  // `name` is absolute) the basename.
  std::optional<std::string> read_virtual(std::string_view name);

  // List archive names.
  std::vector<std::string> list_virtual();
  std::vector<fxe::runtime::fxa_archive::BundledFont> bundle_fonts();

  std::optional<bundle_font_entry> resolve_bundled_font(std::string_view family, u32 weight = 400,
                                                        std::string_view style = "normal");

} // namespace fxe::runtime
