// CoreText-backed font discovery. Uses CTFontDescriptor matching to turn a
// `Descriptor` into 0+ resolved descriptors with concrete file paths.

#include <fxe/font/discover.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <CoreText/CoreText.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <fxe/types.hpp>

namespace fxe::font {
  namespace {

    std::string cf_string_to_utf8(CFStringRef s) {
      if (!s) return {};
      const CFIndex len = CFStringGetLength(s);
      const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
      std::string out(static_cast<usize>(max), '\0');
      if (CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
      } else {
        out.clear();
      }
      return out;
    }

    class CoreTextDiscover final : public Discover {
    public:
      [[nodiscard]] std::vector<Descriptor> find(const Descriptor& q) override {
        std::vector<Descriptor> out;

        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        if (!q.family.empty()) {
          CFStringRef fam = CFStringCreateWithBytes(
              nullptr, reinterpret_cast<const UInt8*>(q.family.data()),
              static_cast<CFIndex>(q.family.size()), kCFStringEncodingUTF8, false);
          if (fam) {
            CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, fam);
            CFRelease(fam);
          }
        }

        // Encode bold/italic as symbolic traits if requested.
        CTFontSymbolicTraits want = 0;
        switch (q.style) {
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
        if (want != 0) {
          CFMutableDictionaryRef traits = CFDictionaryCreateMutable(
              nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
          const SInt32 packed = static_cast<SInt32>(want);
          CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &packed);
          CFDictionarySetValue(traits, kCTFontSymbolicTrait, num);
          CFRelease(num);
          CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traits);
          CFRelease(traits);
        }

        CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
        CFRelease(attrs);
        if (!desc) return out;

        // Mandatory attributes for the matching pass: just the family name
        // when we asked for one. Empty `mandatory` means "best-effort match".
        CFMutableSetRef mandatory = CFSetCreateMutable(nullptr, 0, &kCFTypeSetCallBacks);
        if (!q.family.empty()) {
          CFSetAddValue(mandatory, kCTFontFamilyNameAttribute);
        }

        CFArrayRef matches = CTFontDescriptorCreateMatchingFontDescriptors(desc, mandatory);
        CFRelease(desc);
        CFRelease(mandatory);
        if (!matches) return out;

        const CFIndex n = CFArrayGetCount(matches);
        out.reserve(static_cast<usize>(n));
        for (CFIndex i = 0; i < n; ++i) {
          CTFontDescriptorRef match =
              static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(matches, i));
          if (!match) continue;
          Descriptor r;
          r.size_pt = q.size_pt;
          r.style = q.style;
          r.weight = q.weight;
          r.required_codepoints = q.required_codepoints;
          r.require_color = q.require_color;
          if (CFStringRef fam = static_cast<CFStringRef>(
                  CTFontDescriptorCopyAttribute(match, kCTFontFamilyNameAttribute))) {
            r.family = cf_string_to_utf8(fam);
            CFRelease(fam);
          }
          if (CFURLRef url = static_cast<CFURLRef>(
                  CTFontDescriptorCopyAttribute(match, kCTFontURLAttribute))) {
            UInt8 buf[1024] = {0};
            if (CFURLGetFileSystemRepresentation(url, true, buf, sizeof(buf))) {
              r.path = std::string{reinterpret_cast<const char*>(buf)};
            }
            CFRelease(url);
          }
          if (r.path) {
            out.push_back(std::move(r));
          }
        }
        CFRelease(matches);
        return out;
      }
    };

  } // namespace

  std::unique_ptr<Discover> make_coretext_discover() {
    return std::make_unique<CoreTextDiscover>();
  }

} // namespace fxe::font
