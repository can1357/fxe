// CoreText-backed Shaper. Uses CTLine + CTRun to get shaped, positioned
// glyphs out of a CTFontRef. Apple's text engine handles complex scripts,
// kerning, and ligatures, so the API surface is essentially "build a
// CFAttributedString, ask CTLineCreateWithAttributedString, walk the runs".
//
// Feature flags travel via `kCTFontFeatureSettingsAttribute`. CoreText
// uses Apple's pre-OpenType feature selectors internally but accepts
// modern OpenType-style dicts too via the AAT bridge layer.

#include <fxe/font/embedded_nerd.hpp>
#include <fxe/font/face.hpp>
#include <fxe/font/shaper.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <CoreText/CoreText.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>
#include <fxe/types.hpp>

namespace fxe::font {
  namespace {

    class CoreTextShaper final : public Shaper {
    public:
      CoreTextShaper() = default;
      ~CoreTextShaper() override {
        for (auto& [_, ref] : cascaded_by_face_id_) {
          if (ref) CFRelease(ref);
        }
        if (nerd_descriptor_) CFRelease(nerd_descriptor_);
      }

      [[nodiscard]] std::vector<ShapeRun> shape(Face& face, std::string_view utf8,
                                                const ShapeOptions& opts) override {
        std::vector<ShapeRun> out;
        auto append_empty = [&]() {
          ShapeRun r{};
          r.direction = opts.direction;
          r.face = &face;
          out.push_back(std::move(r));
          return std::move(out);
        };
        if (utf8.empty()) return append_empty();

        CTFontRef ct = static_cast<CTFontRef>(face.native_handle());
        if (!ct) return append_empty();

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
        if (!text) return append_empty();

        // Build attributes: just the font for the simple case. Feature flags
        // are encoded as an array of dicts with `kCTFontOpenTypeFeatureTag`/`-Value`.
        // Swap the bare CTFontRef for a copy that carries our embedded
        // Nerd Font in its cascade list. The CTLine cascade walks this
        // list for any codepoint the primary doesn't cover, so PUA glyphs
        // (powerline, devicons, font-awesome, …) render in the embedded
        // face at the same size with zero JS-side wiring.
        CTFontRef cascaded = build_cascaded(ct);
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attrs, kCTFontAttributeName, cascaded);

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
        if (!attrText) return append_empty();

        CTLineRef line = CTLineCreateWithAttributedString(attrText);
        CFRelease(attrText);
        if (!line) return append_empty();

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
          CTFontRef run_ct = cascaded;
          if (CFDictionaryRef run_attrs = CTRunGetAttributes(run); run_attrs) {
            if (auto* maybe_font = CFDictionaryGetValue(run_attrs, kCTFontAttributeName)) {
              run_ct = static_cast<CTFontRef>(maybe_font);
            }
          }
          Face* run_face = &face;
          // Compare against the cascaded primary, not the bare `ct`: when
          // we baked the Nerd Font cascade into `cascaded`, the primary
          // text run reports `cascaded` here. Treating that as a
          // substitution would mint a duplicate Face for the primary.
          if (run_ct != cascaded) {
            run_face = resolve_substitute_face(run_ct);
            if (!run_face) run_face = &face; // best-effort fallback
          }

          std::vector<CGGlyph> glyphs(static_cast<usize>(glyph_count));
          std::vector<CGSize> advances(static_cast<usize>(glyph_count));
          std::vector<CGPoint> positions(static_cast<usize>(glyph_count));
          std::vector<CFIndex> indices(static_cast<usize>(glyph_count));
          const CFRange whole = CFRangeMake(0, glyph_count);
          CTRunGetGlyphs(run, whole, glyphs.data());
          CTRunGetAdvances(run, whole, advances.data());
          CTRunGetPositions(run, whole, positions.data());
          CTRunGetStringIndices(run, whole, indices.data());

          ShapeRun srun{};
          srun.direction = opts.direction;
          srun.face = run_face;
          srun.glyphs.reserve(static_cast<usize>(glyph_count));
          // CTRun positions are absolute within the parent CTLine, not the
          // run. For the first run that's harmless (line starts at x=0), but
          // for subsequent runs (e.g. the emoji run after "Welcome back ")
          // the first glyph's `pos.x` already encodes ~130px of leading text.
          // Treating that as an x_offset would push the glyph 130px past the
          // pen. Subtract the run-local origin so per-glyph offsets stay
          // relative to the start of THIS run; the renderer is responsible
          // for advancing pen across runs via the per-glyph `x_advance`.
          const float run_origin_x =
              glyph_count > 0 ? static_cast<float>(positions[0].x) : 0.0f;
          const float run_origin_y =
              glyph_count > 0 ? static_cast<float>(positions[0].y) : 0.0f;
          for (CFIndex j = 0; j < glyph_count; ++j) {
            ShapedGlyph g{};
            g.glyph_id = glyphs[static_cast<usize>(j)];
            g.x_advance = static_cast<float>(advances[static_cast<usize>(j)].width);
            g.y_advance = static_cast<float>(advances[static_cast<usize>(j)].height);
            float prev_x = 0.0f;
            float prev_y = 0.0f;
            if (j > 0) {
              const auto& p = positions[static_cast<usize>(j - 1)];
              prev_x = static_cast<float>(p.x) - run_origin_x
                       + static_cast<float>(advances[static_cast<usize>(j - 1)].width);
              prev_y = static_cast<float>(p.y) - run_origin_y
                       + static_cast<float>(advances[static_cast<usize>(j - 1)].height);
            }
            const auto& pos = positions[static_cast<usize>(j)];
            g.x_offset = (static_cast<float>(pos.x) - run_origin_x) - prev_x;
            g.y_offset = (static_cast<float>(pos.y) - run_origin_y) - prev_y;
            g.cluster = static_cast<u32>(indices[static_cast<usize>(j)]);
            srun.glyphs.push_back(g);
            srun.total_advance += g.x_advance;
          }
          out.push_back(std::move(srun));
        }

