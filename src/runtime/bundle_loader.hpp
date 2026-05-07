#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::runtime {

  // Inspect the host binary at `argv0` for an appended fxe bundle trailer.
  // On success, mounts the bundle as the process-wide virtual filesystem root
  // and returns true. Subsequent calls are no-ops.
  bool mount_bundle_from_argv0(const char* argv0);

  // True iff a bundle is mounted.
  bool bundle_mounted();

  // The archive name designated as the entry point. Empty if none / unmounted.
  std::string bundle_entry();

  // Read a virtual file. Names match those embedded by `fxe-pack`. The lookup
  // also tries a few common normalizations: leading "./" stripped, and (when
  // `name` is absolute) the basename.
  std::optional<std::string> read_virtual(std::string_view name);

  // List archive names.
  std::vector<std::string> list_virtual();

} // namespace fxe::runtime
