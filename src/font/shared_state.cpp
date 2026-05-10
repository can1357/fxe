// Process-wide font module singletons.
//
//   shared_glyph_cache() — the cache renderers upload from each frame.
//   shared_collection()  — the lazy-loaded default {regular,bold,italic,bi}
//                          face stack with an emoji-capable fallback.
//
// Both are lazy: the collection runs discovery on first access. If discovery
// returns nothing (e.g. running on a stripped-down system without
// fontconfig/CT/win32-fonts), the collection stays empty and the text path
// degrades to a no-op.

#include "../runtime/bundle_loader.hpp"
#include <fxe/font.hpp>

#include <atomic>
#include <cmath>
#include <utility>

namespace fxe::font {

  GlyphCache& shared_glyph_cache() {
    static GlyphCache c;
    return c;
  }

  namespace {
    [[nodiscard]] u32 style_weight(Style style) {
      switch (style) {
      case Style::regular:
      case Style::italic:
        return 400;
      case Style::bold:
      case Style::bold_italic:
        return 700;
      }
      return 400;
    }

    [[nodiscard]] std::string_view style_name(Style style) {
      switch (style) {
      case Style::regular:
      case Style::bold:
        return "normal";
      case Style::italic:
      case Style::bold_italic:
        return "italic";
      }
      return "normal";
    }

    [[nodiscard]] std::shared_ptr<Face> load_bundled_face(std::string_view family, Style style,
                                                          f32 pixel_size) {
      auto bundled =
          fxe::runtime::resolve_bundled_font(family, style_weight(style), style_name(style));
      if (!bundled)
        return nullptr;
      auto face = load_face_from_bytes(bundled->bytes_view, pixel_size, 0);
      if (!face)
        return nullptr;
      return std::shared_ptr<Face>(std::move(face));
    }

    void add_family_candidate(Collection& c, Discover* disc, Style style, std::string_view family,
                              f32 pixel_size, bool is_fallback, bool& added_primary) {
      if (auto bundled = load_bundled_face(family, style, pixel_size)) {
        if (!added_primary && !is_fallback) {
          c.add_primary(style, std::move(bundled));
          added_primary = true;
        } else {
          c.add_fallback(style, std::move(bundled));
        }
        return;
      }
      if (!disc)
        return;
      Descriptor q;
      q.family = std::string(family);
      q.style = style;
      q.size_pt = pixel_size;
      auto results = disc->find(q);
      if (results.empty())
        return;
      auto& d = results.front();
      d.style = style;
      d.size_pt = pixel_size;
      if (!added_primary && !is_fallback) {
        c.add_primary(style, std::move(d));
        added_primary = true;
      } else {
        c.add_fallback(style, std::move(d));
      }
    }

    void populate_default_collection(Collection& c) {
      auto disc = default_discover();

      // Style → list of preferred families. Ordered from "platform native"
      // outwards so the first hit wins.
      struct StylePref {
        Style style;
        std::initializer_list<const char*> families;
      };
      const StylePref prefs[] = {
          {Style::regular,
           {"SF Pro", "SF Pro Text", "San Francisco", "Helvetica Neue", "Helvetica", "DejaVu Sans",
            "Liberation Sans", "Noto Sans", "Segoe UI", "Arial"}},
          {Style::bold,
           {"SF Pro", "SF Pro Text", "Helvetica Neue", "Helvetica", "DejaVu Sans",
            "Liberation Sans", "Noto Sans", "Segoe UI", "Arial"}},
          {Style::italic,
           {"SF Pro", "SF Pro Text", "Helvetica Neue", "Helvetica", "DejaVu Sans",
            "Liberation Sans", "Noto Sans", "Segoe UI", "Arial"}},
          {Style::bold_italic,
           {"SF Pro", "SF Pro Text", "Helvetica Neue", "Helvetica", "DejaVu Sans",
            "Liberation Sans", "Noto Sans", "Segoe UI", "Arial"}},
      };

      const float default_pt = 16.0f;
      for (const auto& sp : prefs) {
        bool added_primary = false;
        for (const char* fam : sp.families)
          add_family_candidate(c, disc.get(), sp.style, fam, default_pt, false, added_primary);
      }

      // Emoji fallback. Try platform-native names; let discovery do the rest.
      const char* emoji_families[] = {
          "Apple Color Emoji",
          "Segoe UI Emoji",
          "Noto Color Emoji",
          "Twitter Color Emoji",
      };
      for (Style s : {Style::regular, Style::bold, Style::italic, Style::bold_italic}) {
        for (const char* fam : emoji_families) {
          Descriptor q;
          q.family = fam;
          q.style = Style::regular;
          q.size_pt = default_pt;
          q.require_color = true;
          auto results = disc->find(q);
          if (results.empty())
            continue;
          auto& d = results.front();
          d.style = Style::regular;
          d.size_pt = default_pt;
          d.require_color = true;
          c.add_fallback(s, std::move(d));
          break; // one emoji fallback per style is enough.
        }
      }
    }
  } // namespace

  Collection& shared_collection() {
    static Collection c;
    static bool inited = false;
    if (!inited) {
      inited = true;
      populate_default_collection(c);
    }
    return c;
  }

} // namespace fxe::font

namespace fxe::font {
  namespace {
    std::atomic<float> g_dpr{1.0f};
  } // namespace

  void set_device_pixel_ratio(float dpr) noexcept {
    if (!std::isfinite(dpr) || dpr <= 0.0f)
      dpr = 1.0f;
    g_dpr.store(dpr, std::memory_order_relaxed);
  }

  float device_pixel_ratio() noexcept {
    return g_dpr.load(std::memory_order_relaxed);
  }
} // namespace fxe::font
