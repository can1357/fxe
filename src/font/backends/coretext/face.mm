// CoreText-backed Face. macOS default rasterizer.
//
// CoreText is heavyweight on the JVM/V8/Node side because it pulls in
// Core Animation runtime, but is essentially free for native processes:
// it ships with the OS and is the same engine AppKit/UIKit use, so the
// rasterized glyphs match what the user sees in every other macOS app.
//
// Glyph rendering goes through `CGContextRef`. We allocate a one-shot
// CGBitmapContext per glyph sized to the bounding box, blit the rendered
// bitmap into the appropriate atlas page (mask or color), and emit a
// Glyph record. Color glyphs (Apple Color Emoji) use sbix tables and
// CoreText returns BGRA pixels directly when the context is RGBA.

#include <fxe/font/atlas.hpp>
#include <fxe/font/face.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <fxe/types.hpp>

namespace fxe::font {
  namespace {
    std::atomic<u64> g_face_id{1};

    Style style_from_traits(CTFontSymbolicTraits t) noexcept {
      const bool b = (t & kCTFontTraitBold) != 0;
      const bool i = (t & kCTFontTraitItalic) != 0;
      if (b && i) return Style::bold_italic;
      if (b) return Style::bold;
      if (i) return Style::italic;
      return Style::regular;
    }

    class CoreTextFace final : public Face {
    public:
      CoreTextFace(CTFontRef font, float pixel_size)
          : font_(font), pixel_size_(pixel_size), id_(g_face_id.fetch_add(1)) {
        CFRetain(font_);
        traits_ = CTFontGetSymbolicTraits(font_);
        const CFRange r = CFRangeMake(0, 0);
        ascent_ = static_cast<float>(CTFontGetAscent(font_));
        descent_ = static_cast<float>(CTFontGetDescent(font_));
        leading_ = static_cast<float>(CTFontGetLeading(font_));
        underline_pos_ = static_cast<float>(CTFontGetUnderlinePosition(font_));
        underline_thick_ = static_cast<float>(CTFontGetUnderlineThickness(font_));
        // 'M' as a rough em advance for fixed-pitch-ish fonts.
        const UniChar mch = 'M';
        CGGlyph mglyph = 0;
        if (CTFontGetGlyphsForCharacters(font_, &mch, &mglyph, 1) && mglyph != 0) {
          CGSize adv{};
          CTFontGetAdvancesForGlyphs(font_, kCTFontOrientationHorizontal, &mglyph, &adv, 1);
          em_advance_ = static_cast<float>(adv.width);
        }
        (void)r;
      }
      ~CoreTextFace() override {
        if (font_) CFRelease(font_);
      }

      [[nodiscard]] u64 id() const noexcept override { return id_; }

      [[nodiscard]] std::string family_name() const override {
        CFStringRef cf = CTFontCopyFamilyName(font_);
        if (!cf) return {};
        std::string out;
        const CFIndex len = CFStringGetLength(cf);
        const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        out.resize(static_cast<usize>(max));
        if (CFStringGetCString(cf, out.data(), max, kCFStringEncodingUTF8)) {
          out.resize(std::strlen(out.c_str()));
        } else {
          out.clear();
        }
        CFRelease(cf);
        return out;
      }

      [[nodiscard]] Style style() const noexcept override { return style_from_traits(traits_); }
      [[nodiscard]] float pixel_size() const noexcept override { return pixel_size_; }

      [[nodiscard]] FaceMetrics metrics() const noexcept override {
        FaceMetrics m{};
        m.ascent = ascent_;
        m.descent = descent_;
        m.line_gap = leading_;
        m.line_height = ascent_ + descent_ + leading_;
        m.underline_position = underline_pos_;
        m.underline_thickness = underline_thick_;
        m.em_advance = em_advance_;
        return m;
      }

      [[nodiscard]] bool has_color() const noexcept override {
        return (traits_ & kCTFontTraitColorGlyphs) != 0;
      }

      [[nodiscard]] u32 glyph_index(char32_t cp) const noexcept override {
        // Convert codepoint to UTF-16 surrogate pair if needed.
        UniChar units[2] = {0, 0};
        CGGlyph glyphs[2] = {0, 0};
        u32 count = 1;
        if (cp < 0x10000) {
          units[0] = static_cast<UniChar>(cp);
        } else {
          char32_t v = cp - 0x10000;
          units[0] = static_cast<UniChar>(0xD800 + (v >> 10));
          units[1] = static_cast<UniChar>(0xDC00 + (v & 0x3FF));
          count = 2;
        }
        if (!CTFontGetGlyphsForCharacters(font_, units, glyphs, count)) return 0;
        return glyphs[0];
      }