        CFRelease(line);
        if (out.empty()) return append_empty();
        return out;
      }

    private:
      // Cache substitute Faces keyed by the logical font identity, not
      // CTFontRef pointer identity. CoreText creates a fresh CTFontRef on
      // every cascade resolution even when the resolved logical font is
      // identical, so caching by pointer never hit and every cascade
      // emoji/symbol minted a new face_id — exhausting the glyph cache
      // budget within a few hundred frames and triggering eviction +
      // repack thrash that scrambled cached text UVs.
      //
      // Logical identity = (PostScript name, quantised pixel size). Two
      // CTFontRefs that wrap the same physical font at the same size
      // produce the same key, so we get one Face per (font, size) pair
      // and the glyph cache deduplicates correctly.
      Face* resolve_substitute_face(CTFontRef ct) {
        const float px = static_cast<float>(CTFontGetSize(ct));
        std::string key;
        if (CFStringRef ps = CTFontCopyPostScriptName(ct); ps) {
          char buf[256] = {};
          if (CFStringGetCString(ps, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            key.assign(buf);
          }
          CFRelease(ps);
        }
        // Append size so different sizes of the same font get separate Faces.
        // (Glyph rendering depends on size; sharing a Face across sizes
        // would produce wrong-sized atlas entries.)
        key.push_back('@');
        const u32 size_q = static_cast<u32>(std::lround(px * 64.0f));
        char size_buf[16];
        std::snprintf(size_buf, sizeof(size_buf), "%u", size_q);
        key.append(size_buf);

        auto it = substitute_faces_.find(key);
        if (it != substitute_faces_.end()) return it->second.get();
        auto wrapped = make_face_from_ctfont(reinterpret_cast<void*>(const_cast<__CTFont*>(ct)), px);
        if (!wrapped) return nullptr;
        Face* raw = wrapped.get();
        substitute_faces_.emplace(std::move(key), std::move(wrapped));
        return raw;
      }

      // Returns a CTFontRef wrapping `base` with our embedded Nerd Font
      // appended to the cascade list. Cached per-base by face id so we
      // pay the descriptor-clone cost once per (font, size) tuple. The
      // returned font is owned by the cache; callers do NOT release it.
      CTFontRef build_cascaded(CTFontRef base) {
        if (!base) return base;
        const uintptr_t key = reinterpret_cast<uintptr_t>(base);
        if (auto it = cascaded_by_face_id_.find(key); it != cascaded_by_face_id_.end())
          return it->second;

        CTFontDescriptorRef nerd_desc = ensure_nerd_descriptor();
        if (!nerd_desc) {
          cascaded_by_face_id_.emplace(key, base);
          return base; // no descriptor → no cascade, just hand back original
        }

        const void* cascade_items[] = {nerd_desc};
        CFArrayRef cascade =
            CFArrayCreate(nullptr, cascade_items, 1, &kCFTypeArrayCallBacks);
        if (!cascade) {
          cascaded_by_face_id_.emplace(key, base);
          return base;
        }

        const void* keys[] = {kCTFontCascadeListAttribute};
        const void* vals[] = {cascade};
        CFDictionaryRef attrs = CFDictionaryCreate(
            nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFRelease(cascade);
        if (!attrs) {
          cascaded_by_face_id_.emplace(key, base);
          return base;
        }

        CTFontDescriptorRef base_desc = CTFontCopyFontDescriptor(base);
        CTFontDescriptorRef new_desc =
            base_desc ? CTFontDescriptorCreateCopyWithAttributes(base_desc, attrs) : nullptr;
        if (base_desc) CFRelease(base_desc);
        CFRelease(attrs);
        if (!new_desc) {
          cascaded_by_face_id_.emplace(key, base);
          return base;
        }

        CTFontRef wrapped =
            CTFontCreateWithFontDescriptor(new_desc, CTFontGetSize(base), nullptr);
        CFRelease(new_desc);
        if (!wrapped) {
          cascaded_by_face_id_.emplace(key, base);
          return base;
        }

        cascaded_by_face_id_.emplace(key, wrapped);
        return wrapped;
      }

      // Lazily creates a CTFontDescriptor for the embedded Nerd Font.
      // CTFontManagerCreateFontDescriptorsFromData makes the font usable
      // without installing it into the user/system font dirs; the
      // descriptor stays valid for the lifetime of the shaper.
      CTFontDescriptorRef ensure_nerd_descriptor() {
        std::call_once(nerd_descriptor_once_, [this]() {
          const auto bytes = embedded_nerd_font_bytes();
          if (bytes.empty()) return;
          CFDataRef data = CFDataCreate(nullptr, bytes.data(),
                                        static_cast<CFIndex>(bytes.size()));
          if (!data) return;
          CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromData(data);
          CFRelease(data);
          if (!descs) return;
          if (CFArrayGetCount(descs) > 0) {
            nerd_descriptor_ =
                static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descs, 0));
            CFRetain(nerd_descriptor_);
          }
          CFRelease(descs);
        });
        return nerd_descriptor_;
      }

      std::unordered_map<std::string, std::unique_ptr<Face>> substitute_faces_;
      std::unordered_map<uintptr_t, CTFontRef> cascaded_by_face_id_;
      std::once_flag nerd_descriptor_once_;
      CTFontDescriptorRef nerd_descriptor_ = nullptr;

    };

  } // namespace

  std::unique_ptr<Shaper> make_coretext_shaper() {
    return std::make_unique<CoreTextShaper>();
  }

} // namespace fxe::font