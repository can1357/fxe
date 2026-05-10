// fxe-pack — append a TypeScript entry + assets to a copy of fxe_run to
// produce a shippable application binary or platform package.
//
// Usage:
//   fxe-pack <entry.ts> [--out <path>] [--name <appname>] [--icon <path>]
//            [--platform macos|win|linux] [--include <glob>...]
//            [--signing-policy unsigned-dev|signed-release|signed-and-notarized|verify-only]
//            [--identity <codesign-id>] [--notarize-profile <profile>]
//            [--webauthn-rp-id <rp_id>...] [--webauthn-mode production|developer]
//            [--entitlement <key>...]
//            [--cert <path-or-subject>] [--installer dmg|pkg|msi|msix|appimage|none]
//            [--dmg|--msi|--msix|--appimage DEPRECATED aliases for --installer]
//            [--update-url <url>] [--public-key <key>] [--channel stable|beta|alpha]
//            [--version <semver> REQUIRED for installer output]
//            [--manufacturer <name>|--publisher <name> REQUIRED for installer output]
//            [--compress zstd|none]
//
// Entitlements:
//   --entitlement <key> may be repeated. Accepted keys: camera, microphone,
//   network-client, network-server, apple-events, jit, unsigned-memory,
//   dyld-env-vars, disable-library-validation, files-user-selected-rw, app-sandbox.

// Signing policies:
//   unsigned-dev (default): build without signing; emits a NOTE about unsigned output.
//   signed-release: sign + verify release artifacts; requires --identity on macOS,
//                   --cert on Windows, and a future Linux signing path.
//   signed-and-notarized: macOS only; requires --identity and --notarize-profile,
//                         then signs, verifies, notarizes, and staples.
//   verify-only: skip signing but verify an externally signed macOS or Windows artifact.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bundle.hpp"
#include "cli_detail.hpp"
#include <fxe/types.hpp>

namespace fs = std::filesystem;

namespace {

  using fxe_pack::cli_detail::InstallerFormat;
  enum class Compression { None, Zstd };

  enum class SigningPolicy { UnsignedDev, SignedRelease, SignedAndNotarized, VerifyOnly };
  struct PackageMetadata {
    std::string version;
    std::string manufacturer;
    std::string channel = "stable";
    std::string update_url;
    std::string public_key;
  };

  struct Args {
    std::string entry;
    std::string out;
    std::string name;
    std::string icon;
    std::string platform;
    std::string identity;
    std::string notarize_profile;
    std::vector<std::string> webauthn_rp_ids;
    std::string webauthn_mode;
    std::vector<std::string> entitlements;
    std::string cert;
    SigningPolicy signing_policy = SigningPolicy::UnsignedDev;
    bool signing_policy_explicit = false;
    InstallerFormat installer = InstallerFormat::None;
    bool installer_explicit = false;
    PackageMetadata package;
    Compression compress = Compression::None;
    std::vector<std::string> includes;
  };

  bool produces_installer(const Args& a) {
    return a.installer != InstallerFormat::None;
  }
  [[noreturn]] void die(const std::string& msg) {
    std::cerr << "fxe-pack: " << msg << "\n";
    std::exit(1);
  }

