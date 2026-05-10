#include "bundle_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <fxe/log.hpp>

#include "fxa_archive.hpp"

namespace fxe::runtime {

  namespace {

    std::once_flag g_once;
    std::unique_ptr<fxe::runtime::fxa_archive::Bundle> g_bundle;
    std::string g_entry;
    bool g_signature_verified = false;
    std::vector<fxe::runtime::fxa_archive::BundledFont> g_fonts;
    std::unordered_map<std::string, std::shared_ptr<std::string>> g_font_bytes;

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
    [[nodiscard]] std::string lower_ascii(std::string_view value) {
      std::string out;
      out.reserve(value.size());
      for (char ch : value)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      return out;
    }

    [[nodiscard]] bool matches_ci(std::string_view lhs, std::string_view rhs) {
      return lower_ascii(lhs) == lower_ascii(rhs);
    }

    [[nodiscard]] u32 weight_distance(u32 lhs, u32 rhs) {
      return lhs >= rhs ? lhs - rhs : rhs - lhs;
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
      g_fonts = b->fonts();
      g_font_bytes.clear();
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
  std::vector<fxe::runtime::fxa_archive::BundledFont> bundle_fonts() {
    if (!g_bundle)
      return {};
    return g_fonts;
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

  std::optional<bundle_font_entry> resolve_bundled_font(std::string_view family, u32 weight,
                                                        std::string_view style) {
    if (!g_bundle)
      return std::nullopt;
    const std::string family_lc = lower_ascii(family);
    const std::string style_lc = lower_ascii(style);
    const fxe::runtime::fxa_archive::BundledFont* best = nullptr;
    u32 best_distance = 0;
    for (const auto& font : g_fonts) {
      if (!matches_ci(font.family, family_lc))
        continue;
      const u32 distance = weight_distance(font.weight, weight);
      if (!best || distance < best_distance) {
        best = &font;
        best_distance = distance;
      }
    }
    if (!best)
      return std::nullopt;
    const fxe::runtime::fxa_archive::BundledFont* exact_style = nullptr;
    const fxe::runtime::fxa_archive::BundledFont* normal_style = nullptr;
    const fxe::runtime::fxa_archive::BundledFont* fallback = nullptr;
    for (const auto& font : g_fonts) {
      if (!matches_ci(font.family, family_lc) ||
          weight_distance(font.weight, weight) != best_distance)
        continue;
      if (!fallback)
        fallback = &font;
      if (matches_ci(font.style, style_lc) && !exact_style)
        exact_style = &font;
      if (matches_ci(font.style, "normal") && !normal_style)
        normal_style = &font;
    }
    best = exact_style ? exact_style : (normal_style ? normal_style : fallback);
    if (!best)
      return std::nullopt;
    auto it = g_font_bytes.find(best->virtual_path);
    if (it == g_font_bytes.end()) {
      auto bytes = g_bundle->read(best->virtual_path);
      if (!bytes)
        return std::nullopt;
      it =
          g_font_bytes.emplace(best->virtual_path, std::make_shared<std::string>(std::move(*bytes)))
              .first;
    }
    const auto& stored = *it->second;
    return bundle_font_entry{
        .virtual_name = best->virtual_path,
        .bytes_view =
            std::span<const u8>(reinterpret_cast<const u8*>(stored.data()), stored.size()),
    };
  }

  std::vector<std::string> list_virtual() {
    if (!g_bundle)
      return {};
    return g_bundle->list();
  }

} // namespace fxe::runtime
