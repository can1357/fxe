#include "bundle_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>

#include <fxe/log.hpp>

#include "fxa_archive.hpp"

namespace fxe::runtime {

  namespace {

    std::once_flag g_once;
    std::unique_ptr<fxe::runtime::fxa_archive::Bundle> g_bundle;
    std::string g_entry;
    bool g_signature_verified = false;

    std::string normalize(std::string_view name) {
      std::string s(name);
      if (s.size() >= 2 && s[0] == '.' && s[1] == '/')
        s.erase(0, 2);
      return s;
    }
    bool allow_unsigned_override() {
      const char* value = std::getenv("FXE_BUNDLE_ALLOW_UNSIGNED");
      return value && std::string_view(value) == "1";
    }

  } // namespace

  bool mount_bundle_from_argv0(const char* argv0) {
    if (!argv0)
      return false;
    bool ok = false;
    std::call_once(g_once, [&]() {
      std::error_code ec;
      std::filesystem::path p(argv0);
      if (p.is_relative()) {
        // Resolve via $PATH-equivalent: just use weakly_canonical relative to cwd.
        auto abs = std::filesystem::weakly_canonical(p, ec);
        if (!ec && !abs.empty())
          p = abs;
      }
      auto b = std::make_unique<fxe::runtime::fxa_archive::Bundle>(p.string());
      if (!b->valid())
        return;
      const bool allow_override = allow_unsigned_override();
      const bool needs_override = !b->signed_archive() || !b->signature_verified();
      const bool allow_unsigned =
#ifdef NDEBUG
          allow_override;
#else
          !b->signed_archive() || allow_override;
#endif
      if (needs_override && !allow_unsigned)
        return;
      if (needs_override && allow_override) {
        FXE_WARN("runtime.bundle",
                 "allowing bundle load without a verified archive signature because "
                 "FXE_BUNDLE_ALLOW_UNSIGNED=1");
      }
      g_signature_verified = b->signature_verified();
      // Look up entry pointer: prefer "__entry__" sentinel if present, else "main".
      if (auto e = b->read("__entry__")) {
        g_entry = *e;
      } else if (b->read("main.ts")) {
        g_entry = "main.ts";
      } else if (b->read("main.js")) {
        g_entry = "main.js";
      }
      g_bundle = std::move(b);
      ok = true;
    });
    return ok || g_bundle != nullptr;
  }

  bool bundle_mounted() {
    return g_bundle != nullptr;
  }
  bool bundle_signature_verified() {
    return g_signature_verified;
  }

  std::string bundle_entry() {
    return g_entry;
  }

  std::optional<std::string> read_virtual(std::string_view name) {
    if (!g_bundle)
      return std::nullopt;
    if (auto v = g_bundle->read(name))
      return v;
    auto n = normalize(name);
    if (n != name) {
      if (auto v = g_bundle->read(n))
        return v;
    }
    // Try basename if absolute / nested.
    auto slash = n.find_last_of('/');
    if (slash != std::string::npos) {
      if (auto v = g_bundle->read(n.substr(slash + 1)))
        return v;
    }
    return std::nullopt;
  }

  std::vector<std::string> list_virtual() {
    if (!g_bundle)
      return {};
    return g_bundle->list();
  }

} // namespace fxe::runtime
