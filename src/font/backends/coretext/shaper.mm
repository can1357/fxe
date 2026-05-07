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
#include <unordered_map>
#include <string_view>
#include <vector>

namespace fxe::font {
  namespace {

    class CoreTextShaper final : public Shaper {
    public:
      [[nodiscard]] std::vector<ShapeRun> shape(Face& face, std::string_view utf8,
                                                const ShapeOptions& opts) override {
        std::vector<ShapeRun> out;
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

          // CoreText cascades to a different physical font when the requested
          // font does not cover a codepoint (Latin → Apple Color Emoji for
          // 🔒 for example). Each CTRun reports the font it actually used in
          // its attribute dictionary. We resolve that here so the renderer
          // looks up glyphs against the right Face — otherwise the glyph_id
          // we hand out belongs to the substitute font but the cache renders
          // it through the original face, producing missing/garbled output.
          CTFontRef run_ct = ct;
          if (CFDictionaryRef run_attrs = CTRunGetAttributes(run); run_attrs) {
            if (auto* maybe_font = CFDictionaryGetValue(run_attrs, kCTFontAttributeName)) {
              run_ct = static_cast<CTFontRef>(maybe_font);
            }
          }
          Face* run_face = &face;
          if (run_ct != ct) {
            run_face = resolve_substitute_face(run_ct);
            if (!run_face) run_face = &face; // best-effort fallback
          }

          std::vector<CGGlyph> glyphs(static_cast<std::size_t>(glyph_count));
          std::vector<CGSize> advances(static_cast<std::size_t>(glyph_count));
          std::vector<CGPoint> positions(static_cast<std::size_t>(glyph_count));
          std::vector<CFIndex> indices(static_cast<std::size_t>(glyph_count));
          const CFRange whole = CFRangeMake(0, glyph_count);
          CTRunGetGlyphs(run, whole, glyphs.data());
          CTRunGetAdvances(run, whole, advances.data());
          CTRunGetPositions(run, whole, positions.data());
          CTRunGetStringIndices(run, whole, indices.data());

          ShapeRun srun{};
          srun.direction = opts.direction;
          srun.face = run_face;
          srun.glyphs.reserve(static_cast<std::size_t>(glyph_count));
          for (CFIndex j = 0; j < glyph_count; ++j) {
            ShapedGlyph g{};
            g.glyph_id = glyphs[static_cast<std::size_t>(j)];
            g.x_advance = static_cast<float>(advances[static_cast<std::size_t>(j)].width);
            g.y_advance = static_cast<float>(advances[static_cast<std::size_t>(j)].height);
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
            srun.glyphs.push_back(g);
            srun.total_advance += g.x_advance;
          }
          out.push_back(std::move(srun));
        }

        CFRelease(line);
        return out;
      }

    private:
      // Cache substitute Faces keyed by CTFontRef pointer identity. CoreText
      // returns the same CTFontRef for repeated cascade hits within a process,
      // so a flat map is enough. We retain CTFonts to keep the keys valid;
      // the wrapping Face also retains internally for glyph rendering.
      Face* resolve_substitute_face(CTFontRef ct) {
        auto it = substitute_faces_.find(ct);
        if (it != substitute_faces_.end()) return it->second.get();
        const float px = static_cast<float>(CTFontGetSize(ct));
        auto wrapped = make_face_from_ctfont(reinterpret_cast<void*>(const_cast<__CTFont*>(ct)), px);
        if (!wrapped) return nullptr;
        Face* raw = wrapped.get();
        substitute_faces_.emplace(ct, std::move(wrapped));
        return raw;
      }

      std::unordered_map<CTFontRef, std::unique_ptr<Face>> substitute_faces_;
    };

  } // namespace

  std::unique_ptr<Shaper> make_coretext_shaper() {
    return std::make_unique<CoreTextShaper>();
  }

} // namespace fxe::font