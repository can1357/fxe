// (face_id, glyph_id, size, hint, sub_x_bin) → Glyph cache + the two atlas
// pages. Owned by `font::GlyphCache`. The cache stores rendered glyph records
// plus retained bitmap bytes so atlas pages can be rebuilt deterministically
// after LRU eviction or packer pressure.

#include <fxe/font.hpp>
#include <fxe/font/atlas.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/glyph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <list>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fxe::font {
  namespace {
    [[nodiscard]] std::size_t format_index(Format format) noexcept {
      return format == Format::bgra ? 1u : 0u;
    }

    [[nodiscard]] std::uint64_t warning_key(const GlyphKey& key) noexcept {
      std::uint64_t h = 14695981039346656037ull;
      const auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(key.face_id);
      mix(key.pixel_size_q);
      return h;
    }

    [[nodiscard]] std::vector<std::uint8_t> extract_pixels(const Atlas& atlas, const Glyph& glyph) {
      if (glyph.width == 0 || glyph.height == 0)
        return {};

      const auto size = atlas.size();
      const auto bpp = static_cast<std::size_t>(atlas.bytes_per_pixel());
      if (glyph.atlas_x + glyph.width > size.x || glyph.atlas_y + glyph.height > size.y)
        return {};

      std::vector<std::uint8_t> out(static_cast<std::size_t>(glyph.width) * glyph.height * bpp);
      for (std::uint32_t y = 0; y < glyph.height; ++y) {
        const std::uint8_t* src =
            atlas.pixels().data() +
            (static_cast<std::size_t>(glyph.atlas_y + y) * size.x + glyph.atlas_x) * bpp;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * glyph.width * bpp;
        std::copy_n(src, static_cast<std::size_t>(glyph.width) * bpp, dst);
      }
      return out;
    }
  } // namespace

  struct GlyphCache::Impl {
    struct RenderRecipe {
      std::uint64_t face_id = 0;
      std::uint32_t glyph_id = 0;
      std::uint32_t pixel_size_q = 0;
      std::uint8_t hint = 1;
      std::uint8_t sub_x_bin = 0;
    };

    struct Entry {
      Glyph glyph{};
      std::list<GlyphKey>::iterator lru_pos{};
      // Retaining bytes costs memory, but makes repack deterministic and
      // independent of backend face lifetime.
      std::vector<std::uint8_t> pixel_bytes;
      std::uint32_t format = static_cast<std::uint32_t>(Format::grayscale);
      std::uint16_t width = 0;
      std::uint16_t height = 0;
      RenderRecipe render_recipe{};
    };

    explicit Impl(GlyphCacheBudget b) : budget(sanitize(b)) {
      reset_atlases();
    }

    [[nodiscard]] static GlyphCacheBudget sanitize(GlyphCacheBudget b) noexcept {
      b.initial_atlas_size = std::max<std::uint32_t>(b.initial_atlas_size, 1);
      b.max_atlas_size = std::max(b.max_atlas_size, b.initial_atlas_size);
      return b;
    }

    void reset_atlases() {
      mask = Atlas{Format::grayscale, budget.initial_atlas_size, budget.max_atlas_size};
      color = Atlas{Format::bgra, budget.initial_atlas_size, budget.max_atlas_size};
    }

    [[nodiscard]] Atlas& atlas_for(Format format) noexcept {
      return format == Format::bgra ? color : mask;
    }

    [[nodiscard]] const Atlas& atlas_for(Format format) const noexcept {
      return format == Format::bgra ? color : mask;
    }

    [[nodiscard]] std::size_t max_count(Format format) const noexcept {
      return format == Format::bgra ? budget.max_color_glyph_count : budget.max_mask_glyph_count;
    }

    [[nodiscard]] std::size_t max_bytes(Format format) const noexcept {
      return format == Format::bgra ? budget.max_color_atlas_bytes : budget.max_mask_atlas_bytes;
    }

    [[nodiscard]] std::size_t atlas_bytes(Format format) const noexcept {
      const auto size = atlas_for(format).size();
      return static_cast<std::size_t>(size.x) * size.y * atlas_for(format).bytes_per_pixel();
    }

    void promote(std::unordered_map<GlyphKey, Entry, GlyphKeyHash>::iterator it) {
      lru.splice(lru.begin(), lru, it->second.lru_pos);
      it->second.lru_pos = lru.begin();
    }

    [[nodiscard]] bool same_key(const GlyphKey& a, const GlyphKey* b) const noexcept {
      return b && a == *b;
    }

    [[nodiscard]] bool evict_one_lru(Format format, const GlyphKey* protected_key = nullptr) {
      for (auto rit = lru.rbegin(); rit != lru.rend(); ++rit) {
        auto it = cache.find(*rit);
        if (it == cache.end())
          continue;
        if (it->second.glyph.format != format)
          continue;
        if (same_key(it->first, protected_key))
          continue;

        auto list_it = std::prev(rit.base());
        GlyphKey victim = *list_it;
        cache.erase(victim);
        lru.erase(list_it);
        --counts[format_index(format)];
        ++evictions[format_index(format)];
        return true;
      }
      return false;
    }

    [[nodiscard]] std::vector<AtlasRepackItem> live_items(Format format) {
      std::vector<AtlasRepackItem> items;
      items.reserve(counts[format_index(format)]);
      for (const GlyphKey& key : lru) {
        auto it = cache.find(key);
        if (it == cache.end() || it->second.glyph.format != format)
          continue;
        Entry& entry = it->second;
        items.push_back(AtlasRepackItem{&entry.glyph,
                                        std::span<const std::uint8_t>(entry.pixel_bytes),
                                        entry.width, entry.height});
      }
      return items;
    }

    [[nodiscard]] bool rebuild(Format format) {
      auto items = live_items(format);
      return atlas_for(format).rebuild_from_live(items);
    }

    [[nodiscard]] bool over_budget(Format format) const noexcept {
      const std::size_t idx = format_index(format);
      return counts[idx] > max_count(format) || atlas_bytes(format) > max_bytes(format);
    }

    [[nodiscard]] bool enforce_budget(Format format, const GlyphKey& protected_key) {
      bool changed = false;
      while (over_budget(format)) {
        if (!evict_one_lru(format, &protected_key))
          return false;
        changed = true;
        if (!rebuild(format))
          return false;
      }
      return !changed || rebuild(format);
    }

    [[nodiscard]] bool recover_space_for_pack(Format format, const GlyphKey& protected_key) {
      const std::size_t before = counts[format_index(format)];
      std::size_t to_evict = std::max<std::size_t>(1, before / 4);
      bool evicted = false;
      while (to_evict-- > 0 && evict_one_lru(format, &protected_key))
        evicted = true;
      return evicted && rebuild(format);
    }

    [[nodiscard]] bool pack_entry(const GlyphKey& key, Entry& entry) {
      if (entry.width == 0 || entry.height == 0) {
        entry.glyph.atlas_x = 0;
        entry.glyph.atlas_y = 0;
        return true;
      }

      const Format format = entry.glyph.format;
      Atlas& atlas = atlas_for(format);
      for (;;) {
        AtlasRegion r = atlas.pack(entry.width, entry.height, entry.pixel_bytes.data());
        if (r.ok) {
          entry.glyph.atlas_x = r.x;
          entry.glyph.atlas_y = r.y;
          return true;
        }
        if (!recover_space_for_pack(format, key))
          return false;
      }
    }

    void warn_oversize_once(const GlyphKey& key, Format format) {
      const std::uint64_t wk = warning_key(key);
      if (!oversize_warnings.insert(wk).second)
        return;
      std::fprintf(stderr,
                   "fxe.font: glyph atlas cannot fit glyphs for face=%llu size_q=%u format=%u; "
                   "using missing-glyph fallback\n",
                   static_cast<unsigned long long>(key.face_id), key.pixel_size_q,
                   static_cast<unsigned>(format));
    }

    GlyphCacheBudget budget{};
    Atlas mask{Format::grayscale, 256, 8192};
    Atlas color{Format::bgra, 256, 8192};
    std::unordered_map<GlyphKey, Entry, GlyphKeyHash> cache;
    std::list<GlyphKey> lru;
    std::array<std::size_t, 2> counts{};
    std::array<std::size_t, 2> evictions{};
    std::unordered_set<std::uint64_t> oversize_warnings;
    Glyph empty{};
  };

  GlyphCache::GlyphCache() : GlyphCache(GlyphCacheBudget{}) {}
  GlyphCache::GlyphCache(GlyphCacheBudget budget) : impl_(std::make_unique<Impl>(budget)) {}
  GlyphCache::~GlyphCache() = default;

  Atlas& GlyphCache::mask_atlas() noexcept {
    return impl_->mask;
  }
  Atlas& GlyphCache::color_atlas() noexcept {
    return impl_->color;
  }
  const Atlas& GlyphCache::mask_atlas() const noexcept {
    return impl_->mask;
  }
  const Atlas& GlyphCache::color_atlas() const noexcept {
    return impl_->color;
  }

  const Glyph& GlyphCache::lookup(Face& face, std::uint32_t glyph_id, float subpixel_x, Hint hint) {
    // Quantise subpixel x into 4 bins. 0 → integer pen position; 1 → ¼ px,
    // etc. Callers asking for `subpixel_x` outside [0, 1) get clamped.
    if (!std::isfinite(subpixel_x) || subpixel_x < 0.0f)
      subpixel_x = 0.0f;
    if (subpixel_x >= 1.0f)
      subpixel_x -= std::floor(subpixel_x);
    const std::uint8_t sub_bin = static_cast<std::uint8_t>(subpixel_x * 4.0f) & 0x3;
    // Snap the actual sub-pixel offset we hand to the rasteriser to the
    // centre of the chosen bin so each cache entry is rendered at a
    // consistent fractional position regardless of which fractional input
    // landed in it.
    const float bin_subpixel = static_cast<float>(sub_bin) * 0.25f;

    GlyphKey k{};
    k.face_id = face.id();
    k.glyph_id = glyph_id;
    k.pixel_size_q = static_cast<std::uint32_t>(std::lround(face.pixel_size() * 64.0f));
    k.subpixel_x = sub_bin;
    k.hint = static_cast<std::uint8_t>(hint);

    if (auto it = impl_->cache.find(k); it != impl_->cache.end()) {
      impl_->promote(it);
      return it->second.glyph;
    }

    Atlas scratch_mask{Format::grayscale, 256, 8192};
    Atlas scratch_color{Format::bgra, 256, 8192};
    Glyph rendered = face.render_glyph(glyph_id, scratch_mask, scratch_color, hint, bin_subpixel);
    const Format format = rendered.format;
    const Atlas& scratch = (format == Format::bgra) ? scratch_color : scratch_mask;
    std::vector<std::uint8_t> pixels = extract_pixels(scratch, rendered);
    if ((rendered.width != 0 || rendered.height != 0) && pixels.empty()) {
      impl_->warn_oversize_once(k, format);
      return impl_->empty;
    }

    Impl::Entry entry{};
    entry.glyph = rendered;
    entry.pixel_bytes = std::move(pixels);
    entry.format = static_cast<std::uint32_t>(format);
    entry.width = static_cast<std::uint16_t>(std::min<std::uint32_t>(rendered.width, 0xffffu));
    entry.height = static_cast<std::uint16_t>(std::min<std::uint32_t>(rendered.height, 0xffffu));
    entry.render_recipe =
        Impl::RenderRecipe{k.face_id, k.glyph_id, k.pixel_size_q, k.hint, k.subpixel_x};

    if (rendered.width > 0xffffu || rendered.height > 0xffffu || !impl_->pack_entry(k, entry)) {
      impl_->warn_oversize_once(k, format);
      return impl_->empty;
    }

    impl_->lru.push_front(k);
    entry.lru_pos = impl_->lru.begin();
    auto [it, inserted] = impl_->cache.emplace(k, std::move(entry));
    if (!inserted)
      return it->second.glyph;
    ++impl_->counts[format_index(format)];

    if (!impl_->enforce_budget(format, k)) {
      auto live = impl_->cache.find(k);
      if (live != impl_->cache.end()) {
        impl_->cache.erase(live);
        impl_->lru.pop_front();
        --impl_->counts[format_index(format)];
        ++impl_->evictions[format_index(format)];
        (void)impl_->rebuild(format);
      }
      impl_->warn_oversize_once(k, format);
      return impl_->empty;
    }

    auto live = impl_->cache.find(k);
    if (live == impl_->cache.end())
      return impl_->empty;
    return live->second.glyph;
  }

  void GlyphCache::set_budget(GlyphCacheBudget budget) {
    impl_->budget = Impl::sanitize(budget);
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->counts = {};
    impl_->reset_atlases();
  }

  std::size_t GlyphCache::cache_size(Format format) const noexcept {
    return impl_->counts[format_index(format)];
  }

  std::size_t GlyphCache::eviction_count(Format format) const noexcept {
    return impl_->evictions[format_index(format)];
  }

  std::size_t GlyphCache::atlas_bytes(Format format) const noexcept {
    return impl_->atlas_bytes(format);
  }

  std::uint64_t GlyphCache::generation(Format format) const noexcept {
    return impl_->atlas_for(format).generation();
  }

  bool GlyphCache::debug_contains(const GlyphKey& key) const noexcept {
    return impl_->cache.find(key) != impl_->cache.end();
  }

  void GlyphCache::clear() {
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->counts = {};
    impl_->mask.clear();
    impl_->color.clear();
  }

} // namespace fxe::font
