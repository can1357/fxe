#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fxe_pack::cli_detail {

  enum class InstallerFormat { None, Dmg, Pkg, Msi, Msix, AppImage, Snap, Flatpak };

  inline std::string installer_value(InstallerFormat installer) {
    switch (installer) {
    case InstallerFormat::None:
      return "none";
    case InstallerFormat::Dmg:
      return "dmg";
    case InstallerFormat::Pkg:
      return "pkg";
    case InstallerFormat::Msi:
      return "msi";
    case InstallerFormat::Msix:
      return "msix";
    case InstallerFormat::AppImage:
      return "appimage";
    case InstallerFormat::Snap:
      return "snap";
    case InstallerFormat::Flatpak:
      return "flatpak";
    }
    throw std::invalid_argument("unknown installer format");
  }

  inline InstallerFormat parse_installer_value(std::string_view value) {
    if (value == "none")
      return InstallerFormat::None;
    if (value == "dmg")
      return InstallerFormat::Dmg;
    if (value == "pkg")
      return InstallerFormat::Pkg;
    if (value == "msi")
      return InstallerFormat::Msi;
    if (value == "msix")
      return InstallerFormat::Msix;
    if (value == "appimage")
      return InstallerFormat::AppImage;
    if (value == "snap")
      return InstallerFormat::Snap;
    if (value == "flatpak")
      return InstallerFormat::Flatpak;
    throw std::invalid_argument("unknown --installer value: " + std::string(value));
  }

  inline constexpr std::array<std::pair<std::string_view, std::string_view>, 11>
      k_entitlement_shorthands = {{
          {"camera", "com.apple.security.device.camera"},
          {"microphone", "com.apple.security.device.microphone"},
          {"network-client", "com.apple.security.network.client"},
          {"network-server", "com.apple.security.network.server"},
          {"apple-events", "com.apple.security.automation.apple-events"},
          {"jit", "com.apple.security.cs.allow-jit"},
          {"unsigned-memory", "com.apple.security.cs.allow-unsigned-executable-memory"},
          {"dyld-env-vars", "com.apple.security.cs.allow-dyld-environment-variables"},
          {"disable-library-validation", "com.apple.security.cs.disable-library-validation"},
          {"files-user-selected-rw", "com.apple.security.files.user-selected.read-write"},
          {"app-sandbox", "com.apple.security.app-sandbox"},
      }};

  inline std::string map_entitlement_shorthand(std::string_view value) {
    for (const auto& [shorthand, full_key] : k_entitlement_shorthands) {
      if (value == shorthand)
        return std::string(full_key);
    }
    return {};
  }

} // namespace fxe_pack::cli_detail