      void set_variations(std::span<const Variation> vs) override {
        if (vs.empty()) return;
        CFMutableDictionaryRef axes = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        for (const auto& v : vs) {
          const u32 tag = v.tag.packed();
          CFNumberRef key = CFNumberCreate(nullptr, kCFNumberSInt32Type, &tag);
          const double dv = static_cast<double>(v.value);
          CFNumberRef val = CFNumberCreate(nullptr, kCFNumberDoubleType, &dv);
          CFDictionarySetValue(axes, key, val);
          CFRelease(key);
          CFRelease(val);
        }
        const void* desc_keys[1] = {kCTFontVariationAttribute};
        const void* desc_vals[1] = {axes};
        CFDictionaryRef descAttrs = CFDictionaryCreate(
            nullptr, desc_keys, desc_vals, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(descAttrs);
        CTFontRef next = CTFontCreateCopyWithAttributes(
            font_, static_cast<CGFloat>(pixel_size_), nullptr, desc);
        CFRelease(desc);
        CFRelease(descAttrs);
        CFRelease(axes);
        if (next) {
          CFRelease(font_);
          font_ = next;
        }
      }

      [[nodiscard]] Glyph render_glyph(u32 glyph_id, Atlas& mask, Atlas& color,
                                       Hint /*hint*/, float subpixel_x = 0.0f) override {
        Glyph g{};
        if (glyph_id == 0) return g;
        const CGGlyph cg = static_cast<CGGlyph>(glyph_id);
        CGRect bbox{};
        CTFontGetBoundingRectsForGlyphs(font_, kCTFontOrientationHorizontal, &cg, &bbox, 1);
        CGSize adv{};
        CTFontGetAdvancesForGlyphs(font_, kCTFontOrientationHorizontal, &cg, &adv, 1);
        g.advance_x = static_cast<float>(adv.width);
        if (bbox.size.width <= 0 || bbox.size.height <= 0) return g;

        // Clamp `subpixel_x` to [0, 1) — the cache quantises into 4 bins so
        // anything outside that range is a miscount on the caller's side.
        if (!std::isfinite(subpixel_x) || subpixel_x < 0.0f) subpixel_x = 0.0f;
        if (subpixel_x >= 1.0f) subpixel_x -= std::floor(subpixel_x);

        // Pad the bbox by 1 pixel on every side to avoid clipping antialiased
        // edges when the glyph extends slightly outside its reported box. We
        // also widen the right edge by 1 extra pixel so a non-zero
        // `subpixel_x` shift cannot push fringe pixels past the bitmap.
        const int pad = 1;
        const int x0 = static_cast<int>(std::floor(bbox.origin.x)) - pad;
        const int y0 = static_cast<int>(std::floor(bbox.origin.y)) - pad;
        const int x1 = static_cast<int>(std::ceil(bbox.origin.x + bbox.size.width)) + pad
                       + (subpixel_x > 0.0f ? 1 : 0);
        const int y1 = static_cast<int>(std::ceil(bbox.origin.y + bbox.size.height)) + pad;
        const u32 w = static_cast<u32>(std::max(1, x1 - x0));
        const u32 h = static_cast<u32>(std::max(1, y1 - y0));

        const bool color_glyph = has_color();
        const Format fmt = color_glyph ? Format::bgra : Format::grayscale;

        // Allocate a CGBitmapContext sized to the glyph and render. For mask
        // glyphs we use a single-component grayscale context, which is what
        // Ghostty does too.
        std::vector<u8> buffer;
        CGContextRef ctx = nullptr;
        if (color_glyph) {
          buffer.assign(static_cast<usize>(w) * h * 4, 0);
          CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
          ctx = CGBitmapContextCreate(buffer.data(), w, h, 8, w * 4, cs,
                                      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst)
                                          | kCGBitmapByteOrder32Little);
          CGColorSpaceRelease(cs);
        } else {
          buffer.assign(static_cast<usize>(w) * h, 0);
          CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
          ctx = CGBitmapContextCreate(buffer.data(), w, h, 8, w, cs, kCGImageAlphaNone);
          CGColorSpaceRelease(cs);
        }
        if (!ctx) return g;

        CGContextSetShouldAntialias(ctx, true);
        CGContextSetShouldSmoothFonts(ctx, true);
        CGContextSetAllowsFontSubpixelPositioning(ctx, true);
        CGContextSetShouldSubpixelPositionFonts(ctx, true);
        // Quantization snaps the drawing position to the nearest pixel (or a
        // very coarse fraction of one) even when sub-pixel positioning is on,
        // which collapses the per-bin bitmaps the cache asks for back into
        // identical renderings. Ghostty turns it off for the same reason —
        // we own the per-bin position quantisation in `glyph_cache.cpp`, so
        // CoreText must honour the fractional offset we hand it.
        CGContextSetAllowsFontSubpixelQuantization(ctx, false);
        CGContextSetShouldSubpixelQuantizeFonts(ctx, false);
        if (!color_glyph) {
          CGContextSetGrayFillColor(ctx, 1.0, 1.0);
        }

        // Bake the requested sub-pixel shift directly into the rendered
        // bitmap. With quantisation off CoreText preserves the fractional
        // pen offset, so each of the four bins yields a visibly distinct
        // hinted glyph — exactly what the cache wants stored.
        CGPoint pos{static_cast<CGFloat>(-x0) + static_cast<CGFloat>(subpixel_x),
                    static_cast<CGFloat>(-y0)};
        CTFontDrawGlyphs(font_, &cg, &pos, 1, ctx);
        CGContextRelease(ctx);

        // Pack into the appropriate atlas. For mask glyphs the buffer is
        // already the alpha channel. For color glyphs we copy 4 bytes per
        // pixel; the bitmap is BGRA-little-endian = ARGB-big = "BGRA" on
        // disk in the order we want.
        Atlas& target = color_glyph ? color : mask;
        AtlasRegion region;
        if (color_glyph) {
          // CoreText with kCGBitmapByteOrder32Little + AlphaPremultipliedFirst
          // produces BGRA in memory.
          region = target.pack(w, h, buffer.data());
        } else {
          // CoreText draws into the gray channel as an inverted alpha for
          // dark-on-light. The buffer is the alpha mask directly.
          region = target.pack(w, h, buffer.data());
        }
        if (!region.ok) return g;

        g.atlas_x = region.x;
        g.atlas_y = region.y;
        g.width = w;
        g.height = h;
        g.format = fmt;
        // CoreText reports y-up bbox origins relative to the baseline; convert
        // to y-down screen offsets where (0, 0) is the top-left of the glyph
        // image.
        g.offset_x = static_cast<float>(x0);
        g.offset_y = static_cast<float>(-y1);
        return g;
      }

