// CoreText-backed Shaper. Uses CTLine + CTRun to get shaped, positioned
// glyphs out of a CTFontRef. Apple's text engine handles complex scripts,
// kerning, and ligatures, so the API surface is essentially "build a
// CFAttributedString, ask CTLineCreateWithAttributedString, walk the runs".
//
// Feature flags travel via `kCTFontFeatureSettingsAttribute`. CoreText
// uses Apple's pre-OpenType feature selectors internally but accepts
// modern OpenType-style dicts too via the AAT bridge layer.

#include <fxe/font/face.hpp>
#include <fxe/font/shaper.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <CoreText/CoreText.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::font {
  namespace {

    class CoreTextShaper final : public Shaper {
    public:
      [[nodiscard]] ShapeRun shape(Face& face, std::string_view utf8,
                                   const ShapeOptions& opts) override {
        ShapeRun out{};
        out.direction = opts.direction;
        if (utf8.empty()) return out;

        CTFontRef ct = static_cast<CTFontRef>(face.native_handle());
        if (!ct) return out;

        // Apply variations on the face if requested. CoreText handles axes
        // through the Face::set_variations API; the shaper itself is stateless.
        if (!opts.variations.empty()) {
          face.set_variations(opts.variations);
          ct = static_cast<CTFontRef>(face.native_handle());
        }

        CFStringRef text = CFStringCreateWithBytes(nullptr,
                                                   reinterpret_cast<const UInt8*>(utf8.data()),
                                                   static_cast<CFIndex>(utf8.size()),
                                                   kCFStringEncodingUTF8, false);
        if (!text) return out;

        // Build attributes: just the font for the simple case. Feature flags
        // are encoded as an array of dicts with `kCTFontOpenTypeFeatureTag`/`-Value`.
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attrs, kCTFontAttributeName, ct);

        // Build a feature-settings array if we have any.
        std::vector<Feature> feats =
            opts.features.empty() ? default_features() : opts.features;
        if (!feats.empty()) {
          CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
          for (const auto& f : feats) {
            const std::string tagstr = f.tag.str();
            CFStringRef tagcf = CFStringCreateWithBytes(
                nullptr, reinterpret_cast<const UInt8*>(tagstr.data()),
                static_cast<CFIndex>(tagstr.size()), kCFStringEncodingASCII, false);
            if (!tagcf) continue;
            const SInt32 v = static_cast<SInt32>(f.value);
            CFNumberRef vcf = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
            const void* keys[2] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
            const void* vals[2] = {tagcf, vcf};
            CFDictionaryRef d = CFDictionaryCreate(nullptr, keys, vals, 2,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
            if (d) {
              CFArrayAppendValue(arr, d);
              CFRelease(d);
            }
            CFRelease(tagcf);
            CFRelease(vcf);
          }
          if (CFArrayGetCount(arr) > 0) {
            CFDictionarySetValue(attrs, kCTFontFeatureSettingsAttribute, arr);
          }
          CFRelease(arr);
        }

        CFAttributedStringRef attrText = CFAttributedStringCreate(nullptr, text, attrs);
        CFRelease(text);
        CFRelease(attrs);
        if (!attrText) return out;

        CTLineRef line = CTLineCreateWithAttributedString(attrText);
        CFRelease(attrText);
        if (!line) return out;

        CFArrayRef runs = CTLineGetGlyphRuns(line);
        const CFIndex run_count = CFArrayGetCount(runs);
        for (CFIndex i = 0; i < run_count; ++i) {
          CTRunRef run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, i));
          const CFIndex glyph_count = CTRunGetGlyphCount(run);
          if (glyph_count <= 0) continue;
          std::vector<CGGlyph> glyphs(static_cast<std::size_t>(glyph_count));
          std::vector<CGSize> advances(static_cast<std::size_t>(glyph_count));
          std::vector<CGPoint> positions(static_cast<std::size_t>(glyph_count));
          std::vector<CFIndex> indices(static_cast<std::size_t>(glyph_count));
          const CFRange whole = CFRangeMake(0, glyph_count);
          CTRunGetGlyphs(run, whole, glyphs.data());
          CTRunGetAdvances(run, whole, advances.data());
          CTRunGetPositions(run, whole, positions.data());
          CTRunGetStringIndices(run, whole, indices.data());
          for (CFIndex j = 0; j < glyph_count; ++j) {
            ShapedGlyph g{};
            g.glyph_id = glyphs[static_cast<std::size_t>(j)];
            g.x_advance = static_cast<float>(advances[static_cast<std::size_t>(j)].width);
            g.y_advance = static_cast<float>(advances[static_cast<std::size_t>(j)].height);
            // CoreText's positions are absolute within the line; we want
            // per-glyph offsets (offset from baseline pen). Compute offsets by
            // subtracting the cumulative advance up to this glyph.
            float prev_x = 0.0f;
            float prev_y = 0.0f;
            if (j > 0) {
              const auto& p = positions[static_cast<std::size_t>(j - 1)];
              prev_x = static_cast<float>(p.x)
                       + static_cast<float>(advances[static_cast<std::size_t>(j - 1)].width);
              prev_y = static_cast<float>(p.y)
                       + static_cast<float>(advances[static_cast<std::size_t>(j - 1)].height);
            }
            const auto& pos = positions[static_cast<std::size_t>(j)];
            g.x_offset = static_cast<float>(pos.x) - prev_x;
            g.y_offset = static_cast<float>(pos.y) - prev_y;
            g.cluster = static_cast<std::uint32_t>(indices[static_cast<std::size_t>(j)]);
            out.glyphs.push_back(g);
            out.total_advance += g.x_advance;
          }
        }

        CFRelease(line);
        return out;
      }
    };

  } // namespace

  std::unique_ptr<Shaper> make_coretext_shaper() {
    return std::make_unique<CoreTextShaper>();
  }

} // namespace fxe::font