  std::string default_platform() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "win";
#else
    return "linux";
#endif
  }

  bool is_valid_webauthn_rp_id(std::string_view value) {
    if (value.empty() || value.front() == '.' || value.back() == '.')
      return false;
    bool last_was_dot = false;
    for (char c : value) {
      if (c == '.') {
        if (last_was_dot)
          return false;
        last_was_dot = true;
        continue;
      }
      last_was_dot = false;
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
        continue;
      return false;
    }
    return true;
  }

  bool has_suffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::string shell_quote(const fs::path& path) {
    std::string s = path.string();
    std::string out = "'";
    for (char c : s) {
      if (c == '\'')
        out += "'\\''";
      else
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
  }

  [[maybe_unused]] std::string command_quote(const fs::path& path) {
#if defined(_WIN32)
    std::string s = path.string();
    std::string out = "\"";
    for (char c : s) {
      if (c == '"')
        out += "\\\"";
      else
        out.push_back(c);
    }
    out.push_back('"');
    return out;
#else
    return shell_quote(path);
#endif
  }

  std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[21];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
      return {};
    return buf;
  }

  std::string compression_name(Compression c) {
    switch (c) {
    case Compression::None:
      return "none";
    case Compression::Zstd:
      return "zstd";
    }
    return "none";
  }
  [[maybe_unused]] std::string four_part_version(const std::string& version) {
    const usize dot_count = static_cast<usize>(std::count(version.begin(), version.end(), '.'));
    if (dot_count == 2)
      return version + ".0";
    return version;
  }

  bool tool_exists(const std::string& name) {
    if (name.find(fs::path::preferred_separator) != std::string::npos) {
      return fs::exists(name);
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env)
      return false;
    std::string path(path_env);
#if defined(_WIN32)
    constexpr char sep = ';';
    const std::vector<std::string> suffixes = {".exe", ".bat", ".cmd", ""};
#else
    constexpr char sep = ':';
    const std::vector<std::string> suffixes = {""};
#endif
    usize start = 0;
    while (start <= path.size()) {
      usize end = path.find(sep, start);
      if (end == std::string::npos)
        end = path.size();
      fs::path dir = path.substr(start, end - start);
      for (const auto& suffix : suffixes) {
        std::error_code ec;
        fs::path candidate = dir / (name + suffix);
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
          return true;
      }
      if (end == path.size())
        break;
      start = end + 1;
    }
    return false;
  }

  bool command_succeeds(const std::string& command) {
    return std::system(command.c_str()) == 0;
  }

  bool xcrun_tool_exists(const std::string& name) {
#if defined(__APPLE__)
    return command_succeeds("xcrun -f " + name + " >/dev/null 2>&1");
#else
    (void)name;
    return false;
#endif
  }

  bool is_macos_app_output(const Args& a) {
    return a.platform == "macos" && fs::path(a.out).extension() == ".app";
  }

  bool is_macos_dmg_output(const Args& a) {
    return a.platform == "macos" && fs::path(a.out).extension() == ".dmg";
  }
  bool is_macos_pkg_output(const Args& a) {
    return a.platform == "macos" && fs::path(a.out).extension() == ".pkg";
  }

  bool is_windows_msi_output(const Args& a) {
    return a.platform == "win" && fs::path(a.out).extension() == ".msi";
  }

  bool is_windows_msix_output(const Args& a) {
    return a.platform == "win" && fs::path(a.out).extension() == ".msix";
  }
  bool is_linux_appimage_output(const Args& a) {
    return a.platform == "linux" && fs::path(a.out).extension() == ".AppImage";
  }

  std::string signing_policy_value(SigningPolicy policy) {
    switch (policy) {
    case SigningPolicy::UnsignedDev:
      return "unsigned-dev";
    case SigningPolicy::SignedRelease:
      return "signed-release";
    case SigningPolicy::SignedAndNotarized:
      return "signed-and-notarized";
    case SigningPolicy::VerifyOnly:
      return "verify-only";
    }
    die("unknown signing policy");
  }

  SigningPolicy parse_signing_policy_value(std::string_view value) {
    if (value == "unsigned-dev")
      return SigningPolicy::UnsignedDev;
    if (value == "signed-release")
      return SigningPolicy::SignedRelease;
    if (value == "signed-and-notarized")
      return SigningPolicy::SignedAndNotarized;
    if (value == "verify-only")
      return SigningPolicy::VerifyOnly;
    die("unknown --signing-policy value: " + std::string(value) +
        " (expected unsigned-dev, signed-release, signed-and-notarized, or verify-only)");
  }

  bool policy_signs(SigningPolicy policy) {
    return policy == SigningPolicy::SignedRelease || policy == SigningPolicy::SignedAndNotarized;
  }

  [[maybe_unused]] bool has_wix_tooling() {
#if defined(_WIN32)
    return tool_exists("wix") || (tool_exists("candle") && tool_exists("light"));
#else
    return false;
#endif
  }

  [[maybe_unused]] std::string xml_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
      switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        out.push_back(c);
        break;
      }
    }
    return out;
  }
  std::string render_entitlement_keys(const Args& a) {
    std::vector<std::string> rendered_keys;
    std::string out;
    for (const auto& shorthand : a.entitlements) {
      std::string full_key = fxe_pack::cli_detail::map_entitlement_shorthand(shorthand);
      if (full_key.empty())
        die("unknown --entitlement value: " + shorthand +
            " (expected camera, microphone, network-client, network-server, apple-events, jit, "
            "unsigned-memory, dyld-env-vars, disable-library-validation, "
            "files-user-selected-rw, or app-sandbox)");
      if (std::find(rendered_keys.begin(), rendered_keys.end(), full_key) != rendered_keys.end())
        continue;
      rendered_keys.push_back(full_key);
      out += "  <key>" + xml_escape(full_key) + "</key>\n";
      out += "  <true/>\n";
    }
    return out;
  }

  [[maybe_unused]] std::string safe_identifier(std::string_view value) {
    std::string out;
    for (char c : value) {
      unsigned char uc = static_cast<unsigned char>(c);
      out.push_back(std::isalnum(uc) ? static_cast<char>(uc) : '_');
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())))
      out.insert(out.begin(), '_');
    return out;
  }

  [[maybe_unused]] std::string msix_publisher(const Args& a) {
    if (!a.cert.empty() && !fs::exists(a.cert)) {
      if (a.cert.rfind("CN=", 0) == 0)
        return a.cert;
      return "CN=" + a.cert;
    }
    return "CN=" + a.name;
  }

  int run_command(const std::string& command) {
    return std::system(command.c_str());
  }

  // ~3 min total grace; tune by editing schedule.
  void staple_with_retry_or_die(const fs::path& target, const char* label) {
#if defined(__APPLE__)
    constexpr int sleep_schedule[] = {0, 5, 15, 30, 60, 60};
    constexpr int attempt_count =
        static_cast<int>(sizeof(sleep_schedule) / sizeof(sleep_schedule[0]));
    for (int attempt = 0; attempt < attempt_count; ++attempt) {
      if (sleep_schedule[attempt] > 0)
        std::this_thread::sleep_for(std::chrono::seconds(sleep_schedule[attempt]));
      std::cerr << "fxe-pack: stapler attempt " << (attempt + 1) << "/" << attempt_count << " for "
                << label << "\n";
      std::ostringstream staple_cmd;
      staple_cmd << "xcrun stapler staple " << shell_quote(target);
      int rc = run_command(staple_cmd.str());
      if (rc == 0)
        return;
      if (attempt + 1 == attempt_count)
        break;
      std::cerr << "fxe-pack: stapler attempt " << (attempt + 1) << "/" << attempt_count
                << " failed (rc=" << rc << "); sleeping " << sleep_schedule[attempt + 1]
                << "s before retry\n";
    }
    die(std::string(label) +
        " stapling failed after 6 attempts; the notary ticket may not have propagated yet — "
        "re-run after a delay");
#else
    (void)target;
    (void)label;
#endif
  }

  void run_command_or_die(const std::string& command, const std::string& action) {
    int rc = run_command(command);
    if (rc != 0)
      die(action + " failed with exit code " + std::to_string(rc));
  }

  Args parse(int argc, char** argv) {
    Args a;
    a.platform = default_platform();
    for (int i = 1; i < argc; ++i) {
      std::string_view s = argv[i];
      auto need = [&](const char* flag) -> std::string {
        if (i + 1 >= argc)
          die(std::string("flag ") + flag + " requires an argument");
        return argv[++i];
      };
      if (s == "--out")
        a.out = need("--out");
      else if (s == "--update-url")
        a.package.update_url = need("--update-url");
      else if (s == "--public-key")
        a.package.public_key = need("--public-key");
      else if (s == "--channel") {
        a.package.channel = need("--channel");
        if (a.package.channel != "stable" && a.package.channel != "beta" &&
            a.package.channel != "alpha") {
          die("unknown --channel value: " + a.package.channel +
              " (expected stable, beta, or alpha)");
        }
      } else if (s == "--name")
        a.name = need("--name");
      else if (s == "--icon")
        a.icon = need("--icon");
      else if (s == "--platform")
        a.platform = need("--platform");
      else if (s == "--include")
        a.includes.push_back(need("--include"));
      else if (s == "--identity")
        a.identity = need("--identity");
      else if (s == "--notarize-profile")
        a.notarize_profile = need("--notarize-profile");
      else if (s == "--webauthn-rp-id") {
        std::string rp_id = need("--webauthn-rp-id");
        if (!is_valid_webauthn_rp_id(rp_id))
          die("invalid --webauthn-rp-id: " + rp_id);
        a.webauthn_rp_ids.push_back(std::move(rp_id));
      } else if (s == "--webauthn-mode") {
        a.webauthn_mode = need("--webauthn-mode");
        if (a.webauthn_mode != "production" && a.webauthn_mode != "developer") {
          die("unknown --webauthn-mode value: " + a.webauthn_mode +
              " (expected production or developer)");
        }
      } else if (s == "--cert")
        a.cert = need("--cert");
      else if (s == "--signing-policy") {
        const std::string value = need("--signing-policy");
        a.signing_policy = parse_signing_policy_value(value);
        a.signing_policy_explicit = true;
      } else if (s == "--version")
        a.package.version = need("--version");
      else if (s == "--manufacturer" || s == "--publisher")
        a.package.manufacturer = need("--manufacturer/--publisher");
      else if (s == "--installer") {
        const std::string value = need("--installer");
        InstallerFormat installer = InstallerFormat::None;
        try {
          installer = fxe_pack::cli_detail::parse_installer_value(value);
        } catch (const std::invalid_argument&) {
          die("unknown --installer value: " + value +
              " (expected dmg, pkg, msi, msix, appimage, or none)");
        }
        if (a.installer != InstallerFormat::None && a.installer != installer) {
          die("conflicting installer selection: --installer " + value +
              " conflicts with --installer " + fxe_pack::cli_detail::installer_value(a.installer));
        }
        a.installer = installer;
        a.installer_explicit = true;
      } else if (s == "--appimage" || s == "--dmg" || s == "--msi" || s == "--msix") {
        const InstallerFormat installer = s == "--appimage" ? InstallerFormat::AppImage
                                          : s == "--dmg"    ? InstallerFormat::Dmg
                                          : s == "--msi"    ? InstallerFormat::Msi
                                                            : InstallerFormat::Msix;
        if (a.installer != InstallerFormat::None && a.installer != installer) {
          die("conflicting installer selection: " + std::string(s) +
              " conflicts with --installer " + fxe_pack::cli_detail::installer_value(a.installer));
        }
        std::cerr << "fxe-pack: " << s << " is deprecated; use --installer "
                  << fxe_pack::cli_detail::installer_value(installer) << "\n";
        a.installer = installer;
        a.installer_explicit = true;
      } else if (s == "--entitlement") {
        std::string entitlement = need("--entitlement");
        if (fxe_pack::cli_detail::map_entitlement_shorthand(entitlement).empty()) {
          die("unknown --entitlement value: " + entitlement +
              " (expected camera, microphone, network-client, network-server, apple-events, jit, "
              "unsigned-memory, dyld-env-vars, disable-library-validation, "
              "files-user-selected-rw, or app-sandbox)");
        }
        a.entitlements.push_back(std::move(entitlement));
      } else if (s == "--compress") {
        std::string value = need("--compress");
        if (value == "none")
          a.compress = Compression::None;
        else if (value == "zstd")
          a.compress = Compression::Zstd;
        else
          die("unknown --compress value: " + value + " (expected zstd or none)");
      } else if (s == "-h" || s == "--help") {
        std::cout
            << "Usage: fxe-pack <entry.ts> [--out PATH] [--name NAME] [--icon PATH]\n"
            << "                [--platform macos|win|linux] [--include GLOB ...]\n"
            << "                [--signing-policy "
               "unsigned-dev|signed-release|signed-and-notarized|verify-only]\n"
            << "                [--identity CODESIGN_ID] [--notarize-profile PROFILE]\n"
            << "                [--webauthn-rp-id RP_ID ...] [--webauthn-mode "
               "production|developer]\n"
            << "                [--entitlement KEY ...]\n"
            << "                [--cert PATH_OR_SUBJECT] [--installer "
               "dmg|pkg|msi|msix|appimage|none]\n"
            << "                [--dmg|--msi|--msix|--appimage DEPRECATED]\n"
            << "                [--update-url URL] [--public-key KEY] [--channel "
               "stable|beta|alpha]\n"
            << "                [--version SEMVER REQUIRED for installer output]\n"
            << "                [--manufacturer NAME|--publisher NAME REQUIRED for installer "
               "output]\n"
            << "                [--compress zstd|none]\n"
            << "\nExamples:\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.app --platform macos\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.dmg --platform macos\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.pkg --platform macos "
               "--installer "
               "pkg\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.exe --platform win\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.msi --platform win\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.msix --platform win --cert "
               "CN=Publisher\n"
            << "  fxe-pack examples/js/react_demo.ts --out MyApp.AppImage --platform linux "
               "--installer appimage\n"
            << "  fxe-pack examples/js/react_demo.ts --out my-app.tar.gz --platform linux\n"
            << "\nEntitlements:\n"
            << "  --entitlement KEY may be repeated. Accepted keys: camera, microphone,\n"
            << "    network-client, network-server, apple-events, jit, unsigned-memory,\n"
            << "    dyld-env-vars, disable-library-validation, files-user-selected-rw,\n"
            << "    app-sandbox.\n"
            << "\nSigning policies:\n"
            << "  unsigned-dev (default): build without signing; emits a NOTE.\n"
            << "  signed-release: sign + verify release artifacts; requires --identity on "
               "macOS,\n"
            << "    --cert on Windows, and a future Linux signing path.\n"
            << "  signed-and-notarized: macOS only; requires --identity and --notarize-profile,\n"
            << "    then signs, verifies, notarizes, and staples.\n"
            << "  verify-only: skip signing but verify an externally signed macOS or Windows "
               "artifact.\n";
        std::exit(0);
      } else if (!s.empty() && s[0] == '-') {
        die("unknown flag: " + std::string(s));
      } else if (a.entry.empty()) {
        a.entry = std::string(s);
      } else {
        die("unexpected positional: " + std::string(s));
      }
    }
    if (a.entry.empty())
      die("missing <entry.ts>");
    if (a.platform != "macos" && a.platform != "win" && a.platform != "linux")
      die("unknown --platform: " + a.platform);
    if (!a.webauthn_rp_ids.empty() && a.platform != "macos")
      die("--webauthn-rp-id requires --platform macos");
    if (!a.webauthn_mode.empty() && a.platform != "macos")
      die("--webauthn-mode requires --platform macos");
    if (!a.entitlements.empty() && a.platform != "macos")
      die("--entitlement requires --platform macos");
    if (!a.signing_policy_explicit) {
      if (!a.identity.empty() && !a.notarize_profile.empty()) {
        a.signing_policy = SigningPolicy::SignedAndNotarized;
      } else if (!a.identity.empty() || !a.cert.empty()) {
        a.signing_policy = SigningPolicy::SignedRelease;
      }
      if (a.signing_policy != SigningPolicy::UnsignedDev) {
        std::cerr << "fxe-pack: DEPRECATED: inferred signing policy "
                  << signing_policy_value(a.signing_policy)
                  << " from legacy flags; pass --signing-policy explicitly.\n";
      }
    }
    if (a.webauthn_mode.empty() && !a.webauthn_rp_ids.empty()) {
      // Unsigned developer builds keep the associated domain in developer mode;
      // signed release policies default to production entitlements.
      a.webauthn_mode = policy_signs(a.signing_policy) ? "production" : "developer";
    }
    if (a.installer == InstallerFormat::Dmg && a.platform != "macos")
      die("--installer dmg requires --platform macos");
    if (a.installer == InstallerFormat::Pkg && a.platform != "macos")
      die("--installer pkg requires --platform macos");
    if (a.installer == InstallerFormat::Msi && a.platform != "win")
      die("--installer msi requires --platform win");
    if (a.installer == InstallerFormat::Msix && a.platform != "win")
      die("--installer msix requires --platform win");
    if (a.installer == InstallerFormat::AppImage && a.platform != "linux")
      die("--installer appimage requires --platform linux");
    if (a.name.empty())
      a.name = fs::path(a.entry).stem().string();
    if (a.out.empty())
      a.out = a.name;
    if (fs::path(a.out).extension() == ".app" && a.platform != "macos")
      die(".app output requires --platform macos");
    if (fs::path(a.out).extension() == ".dmg" && a.platform != "macos")
      die(".dmg output requires --platform macos");
    if (fs::path(a.out).extension() == ".pkg" && a.platform != "macos")
      die(".pkg output requires --platform macos");
    if (fs::path(a.out).extension() == ".exe" && a.platform != "win")
      die(".exe output requires --platform win");
    if (fs::path(a.out).extension() == ".msi" && a.platform != "win")
      die(".msi output requires --platform win");
    if (fs::path(a.out).extension() == ".msix" && a.platform != "win")
      die(".msix output requires --platform win");
    if (has_suffix(a.out, ".tar.gz") && a.platform != "linux")
      die(".tar.gz output requires --platform linux");
    if (!a.installer_explicit && a.installer == InstallerFormat::None) {
      if (is_macos_dmg_output(a))
        a.installer = InstallerFormat::Dmg;
      else if (is_macos_pkg_output(a))
        a.installer = InstallerFormat::Pkg;
      else if (is_windows_msi_output(a))
        a.installer = InstallerFormat::Msi;
      else if (is_windows_msix_output(a))
        a.installer = InstallerFormat::Msix;
      else if (is_linux_appimage_output(a))
        a.installer = InstallerFormat::AppImage;
    }
    return a;
  }

  void validate_requested_tools(const Args& a) {
    if (!a.identity.empty() && a.signing_policy == SigningPolicy::UnsignedDev) {
      std::cerr << "fxe-pack: NOTE: ignoring --identity under --signing-policy unsigned-dev\n";
    }
    if (!a.notarize_profile.empty() && a.signing_policy == SigningPolicy::UnsignedDev) {
      std::cerr
          << "fxe-pack: NOTE: ignoring --notarize-profile under --signing-policy unsigned-dev\n";
    }
    if (!a.cert.empty() && a.signing_policy == SigningPolicy::UnsignedDev) {
      std::cerr << "fxe-pack: NOTE: ignoring --cert under --signing-policy unsigned-dev\n";
    }
    if (a.signing_policy == SigningPolicy::SignedAndNotarized && a.platform != "macos")
      die("--signing-policy signed-and-notarized is only supported with --platform macos");
    if ((a.signing_policy == SigningPolicy::SignedRelease ||
         a.signing_policy == SigningPolicy::SignedAndNotarized ||
         a.signing_policy == SigningPolicy::VerifyOnly) &&
        a.platform == "macos" && !is_macos_app_output(a) && !is_macos_dmg_output(a) &&
        !is_macos_pkg_output(a) && a.installer != InstallerFormat::Dmg &&
        a.installer != InstallerFormat::Pkg) {
      die("--signing-policy " + signing_policy_value(a.signing_policy) +
          " requires macOS .app, .dmg, or .pkg output (use --out <name>.app, <name>.dmg, "
          "<name>.pkg, or --installer dmg/pkg)");
    }
    if (a.signing_policy == SigningPolicy::SignedRelease) {
      if (a.platform == "macos") {
        if (a.identity.empty())
          die("--signing-policy signed-release requires --identity on macOS");
#if !defined(__APPLE__)
        die("--signing-policy signed-release requires Apple's codesign tool and can only run on "
            "macOS hosts");
#else
        if (!tool_exists("codesign"))
          die("--signing-policy signed-release requires Apple's codesign tool, which was not "
              "found in PATH");
#endif
      } else if (a.platform == "win") {
        if (a.cert.empty())
          die("--signing-policy signed-release requires --cert on Windows");
#if !defined(_WIN32)
        die("--signing-policy signed-release requires Windows signtool and can only run on "
            "Windows hosts");
#else
        if (!tool_exists("signtool"))
          die("--signing-policy signed-release requires signtool, which was not found in PATH");
#endif
      } else if (a.platform == "linux") {
        die("--signing-policy signed-release requires a Linux GPG/minisign signing path, which "
            "is TODO");
      }
    }
    if (a.signing_policy == SigningPolicy::SignedAndNotarized) {
      if (a.identity.empty())
        die("--signing-policy signed-and-notarized requires --identity");
      if (a.notarize_profile.empty())
        die("--signing-policy signed-and-notarized requires --notarize-profile");
#if !defined(__APPLE__)
      die("--signing-policy signed-and-notarized requires macOS codesign/notarytool and can only "
          "run on macOS hosts");
#else
      if (!tool_exists("codesign"))
        die("--signing-policy signed-and-notarized requires Apple's codesign tool, which was not "
            "found in PATH");
      if (!xcrun_tool_exists("notarytool"))
        die("--signing-policy signed-and-notarized requires xcrun notarytool, which was not "
            "found");
      if (!xcrun_tool_exists("stapler"))
        die("--signing-policy signed-and-notarized requires xcrun stapler, which was not found");
#endif
    }
    if (a.signing_policy == SigningPolicy::VerifyOnly) {
      if (a.platform == "macos") {
#if !defined(__APPLE__)
        die("--signing-policy verify-only requires Apple's codesign tool and can only run on "
            "macOS hosts");
#else
        if (!tool_exists("codesign"))
          die("--signing-policy verify-only requires Apple's codesign tool, which was not found "
              "in PATH");
#endif
      } else if (a.platform == "win") {
#if !defined(_WIN32)
        die("--signing-policy verify-only requires Windows signtool and can only run on Windows "
            "hosts");
#else
        if (!tool_exists("signtool"))
          die("--signing-policy verify-only requires signtool, which was not found in PATH");
#endif
      } else if (a.platform == "linux") {
        die("--signing-policy verify-only requires a Linux signature verification path, which is "
            "TODO");
      }
    }
    if (a.installer == InstallerFormat::Dmg || is_macos_dmg_output(a)) {
#if !defined(__APPLE__)
      die(".dmg output requires hdiutil and can only be built on macOS hosts");
#else
      if (!tool_exists("hdiutil"))
        die(".dmg output requires hdiutil, which was not found in PATH");
#endif
    }
    if (a.installer == InstallerFormat::Pkg || is_macos_pkg_output(a)) {
#if !defined(__APPLE__)
      die(".pkg output requires hdiutil-class macOS tooling (pkgbuild/productbuild) and can only "
          "be built on macOS hosts");
#else
      if (!tool_exists("pkgbuild"))
        die(".pkg output requires pkgbuild, which was not found in PATH");
      if (!tool_exists("productbuild"))
        die(".pkg output requires productbuild, which was not found in PATH");
      if (policy_signs(a.signing_policy) && !tool_exists("productsign"))
        die(".pkg signing requires productsign, which was not found in PATH");
      if (a.signing_policy == SigningPolicy::VerifyOnly && !tool_exists("pkgutil"))
        die(".pkg signature verification requires pkgutil, which was not found in PATH");
#endif
    }
    if (a.installer == InstallerFormat::Msi || is_windows_msi_output(a)) {
      if (!tool_exists("candle") || !tool_exists("light"))
        die("WiX not found on PATH (requires candle.exe and light.exe)");
#if !defined(_WIN32)
      if (a.installer != InstallerFormat::Msi)
        die(".msi output requires WiX tooling and can only be built on Windows hosts");
#endif
    }
    if (a.installer == InstallerFormat::Msix || is_windows_msix_output(a)) {
      if (!tool_exists("makeappx.exe") && !tool_exists("makeappx"))
        die("makeappx.exe not found on PATH (Windows SDK MakeAppx is required)");
#if defined(_WIN32)
      if (a.signing_policy == SigningPolicy::SignedRelease && !tool_exists("signtool"))
        die(".msix signing requires Windows SDK signtool, which was not found in PATH");
#else
      if (a.installer != InstallerFormat::Msix)
        die(".msix output requires Windows SDK MakeAppx and can only be built on Windows hosts");
#endif
    }
    if (a.installer == InstallerFormat::AppImage && a.platform != "linux")
      die("--installer appimage requires --platform linux");
    if (fs::path(a.out).extension() == ".AppImage" && a.platform != "linux")
      die("AppImage output requires --platform linux");
    if (a.platform == "linux" &&
        (a.installer == InstallerFormat::AppImage || fs::path(a.out).extension() == ".AppImage") &&
        !tool_exists("appimagetool")) {
      die("AppImage output was requested but appimagetool was not found in PATH; use --out "
          "<name>.tar.gz for fallback packaging or install appimagetool");
    }
    if (a.compress == Compression::Zstd && !tool_exists("zstd")) {
      die("--compress zstd requires the zstd command, which was not found in PATH");
    }
  }

  fs::path locate_fxe_run(const fs::path& self_dir) {
    if (const char* env = std::getenv("FXE_RUN")) {
      fs::path p(env);
      if (fs::exists(p))
        return p;
      die(std::string("FXE_RUN points to missing file: ") + env);
    }
    fs::path sib = self_dir / "fxe_run";
#if defined(_WIN32)
    if (fs::exists(self_dir / "fxe_run.exe"))
      return self_dir / "fxe_run.exe";
#endif
    if (fs::exists(sib))
      return sib;
    die("cannot locate fxe_run; set FXE_RUN env var or place fxe-pack next to it");
  }

  void copy_one(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
  }

  void move_one(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
      return;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    fs::remove(from);
  }

  void make_executable(const fs::path& p) {
#if !defined(_WIN32)
    fs::permissions(p,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
#else
    (void)p;
#endif
  }

  std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
      die("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  }

  void spit(const fs::path& p, std::string_view body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
  }

  std::string load_template_or(const fs::path& tmpl_dir, const std::string& name,
                               std::string_view fallback) {
    fs::path p = tmpl_dir / name;
    if (fs::exists(p))
      return slurp(p);
    return std::string(fallback);
  }

  std::string subst(std::string s, const std::string& key, const std::string& value) {
    std::string needle = "@" + key + "@";
    for (usize pos = 0; (pos = s.find(needle, pos)) != std::string::npos; pos += value.size()) {
      s.replace(pos, needle.size(), value);
    }
    return s;
  }

  fs::path templates_dir(const fs::path& self_dir) {
    // Try sibling install layout first, then in-tree layout.
    for (const fs::path& cand : {
             self_dir / "share" / "fxe" / "fxe-pack" / "templates",
             self_dir / ".." / "share" / "fxe" / "fxe-pack" / "templates",
             self_dir / "templates",
             // CMake source-tree layout: fxe-pack lives at build/<preset>/fxe-pack
             // (FXE_BUILD_PACKAGER puts the executable directly in the preset bin
             // dir), so the template tree is two parents above.
             self_dir / ".." / ".." / "tools" / "fxe-pack" / "templates",
         }) {
      if (fs::exists(cand))
        return fs::weakly_canonical(cand);
    }
    return self_dir / "templates"; // best effort
  }

  struct Files {
    // disk path -> archive name
    std::vector<std::pair<std::string, std::string>> v;
    std::string entry_archive;
  };

  Files collect(const Args& a) {
    Files out;
    fs::path entry = fs::absolute(a.entry);
    if (!fs::exists(entry))
      die("entry not found: " + a.entry);
    fs::path root = entry.parent_path();
    auto rel = [&](const fs::path& p) {
      std::error_code ec;
      auto r = fs::relative(fs::absolute(p), root, ec);
      if (ec || r.empty() || *r.begin() == "..")
        return p.filename().string();
      std::string s = r.generic_string();
      return s;
    };

    out.v.emplace_back(entry.string(), rel(entry));
    out.entry_archive = rel(entry);
    // Also expose as the conventional entry name so the loader finds it.
    out.v.emplace_back(entry.string(), std::string("__entry__"));

    for (const auto& inc : a.includes) {
      fs::path p(inc);
      if (fs::exists(p) && fs::is_regular_file(p)) {
        out.v.emplace_back(p.string(), rel(p));
        continue;
      }
      // Glob: simple shell-style under root.
      fs::path base = p.is_absolute() ? p.parent_path() : root;
      std::string pattern = p.filename().string();
      if (!fs::exists(base))
        continue;
      for (auto& e : fs::recursive_directory_iterator(base)) {
        if (!e.is_regular_file())
          continue;
        const auto& name = e.path().filename().string();
        // very small glob: '*' wildcard
        if (pattern == "*" || pattern == name) {
          out.v.emplace_back(e.path().string(), rel(e.path()));
        } else if (pattern.size() > 2 && pattern.front() == '*') {
          std::string suf = pattern.substr(1);
          if (name.size() >= suf.size() &&
              name.compare(name.size() - suf.size(), suf.size(), suf) == 0)
            out.v.emplace_back(e.path().string(), rel(e.path()));
        }
      }
    }
    // De-duplicate by archive name (keep first).
    std::vector<std::pair<std::string, std::string>> dedup;
    for (auto& p : out.v) {
      bool seen = false;
      for (auto& q : dedup)
        if (q.second == p.second) {
          seen = true;
          break;
        }
      if (!seen)
        dedup.push_back(p);
    }
    out.v = std::move(dedup);

    // The "__entry__" sentinel must hold the *archive name* of the entry,
    // not the entry itself. Rewrite that pair to write a tiny indirection
    // file: contents = archive name.
    for (auto& [disk, arch] : out.v) {
      if (arch == "__entry__") {
        // Generate a temp file with the entry's archive name as content.
        fs::path tmp = fs::temp_directory_path() / "fxe-pack-entry.txt";
        std::ofstream f(tmp, std::ios::binary);
        f.write(out.entry_archive.data(), static_cast<std::streamsize>(out.entry_archive.size()));
        f.close();
        disk = tmp.string();
      }
    }
    return out;
  }

  void copy_runtime_sidecars(const fs::path& fxe_run_dir, const fs::path& dest) {
    fs::path icudtl = fxe_run_dir / "icudtl.dat";
    if (fs::exists(icudtl))
      copy_one(icudtl, dest / "icudtl.dat");
  }

  fs::path wrap_macos_app(const Args& a, const fs::path& staged_bin, const fs::path& app,
                          const fs::path& tmpl_dir, const fs::path& fxe_run_dir) {
    if (fs::exists(app))
      fs::remove_all(app);
    fs::path macos = app / "Contents" / "MacOS";
    fs::path res = app / "Contents" / "Resources";
    fs::create_directories(macos);
    fs::create_directories(res);
    fs::path final_bin = macos / a.name;
    move_one(staged_bin, final_bin);
    make_executable(final_bin);

    std::string plist =
        subst(load_template_or(tmpl_dir, "Info.plist.in", ""), "FXE_APP_NAME", a.name);
    plist = subst(plist, "FXE_APP_BUNDLE_ID", "com.fxe." + a.name);
    plist = subst(plist, "FXE_APP_VERSION", a.package.version);
    spit(app / "Contents" / "Info.plist", plist);

    if (!a.webauthn_rp_ids.empty() || !a.entitlements.empty()) {
      std::string domains;
      for (const auto& rp_id : a.webauthn_rp_ids) {
        domains += "    <string>webcredentials:" + xml_escape(rp_id);
        if (a.webauthn_mode == "developer")
          domains += "?mode=developer";
        domains += "</string>\n";
      }
      std::string ent = subst(load_template_or(tmpl_dir, "entitlements.plist.in", ""),
                              "FXE_WEBAUTHN_ASSOCIATED_DOMAINS", domains);
      ent = subst(ent, "FXE_ENTITLEMENT_KEYS", render_entitlement_keys(a));
      spit(app / "Contents" / "entitlements.plist", ent);
    }

    if (!a.icon.empty())
      copy_one(a.icon, res / "AppIcon.icns");
    copy_runtime_sidecars(fxe_run_dir, macos);
    return app;
  }

  fs::path wrap_win_exe(const Args& a, const fs::path& staged_bin, fs::path out,
                        const fs::path& fxe_run_dir) {
    (void)a;
    if (out.extension() != ".exe")
      out += ".exe";
    move_one(staged_bin, out);
    copy_runtime_sidecars(fxe_run_dir, out.parent_path());
    return out;
  }

  fs::path wrap_linux_appdir(const Args& a, const fs::path& staged_bin, const fs::path& appdir,
                             const fs::path& tmpl_dir, const fs::path& fxe_run_dir) {
    if (fs::exists(appdir))
      fs::remove_all(appdir);
    fs::path bin = appdir / "usr" / "bin";
    fs::create_directories(bin);
    fs::path final_bin = bin / a.name;
    move_one(staged_bin, final_bin);
    make_executable(final_bin);

    std::string apprun = subst(load_template_or(tmpl_dir, "AppRun.in", ""), "FXE_APP_NAME", a.name);
    spit(appdir / "AppRun", apprun);
    make_executable(appdir / "AppRun");

    std::string desktop = "[Desktop Entry]\nType=Application\nName=" + a.name + "\nExec=" + a.name +
                          "\nIcon=" + a.name + "\nCategories=Utility;\n";
    spit(appdir / (a.name + ".desktop"), desktop);

    if (!a.icon.empty())
      copy_one(a.icon, appdir / (a.name + ".png"));
    copy_runtime_sidecars(fxe_run_dir, bin);
    return appdir;
  }

  fs::path wrap_linux_tar_gz(const Args& a, const fs::path& staged_bin, const fs::path& out,
                             const fs::path& tmpl_dir, const fs::path& fxe_run_dir) {
    if (!tool_exists("tar"))
      die(".tar.gz output requires tar, which was not found in PATH");
    fs::path appdir = fs::temp_directory_path() / ("fxe-pack-" + a.name + ".AppDir");
    wrap_linux_appdir(a, staged_bin, appdir, tmpl_dir, fxe_run_dir);
    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream cmd;
    cmd << "tar -czf " << shell_quote(out) << " -C " << shell_quote(appdir.parent_path()) << " "
        << shell_quote(appdir.filename());
    run_command_or_die(cmd.str(), "tar.gz packaging");
    fs::remove_all(appdir);
    return out;
  }

  fs::path wrap_linux_appimage(const Args& a, const fs::path& staged_bin, const fs::path& out,
                               const fs::path& tmpl_dir, const fs::path& fxe_run_dir) {
    if (!tool_exists("appimagetool")) {
      die("AppImage output was requested but appimagetool was not found in PATH; use --out "
          "<name>.tar.gz for fallback packaging or install appimagetool");
    }
    fs::path appdir = fs::temp_directory_path() / ("fxe-pack-" + a.name + ".AppDir");
    wrap_linux_appdir(a, staged_bin, appdir, tmpl_dir, fxe_run_dir);
    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream cmd;
    cmd << "appimagetool " << shell_quote(appdir) << " " << shell_quote(out);
    run_command_or_die(cmd.str(), "AppImage packaging");
    fs::remove_all(appdir);
    return out;
  }

  fs::path write_plain_output(const fs::path& staged_bin, const fs::path& out) {
    move_one(staged_bin, out);
    make_executable(out);
    return out;
  }

  void maybe_zstd_compress(const Args& a, const fs::path& out) {
    if (a.compress != Compression::Zstd)
      return;
    if (fs::is_directory(out))
      die("--compress zstd cannot compress directory output; choose file output or --compress "
          "none");
    fs::path zst = out;
    zst += ".zst";
    std::ostringstream cmd;
    cmd << "zstd -f -q " << shell_quote(out) << " -o " << shell_quote(zst);
    run_command_or_die(cmd.str(), "zstd compression");
    std::cout << "fxe-pack: compressed " << zst.string() << "\n";
  }

  void verify_codesign_or_die(const fs::path& signed_target, const char* label);
  void verify_pkg_signature_or_die(const fs::path& signed_target, const char* label);

  void verify_signtool_or_die(const fs::path& signed_target, const char* label);
  void sign_macos_app_or_die(const Args& a, const fs::path& app) {
    if (a.signing_policy == SigningPolicy::UnsignedDev)
      return;
    if (a.signing_policy == SigningPolicy::VerifyOnly) {
      verify_codesign_or_die(app, "app bundle");
      return;
    }
    std::ostringstream cmd;
    cmd << "codesign --force --deep --timestamp --options runtime --sign "
        << shell_quote(fs::path(a.identity));
    fs::path entitlements = app / "Contents" / "entitlements.plist";
    if (fs::exists(entitlements))
      cmd << " --entitlements " << shell_quote(entitlements);
    cmd << " " << shell_quote(app);
    run_command_or_die(cmd.str(), "codesign");
    verify_codesign_or_die(app, "app bundle");
  }

  void verify_codesign_or_die(const fs::path& signed_target, const char* label) {
#if defined(__APPLE__)
    std::ostringstream cmd;
    cmd << "codesign --verify --deep --strict --verbose=2 " << shell_quote(signed_target);
    run_command_or_die(cmd.str(), std::string("codesign verify (") + label + ")");
#else
    (void)signed_target;
    (void)label;
#endif
  }

  void notarize_macos_app_or_die(const Args& a, const fs::path& app) {
    if (a.signing_policy != SigningPolicy::SignedAndNotarized)
      return;
#if defined(__APPLE__)
    fs::path zip = fs::temp_directory_path() / (app.filename().string() + ".notarize.zip");
    fs::remove(zip);

    std::ostringstream zip_cmd;
    zip_cmd << "ditto -c -k --keepParent " << shell_quote(app.filename()) << " "
            << shell_quote(zip);
    run_command_or_die("cd " + shell_quote(app.parent_path()) + " && " + zip_cmd.str(),
                       "notarization zip creation");

    std::ostringstream submit_cmd;
    submit_cmd << "xcrun notarytool submit " << shell_quote(zip) << " --keychain-profile "
               << shell_quote(fs::path(a.notarize_profile)) << " --wait";
    run_command_or_die(submit_cmd.str(), "notarization");
    fs::remove(zip);

    staple_with_retry_or_die(app, "notarization");
#else
    (void)a;
    (void)app;
#endif
  }

  void sign_windows_exe_or_die(const Args& a, const fs::path& exe) {
    if (a.signing_policy == SigningPolicy::UnsignedDev)
      return;
    if (a.signing_policy == SigningPolicy::VerifyOnly) {
      verify_signtool_or_die(exe, "exe");
      return;
    }
#if defined(_WIN32)
    std::ostringstream cmd;
    cmd << "signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ";
    if (fs::exists(a.cert))
      cmd << "/f " << command_quote(fs::path(a.cert)) << " ";
    else
      cmd << "/n " << command_quote(fs::path(a.cert)) << " ";
    cmd << command_quote(exe);
    run_command_or_die(cmd.str(), "signtool signing");
    verify_signtool_or_die(exe, "exe");
#else
    (void)a;
    (void)exe;
#endif
  }

  void verify_signtool_or_die(const fs::path& signed_target, const char* label) {
#if defined(_WIN32)
    std::ostringstream cmd;
    cmd << "signtool verify /pa /all " << command_quote(signed_target);
    run_command_or_die(cmd.str(), std::string("signtool verify (") + label + ")");
#else
    (void)signed_target;
    (void)label;
#endif
  }

  [[maybe_unused]] void sign_windows_package_or_die(const Args& a, const fs::path& package) {
    if (a.signing_policy == SigningPolicy::UnsignedDev)
      return;
    if (a.signing_policy == SigningPolicy::VerifyOnly) {
      verify_signtool_or_die(package, "package");
      return;
    }
#if defined(_WIN32)
    std::ostringstream cmd;
    cmd << "signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /f "
        << command_quote(fs::path(a.cert)) << " " << command_quote(package);
    run_command_or_die(cmd.str(), "signtool package signing");
    verify_signtool_or_die(package, "package");
#else
    (void)a;
    (void)package;
#endif
  }

  [[maybe_unused]] std::string generate_uuid() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<u64>(rd()) << 32) ^ rd());
    u64 high = gen();
    u64 low = gen();
    unsigned char bytes[16];
    for (int i = 0; i < 8; ++i)
      bytes[i] = static_cast<unsigned char>((high >> ((7 - i) * 8)) & 0xff);
    for (int i = 0; i < 8; ++i)
      bytes[i + 8] = static_cast<unsigned char>((low >> ((7 - i) * 8)) & 0xff);
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned>(bytes[0]), static_cast<unsigned>(bytes[1]),
                  static_cast<unsigned>(bytes[2]), static_cast<unsigned>(bytes[3]),
                  static_cast<unsigned>(bytes[4]), static_cast<unsigned>(bytes[5]),
                  static_cast<unsigned>(bytes[6]), static_cast<unsigned>(bytes[7]),
                  static_cast<unsigned>(bytes[8]), static_cast<unsigned>(bytes[9]),
                  static_cast<unsigned>(bytes[10]), static_cast<unsigned>(bytes[11]),
                  static_cast<unsigned>(bytes[12]), static_cast<unsigned>(bytes[13]),
                  static_cast<unsigned>(bytes[14]), static_cast<unsigned>(bytes[15]));
    return buf;
  }

  fs::path path_with_extension(fs::path path, const std::string& ext) {
    if (path.extension() == ext)
      return path;
    if (path.has_extension())
      path.replace_extension(ext);
    else
      path += ext;
    return path;
  }

  [[maybe_unused]] fs::path stage_windows_package_payload(const Args& a, const fs::path& exe) {
    fs::path payload = fs::temp_directory_path() / ("fxe-pack-" + a.name + "-package-payload");
    fs::remove_all(payload);
    fs::create_directories(payload);
    copy_one(exe, payload / exe.filename());
    fs::path icudtl = exe.parent_path() / "icudtl.dat";
    if (fs::exists(icudtl))
      copy_one(icudtl, payload / "icudtl.dat");
    return payload;
  }

  void sign_macos_dmg_or_die(const Args& a, const fs::path& dmg) {
    if (a.signing_policy == SigningPolicy::UnsignedDev)
      return;
    if (a.signing_policy == SigningPolicy::VerifyOnly) {
      verify_codesign_or_die(dmg, "dmg");
      return;
    }
    std::ostringstream cmd;
    cmd << "codesign --force --timestamp --sign " << shell_quote(fs::path(a.identity)) << " "
        << shell_quote(dmg);
    run_command_or_die(cmd.str(), "DMG codesign");
    verify_codesign_or_die(dmg, "dmg");
  }

  void notarize_macos_dmg_or_die(const Args& a, const fs::path& dmg) {
    if (a.signing_policy != SigningPolicy::SignedAndNotarized)
      return;
#if defined(__APPLE__)
    std::ostringstream submit_cmd;
    submit_cmd << "xcrun notarytool submit " << shell_quote(dmg) << " --keychain-profile "
               << shell_quote(fs::path(a.notarize_profile)) << " --wait";
    run_command_or_die(submit_cmd.str(), "DMG notarization");

    staple_with_retry_or_die(dmg, "DMG notarization");
#else
    (void)a;
    (void)dmg;
#endif
  }
  void verify_pkg_signature_or_die(const fs::path& signed_target, const char* label) {
#if defined(__APPLE__)
    std::ostringstream cmd;
    cmd << "pkgutil --check-signature " << shell_quote(signed_target);
    run_command_or_die(cmd.str(), std::string("pkg signature verify (") + label + ")");
#else
    (void)signed_target;
    (void)label;
#endif
  }

  void sign_macos_pkg_or_die(const Args& a, const fs::path& unsigned_pkg, const fs::path& out) {
    if (a.signing_policy == SigningPolicy::UnsignedDev) {
      move_one(unsigned_pkg, out);
      return;
    }
    if (a.signing_policy == SigningPolicy::VerifyOnly) {
      move_one(unsigned_pkg, out);
      verify_pkg_signature_or_die(out, "pkg");
      return;
    }
    fs::path signed_pkg = out;
    signed_pkg += ".signed";
    std::ostringstream cmd;
    cmd << "productsign --sign " << shell_quote(fs::path(a.identity)) << " "
        << shell_quote(unsigned_pkg) << " " << shell_quote(signed_pkg);
    run_command_or_die(cmd.str(), "PKG productsign");
    fs::remove(out);
    move_one(signed_pkg, out);
    fs::remove(unsigned_pkg);
    verify_pkg_signature_or_die(out, "pkg");
  }

  void notarize_macos_pkg_or_die(const Args& a, const fs::path& pkg) {
    if (a.signing_policy != SigningPolicy::SignedAndNotarized)
      return;
#if defined(__APPLE__)
    std::ostringstream submit_cmd;
    submit_cmd << "xcrun notarytool submit " << shell_quote(pkg) << " --keychain-profile "
               << shell_quote(fs::path(a.notarize_profile)) << " --wait";
    run_command_or_die(submit_cmd.str(), "PKG notarization");
    staple_with_retry_or_die(pkg, "PKG notarization");
#else
    (void)a;
    (void)pkg;
#endif
  }

  fs::path create_pkg_from_app(const Args& a, const fs::path& app, const fs::path& out) {
#if defined(__APPLE__)
    fs::path component_pkg = fs::temp_directory_path() / (a.name + ".component.pkg");
    fs::path unsigned_pkg = fs::temp_directory_path() / (a.name + ".unsigned.pkg");
    fs::remove(component_pkg);
    fs::remove(unsigned_pkg);
    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream pkgbuild_cmd;
    pkgbuild_cmd << "pkgbuild --component " << shell_quote(app)
                 << " --install-location /Applications " << shell_quote(component_pkg);
    run_command_or_die(pkgbuild_cmd.str(), "pkgbuild packaging");
    std::ostringstream productbuild_cmd;
    productbuild_cmd << "productbuild --package " << shell_quote(component_pkg) << " "
                     << shell_quote(unsigned_pkg);
    run_command_or_die(productbuild_cmd.str(), "productbuild packaging");
    // Distribution-XML customization can layer on top of this single-component flow later.
    sign_macos_pkg_or_die(a, unsigned_pkg, out);
    notarize_macos_pkg_or_die(a, out);
    fs::remove(component_pkg);
    return out;
#else
    (void)a;
    (void)app;
    (void)out;
    die(".pkg output requires hdiutil-class macOS tooling (pkgbuild/productbuild) and can only "
        "be built on macOS hosts");
#endif
  }

  fs::path build_macos_pkg(const Args& a, const fs::path& staged_bin, const fs::path& out,
                           const fs::path& tmpl_dir, const fs::path& fxe_run_dir) {
#if defined(__APPLE__)
    fs::path app = fs::temp_directory_path() / (a.name + ".app");
    wrap_macos_app(a, staged_bin, app, tmpl_dir, fxe_run_dir);
    if (a.signing_policy != SigningPolicy::VerifyOnly) {
      sign_macos_app_or_die(a, app);
      notarize_macos_app_or_die(a, app);
    }
    fs::path pkg = create_pkg_from_app(a, app, out);
    fs::remove_all(app);
    return pkg;
#else
    (void)a;
    (void)staged_bin;
    (void)out;
    (void)tmpl_dir;
    (void)fxe_run_dir;
    die(".pkg output requires hdiutil-class macOS tooling (pkgbuild/productbuild) and can only "
        "be built on macOS hosts");
#endif
  }

  fs::path create_dmg_from_app(const Args& a, const fs::path& app, const fs::path& out) {
#if defined(__APPLE__)
    fs::path stage = fs::temp_directory_path() / ("fxe-pack-" + a.name + "-dmg-stage");
    fs::remove_all(stage);
    fs::create_directories(stage);
    fs::copy(app, stage / app.filename(),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    std::error_code ec;
    fs::create_directory_symlink("/Applications", stage / "Applications", ec);
    if (ec)
      die("creating /Applications symlink failed: " + ec.message());

    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream cmd;
    cmd << "hdiutil create -volname " << shell_quote(fs::path(a.name)) << " -srcfolder "
        << shell_quote(stage) << " -ov -format UDZO " << shell_quote(out);
    run_command_or_die(cmd.str(), "DMG packaging");
    fs::remove_all(stage);
    sign_macos_dmg_or_die(a, out);
    notarize_macos_dmg_or_die(a, out);
    return out;
#else
    (void)a;
    (void)app;
    (void)out;
    die(".dmg output requires hdiutil and can only be built on macOS hosts");
#endif
  }

  fs::path create_msi_from_exe(const Args& a, const fs::path& exe, const fs::path& out,
                               const fs::path& tmpl_dir) {
#if defined(_WIN32)
    fs::path payload = stage_windows_package_payload(a, exe);
    fs::path wxs = payload.parent_path() / (safe_identifier(a.name) + ".wxs");
    fs::path wixobj = payload.parent_path() / (safe_identifier(a.name) + ".wixobj");
    std::string extra_components;
    std::string extra_component_refs;
    if (fs::exists(payload / "icudtl.dat")) {
      extra_components = "        <Component Id=\"IcuDataComponent\" Guid=\"*\">\n"
                         "          <File Id=\"IcuData\" Source=\"" +
                         xml_escape((payload / "icudtl.dat").string()) +
                         "\" KeyPath=\"yes\"/>\n"
                         "        </Component>\n";
      extra_component_refs = "      <ComponentRef Id=\"IcuDataComponent\"/>\n";
    }
    std::string wxs_body = load_template_or(tmpl_dir, "wix_product.wxs.in", "");
    wxs_body = subst(wxs_body, "APP_NAME", xml_escape(a.name));
    wxs_body = subst(wxs_body, "VERSION", a.package.version);
    wxs_body = subst(wxs_body, "MANUFACTURER", a.package.manufacturer);
    wxs_body = subst(wxs_body, "PRODUCT_CODE", generate_uuid());
    wxs_body = subst(wxs_body, "UPGRADE_CODE", "8F128C0A-5D3F-4F71-8CC0-34796C1FCB5D");
    wxs_body = subst(wxs_body, "EXE_RELATIVE_PATH", xml_escape(exe.filename().string()));
    wxs_body = subst(wxs_body, "EXE_SOURCE", xml_escape((payload / exe.filename()).string()));
    wxs_body = subst(wxs_body, "EXTRA_COMPONENTS", extra_components);
    wxs_body = subst(wxs_body, "EXTRA_COMPONENT_REFS", extra_component_refs);
    spit(wxs, wxs_body);

    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream candle;
    candle << "candle " << command_quote(wxs) << " -out " << command_quote(wixobj);
    run_command_or_die(candle.str(), "WiX candle");
    std::ostringstream light;
    light << "light " << command_quote(wixobj) << " -out " << command_quote(out);
    run_command_or_die(light.str(), "WiX light");
    fs::remove(wixobj);
    fs::remove(wxs);
    fs::remove_all(payload);
    sign_windows_package_or_die(a, out);
    return out;
#else
    (void)a;
    (void)exe;
    (void)out;
    (void)tmpl_dir;
    die("WiX not found on PATH (requires candle.exe and light.exe)");
#endif
  }

  fs::path create_msix_from_exe(const Args& a, const fs::path& exe, const fs::path& out,
                                const fs::path& tmpl_dir) {
#if defined(_WIN32)
    fs::path payload = stage_windows_package_payload(a, exe);
    fs::path assets = payload / "Assets";
    fs::create_directories(assets);
    const char one_pixel_png_bytes[] =
        "\211PNG\r\n\032\n\000\000\000\rIHDR\000\000\000\001\000\000\000\001"
        "\010\006\000\000\000\037\025\304\211\000\000\000\rIDATx\234c````\000"
        "\000\000\005\000\001\245\366E@\000\000\000\000IEND\256B`\202";
    const std::string one_pixel_png(one_pixel_png_bytes, sizeof(one_pixel_png_bytes) - 1);
    spit(assets / "StoreLogo.png", one_pixel_png);
    spit(assets / "Square44x44Logo.png", one_pixel_png);
    spit(assets / "Square150x150Logo.png", one_pixel_png);

    std::string manifest = load_template_or(tmpl_dir, "AppxManifest.xml.in", "");
    manifest = subst(manifest, "APP_NAME", xml_escape(a.name));
    manifest = subst(manifest, "VERSION", four_part_version(a.package.version));
    manifest = subst(manifest, "MANUFACTURER", a.package.manufacturer);
    manifest = subst(manifest, "IDENTITY_NAME", "com.fxe." + xml_escape(safe_identifier(a.name)));
    manifest = subst(manifest, "PUBLISHER", xml_escape(msix_publisher(a)));
    manifest = subst(manifest, "EXE_RELATIVE_PATH", xml_escape(exe.filename().string()));
    manifest = subst(manifest, "APP_ID", xml_escape(safe_identifier(a.name)));
    spit(payload / "AppxManifest.xml", manifest);

    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream pack;
    pack << "makeappx.exe pack /d " << command_quote(payload) << " /p " << command_quote(out);
    run_command_or_die(pack.str(), "MSIX packaging");
    fs::remove_all(payload);
    sign_windows_package_or_die(a, out);
    return out;
#else
    (void)a;
    (void)exe;
    (void)out;
    (void)tmpl_dir;
    die("makeappx.exe not found on PATH (Windows SDK MakeAppx is required)");
#endif
  }

  fs::path wrap_macos_dmg(const Args& a, const fs::path& staged_bin, const fs::path& out,
                          const fs::path& tmpl_dir, const fs::path& fxe_run_dir) {
#if defined(__APPLE__)
    fs::path app = fs::temp_directory_path() / (a.name + ".app");
    wrap_macos_app(a, staged_bin, app, tmpl_dir, fxe_run_dir);
    sign_macos_app_or_die(a, app);
    notarize_macos_app_or_die(a, app);

    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream cmd;
    cmd << "hdiutil create -volname " << shell_quote(fs::path(a.name)) << " -srcfolder "
        << shell_quote(app) << " -ov -format UDZO " << shell_quote(out);
    run_command_or_die(cmd.str(), "DMG packaging");
    fs::remove_all(app);
    return out;
#else
    (void)a;
    (void)staged_bin;
    (void)out;
    (void)tmpl_dir;
    (void)fxe_run_dir;
    die(".dmg output requires hdiutil and can only be built on macOS hosts");
#endif
  }

  [[maybe_unused]] fs::path stage_windows_payload(const Args& a, const fs::path& staged_bin,
                                                  const fs::path& fxe_run_dir) {
    fs::path payload = fs::temp_directory_path() / ("fxe-pack-" + a.name + "-payload");
    fs::remove_all(payload);
    fs::create_directories(payload);
    fs::path exe = payload / (a.name + ".exe");
    move_one(staged_bin, exe);
    copy_runtime_sidecars(fxe_run_dir, payload);
    sign_windows_exe_or_die(a, exe);
    return payload;
  }

  fs::path wrap_windows_msi(const Args& a, const fs::path& staged_bin, const fs::path& out,
                            const fs::path& fxe_run_dir) {
#if defined(_WIN32)
    fs::path payload = stage_windows_payload(a, staged_bin, fxe_run_dir);
    fs::path wxs = payload.parent_path() / (a.name + ".wxs");
    std::string app_id = safe_identifier(a.name);
    std::ostringstream files;
    files << "<Component Id=\"MainExeComponent\" Guid=\"*\"><File Id=\"MainExe\" Source=\""
          << xml_escape((payload / (a.name + ".exe")).string())
          << "\" KeyPath=\"yes\"/></Component>\n";
    if (fs::exists(payload / "icudtl.dat")) {
      files << "<Component Id=\"IcuDataComponent\" Guid=\"*\"><File Id=\"IcuData\" Source=\""
            << xml_escape((payload / "icudtl.dat").string())
            << "\" KeyPath=\"yes\"/></Component>\n";
    }
    std::ostringstream wxs_body;
    wxs_body
        << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<Wix xmlns=\"http://schemas.microsoft.com/wix/2006/wi\">\n"
        << "<Product Id=\"*\" Name=\"" << xml_escape(a.name) << "\" Language=\"1033\" Version=\""
        << xml_escape(a.package.version) << "\" Manufacturer=\""
        << xml_escape(a.package.manufacturer)
        << "\" UpgradeCode=\"8F128C0A-5D3F-4F71-8CC0-34796C1FCB5D\">\n"
        << "<Package InstallerVersion=\"500\" Compressed=\"yes\" InstallScope=\"perMachine\"/>\n"
        << "<MediaTemplate EmbedCab=\"yes\"/>\n"
        << "<Directory Id=\"TARGETDIR\" Name=\"SourceDir\"><Directory Id=\"ProgramFilesFolder\">"
        << "<Directory Id=\"INSTALLFOLDER\" Name=\"" << xml_escape(a.name) << "\">\n"
        << files.str() << "</Directory></Directory></Directory>\n"
        << "<Feature Id=\"DefaultFeature\" Title=\"" << xml_escape(a.name) << "\" Level=\"1\">"
        << "<ComponentRef Id=\"MainExeComponent\"/>";
    if (fs::exists(payload / "icudtl.dat"))
      wxs_body << "<ComponentRef Id=\"IcuDataComponent\"/>";
    wxs_body << "</Feature></Product></Wix>\n";
    spit(wxs, wxs_body.str());

    fs::create_directories(out.parent_path());
    fs::remove(out);
    if (tool_exists("wix")) {
      std::ostringstream cmd;
      cmd << "wix build " << command_quote(wxs) << " -o " << command_quote(out);
      run_command_or_die(cmd.str(), "WiX MSI packaging");
    } else {
      fs::path wixobj = payload.parent_path() / (app_id + ".wixobj");
      std::ostringstream candle;
      candle << "candle -out " << command_quote(wixobj) << " " << command_quote(wxs);
      run_command_or_die(candle.str(), "WiX candle");
      std::ostringstream light;
      light << "light -out " << command_quote(out) << " " << command_quote(wixobj);
      run_command_or_die(light.str(), "WiX light");
      fs::remove(wixobj);
    }
    sign_windows_exe_or_die(a, out);
    fs::remove(wxs);
    fs::remove_all(payload);
    return out;
#else
    (void)a;
    (void)staged_bin;
    (void)out;
    (void)fxe_run_dir;
    die(".msi output requires WiX tooling and can only be built on Windows hosts");
#endif
  }

  fs::path wrap_windows_msix(const Args& a, const fs::path& staged_bin, const fs::path& out,
                             const fs::path& fxe_run_dir) {
#if defined(_WIN32)
    fs::path payload = stage_windows_payload(a, staged_bin, fxe_run_dir);
    std::string identity = safe_identifier(a.name);
    std::string publisher = msix_publisher(a);
    std::ostringstream manifest;
    manifest
        << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\" "
           "xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\" "
           "IgnorableNamespaces=\"uap\">\n"
        << "<Identity Name=\"com.fxe." << xml_escape(identity) << "\" Publisher=\""
        << xml_escape(publisher) << "\" Version=\""
        << xml_escape(four_part_version(a.package.version)) << "\"/>\n"
        << "<Properties><DisplayName>" << xml_escape(a.name)
        << "</DisplayName><PublisherDisplayName>" << xml_escape(a.package.manufacturer)
        << "</PublisherDisplayName><Logo>Assets\\StoreLogo.png</Logo></Properties>\n"
        << "<Resources><Resource Language=\"en-us\"/></Resources>\n"
        << "<Dependencies><TargetDeviceFamily Name=\"Windows.Desktop\" "
           "MinVersion=\"10.0.17763.0\" "
           "MaxVersionTested=\"10.0.22621.0\"/></Dependencies>\n"
        << "<Applications><Application Id=\"" << xml_escape(identity) << "\" Executable=\""
        << xml_escape(a.name) << ".exe\" EntryPoint=\"Windows.FullTrustApplication\">"
        << "<uap:VisualElements DisplayName=\"" << xml_escape(a.name) << "\" Description=\""
        << xml_escape(a.name)
        << "\" BackgroundColor=\"transparent\" Square44x44Logo=\"Assets\\Square44x44Logo.png\" "
           "Square150x150Logo=\"Assets\\Square150x150Logo.png\"/>"
        << "</Application></Applications></Package>\n";
    spit(payload / "AppxManifest.xml", manifest.str());

    fs::path assets = payload / "Assets";
    fs::create_directories(assets);
    const char one_pixel_png_bytes[] =
        "\211PNG\r\n\032\n\000\000\000\rIHDR\000\000\000\001\000\000\000\001"
        "\010\006\000\000\000\037\025\304\211\000\000\000\rIDATx\234c````\000"
        "\000\000\005\000\001\245\366E@\000\000\000\000IEND\256B`\202";
    const std::string one_pixel_png(one_pixel_png_bytes, sizeof(one_pixel_png_bytes) - 1);
    spit(assets / "StoreLogo.png", one_pixel_png);
    spit(assets / "Square44x44Logo.png", one_pixel_png);
    spit(assets / "Square150x150Logo.png", one_pixel_png);

    fs::create_directories(out.parent_path());
    fs::remove(out);
    std::ostringstream pack;
    pack << "MakeAppx pack /d " << command_quote(payload) << " /p " << command_quote(out) << " /o";
    run_command_or_die(pack.str(), "MSIX packaging");
    sign_windows_exe_or_die(a, out);
    fs::remove_all(payload);
    return out;
#else
    (void)a;
    (void)staged_bin;
    (void)out;
    (void)fxe_run_dir;
    die(".msix output requires Windows SDK MakeAppx/signtool and can only be built on Windows "
        "hosts");
#endif
  }

} // namespace