      [[nodiscard]] void* native_handle() const noexcept override {
        return reinterpret_cast<void*>(const_cast<__CTFont*>(font_));
      }

    private:
      CTFontRef font_ = nullptr;
      float pixel_size_ = 0.0f;
      u64 id_ = 0;
      CTFontSymbolicTraits traits_ = 0;
      float ascent_ = 0.0f;
      float descent_ = 0.0f;
      float leading_ = 0.0f;
      float underline_pos_ = 0.0f;
      float underline_thick_ = 0.0f;
      float em_advance_ = 0.0f;
    };

  } // namespace

  std::unique_ptr<Face> load_face_coretext(std::span<const u8> bytes, float pixel_size) {
    if (bytes.empty()) return nullptr;
    CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
    if (!data) return nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider) return nullptr;
    CGFontRef cg = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!cg) return nullptr;
    CTFontRef ct = CTFontCreateWithGraphicsFont(cg, static_cast<CGFloat>(pixel_size), nullptr, nullptr);
    CGFontRelease(cg);
    if (!ct) return nullptr;
    auto face = std::make_unique<CoreTextFace>(ct, pixel_size);
    CFRelease(ct);
    return face;
  }

  std::unique_ptr<Face> make_face_from_ctfont(void* ct_font_ref, float pixel_size) {
    if (!ct_font_ref) return nullptr;
    CTFontRef ct = static_cast<CTFontRef>(ct_font_ref);
    if (pixel_size <= 0.0f || !std::isfinite(pixel_size))
      pixel_size = static_cast<float>(CTFontGetSize(ct));
    return std::make_unique<CoreTextFace>(ct, pixel_size);
  }

  std::unique_ptr<Face> load_face_coretext_name(std::string_view family, float pixel_size,
                                                Style style) {
    CFStringRef name = CFStringCreateWithBytes(nullptr,
                                                reinterpret_cast<const UInt8*>(family.data()),
                                                static_cast<CFIndex>(family.size()),
                                                kCFStringEncodingUTF8, false);
    if (!name) return nullptr;

    // Build a descriptor that selects bold/italic via symbolic traits.
    CTFontSymbolicTraits want = 0;
    switch (style) {
    case Style::bold:
      want = kCTFontTraitBold;
      break;
    case Style::italic:
      want = kCTFontTraitItalic;
      break;
    case Style::bold_italic:
      want = kCTFontTraitBold | kCTFontTraitItalic;
      break;
    case Style::regular:
      break;
    }

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, name);
    CFRelease(name);
    CFMutableDictionaryRef traits = CFDictionaryCreateMutable(nullptr, 0,
                                                                &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
    CFNumberRef wantNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &want);
    CFDictionarySetValue(traits, kCTFontSymbolicTrait, wantNum);
    CFRelease(wantNum);
    CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traits);
    CFRelease(traits);

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);
    if (!desc) return nullptr;
    CTFontRef ct = CTFontCreateWithFontDescriptor(desc, static_cast<CGFloat>(pixel_size), nullptr);
    CFRelease(desc);
    if (!ct) return nullptr;
    auto face = std::make_unique<CoreTextFace>(ct, pixel_size);
    CFRelease(ct);
    return face;
  }

} // namespace fxe::font