int main(int argc, char** argv) {
  Args a = parse(argc, argv);
  if (produces_installer(a) && a.package.version.empty())
    die("--version is required when producing an installer (.dmg/.pkg/.msi/.msix/.appimage)");
  if (produces_installer(a) && a.package.manufacturer.empty())
    die("--manufacturer/--publisher is required when producing an installer");
  if (!produces_installer(a) && a.package.version.empty())
    a.package.version = "0.0.0"; // non-installer fallback
  if (!produces_installer(a) && a.package.manufacturer.empty())
    a.package.manufacturer = "unknown"; // non-installer fallback
  validate_requested_tools(a);

  fs::path self = fs::weakly_canonical(fs::path(argv[0]));
  fs::path self_dir = self.parent_path();
  fs::path fxe_run = locate_fxe_run(self_dir);
  fs::path tmpl_dir = templates_dir(self_dir);

  fs::path stage = fs::temp_directory_path() / ("fxe-pack-" + a.name);
  fs::remove_all(stage);
  fs::create_directories(stage);
  fs::path staged_bin = stage / a.name;
  copy_one(fxe_run, staged_bin);
  make_executable(staged_bin);

  Files files = collect(a);
  std::string err;
  fxe::bundle::ManifestMetadata manifest;
  manifest.app_name = a.name;
  manifest.version = a.package.version;
  manifest.entry = files.entry_archive;
  manifest.created_at = utc_timestamp();
  manifest.compression = compression_name(a.compress);
  manifest.update_url = a.package.update_url;
  manifest.public_key = a.package.public_key;
  manifest.channel = a.package.channel;

  if (!fxe::bundle::pack_files(staged_bin.string(),
                               std::vector<std::pair<std::string, std::string>>(files.v), &manifest,
                               &err)) {
    die("bundle pack failed: " + err);
  }

  fs::path requested_out = fs::path(a.out);
  if (!requested_out.has_parent_path())
    requested_out = fs::current_path() / requested_out;
  fs::path final_out;
  std::string artifact_kind = "bundled binary";
  std::vector<std::pair<std::string, fs::path>> extra_artifacts;

  if (a.platform == "macos" && a.installer == InstallerFormat::Dmg) {
    fs::path app_out = requested_out;
    if (app_out.extension() == ".dmg")
      app_out.replace_extension(".app");
    else
      app_out = path_with_extension(app_out, ".app");
    final_out = wrap_macos_app(a, staged_bin, app_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "macOS .app bundle";
    sign_macos_app_or_die(a, final_out);
    notarize_macos_app_or_die(a, final_out);
    fs::path dmg_out = requested_out.extension() == ".dmg" ? requested_out
                                                           : path_with_extension(final_out, ".dmg");
    extra_artifacts.emplace_back("macOS DMG disk image",
                                 create_dmg_from_app(a, final_out, dmg_out));
  } else if (a.platform == "macos" && a.installer == InstallerFormat::Pkg) {
    fs::path app_out = requested_out;
    if (app_out.extension() == ".pkg")
      app_out.replace_extension(".app");
    else
      app_out = path_with_extension(app_out, ".app");
    final_out = wrap_macos_app(a, staged_bin, app_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "macOS .app bundle";
    if (a.signing_policy != SigningPolicy::VerifyOnly) {
      sign_macos_app_or_die(a, final_out);
      notarize_macos_app_or_die(a, final_out);
    }
    fs::path pkg_out = requested_out.extension() == ".pkg" ? requested_out
                                                           : path_with_extension(final_out, ".pkg");
    extra_artifacts.emplace_back("macOS PKG installer", create_pkg_from_app(a, final_out, pkg_out));
  } else if (a.platform == "macos" && requested_out.extension() == ".app") {
    final_out = wrap_macos_app(a, staged_bin, requested_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "macOS .app bundle";
    sign_macos_app_or_die(a, final_out);
    notarize_macos_app_or_die(a, final_out);
  } else if (a.platform == "macos" && requested_out.extension() == ".pkg") {
    final_out = build_macos_pkg(a, staged_bin, requested_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "macOS PKG installer";
  } else if (a.platform == "macos" && requested_out.extension() == ".dmg") {
    final_out = wrap_macos_dmg(a, staged_bin, requested_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "macOS DMG disk image";
  } else if (a.platform == "win" &&
             (a.installer == InstallerFormat::Msi || a.installer == InstallerFormat::Msix)) {
    fs::path exe_out = requested_out;
    if (exe_out.extension() == ".msi" || exe_out.extension() == ".msix")
      exe_out.replace_extension(".exe");
    final_out = wrap_win_exe(a, staged_bin, exe_out, fxe_run.parent_path());
    artifact_kind = "Windows .exe";
    sign_windows_exe_or_die(a, final_out);
    if (a.installer == InstallerFormat::Msi) {
      fs::path msi_out = requested_out.extension() == ".msi"
                             ? requested_out
                             : path_with_extension(final_out, ".msi");
      extra_artifacts.emplace_back("Windows MSI installer",
                                   create_msi_from_exe(a, final_out, msi_out, tmpl_dir));
    }
    if (a.installer == InstallerFormat::Msix) {
      fs::path msix_out = requested_out.extension() == ".msix"
                              ? requested_out
                              : path_with_extension(final_out, ".msix");
      extra_artifacts.emplace_back("Windows MSIX package",
                                   create_msix_from_exe(a, final_out, msix_out, tmpl_dir));
    }
  } else if (a.platform == "win" && requested_out.extension() == ".msi") {
    final_out = wrap_windows_msi(a, staged_bin, requested_out, fxe_run.parent_path());
    artifact_kind = "Windows MSI installer";
  } else if (a.platform == "win" && requested_out.extension() == ".msix") {
    final_out = wrap_windows_msix(a, staged_bin, requested_out, fxe_run.parent_path());
    artifact_kind = "Windows MSIX package";
  } else if (a.platform == "win") {
    final_out = wrap_win_exe(a, staged_bin, requested_out, fxe_run.parent_path());
    artifact_kind = "Windows .exe";
    sign_windows_exe_or_die(a, final_out);
  } else if (a.platform == "linux" && (a.installer == InstallerFormat::AppImage ||
                                       requested_out.extension() == ".AppImage")) {
    final_out = wrap_linux_appimage(a, staged_bin, requested_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "Linux AppImage";
  } else if (a.platform == "linux" && has_suffix(requested_out.string(), ".tar.gz")) {
    final_out = wrap_linux_tar_gz(a, staged_bin, requested_out, tmpl_dir, fxe_run.parent_path());
    artifact_kind = "Linux .tar.gz archive";
  } else {
    final_out = write_plain_output(staged_bin, requested_out);
  }

  maybe_zstd_compress(a, final_out);
  fs::remove_all(stage);

  std::cout << "fxe-pack: built " << artifact_kind << ": " << final_out.string() << " for "
            << a.platform << "\n";
  for (const auto& [kind, path] : extra_artifacts) {
    std::cout << "fxe-pack: built " << kind << ": " << path.string() << " for " << a.platform
              << "\n";
  }
  if (a.signing_policy == SigningPolicy::UnsignedDev) {
    std::cout << "fxe-pack: NOTE: output uses --signing-policy unsigned-dev and was not signed.\n";
  } else if (a.signing_policy == SigningPolicy::SignedRelease && a.platform == "macos") {
    std::cout << "fxe-pack: NOTE: output is signed but not notarized; use --signing-policy "
                 "signed-and-notarized for notarized macOS release output.\n";
  } else if (a.signing_policy == SigningPolicy::VerifyOnly) {
    std::cout << "fxe-pack: NOTE: verification ran without signing due to --signing-policy "
                 "verify-only.\n";
  }
  return 0;
}
