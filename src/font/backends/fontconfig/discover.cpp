// Fontconfig-backed discovery (Linux). Translates a Descriptor into an
// FcPattern, asks fontconfig for the closest matches, and returns
// concrete on-disk paths.

#include <fxe/font/discover.hpp>

#include <fontconfig/fontconfig.h>

#include <memory>
#include <string>
#include <vector>

namespace fxe::font {
  namespace {

    class FontconfigDiscover final : public Discover {
    public:
      FontconfigDiscover() {
        // FcInit is idempotent and process-wide.
        FcInit();
      }
      [[nodiscard]] std::vector<Descriptor> find(const Descriptor& q) override {
        std::vector<Descriptor> out;
        FcPattern* pat = FcPatternCreate();
        if (!q.family.empty()) {
          FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(q.family.c_str()));
        }
        if (q.weight) {
          int fc_weight = FC_WEIGHT_REGULAR;
          if (*q.weight >= 700)
            fc_weight = FC_WEIGHT_BOLD;
          else if (*q.weight >= 500)
            fc_weight = FC_WEIGHT_MEDIUM;
          else if (*q.weight <= 300)
            fc_weight = FC_WEIGHT_LIGHT;
          FcPatternAddInteger(pat, FC_WEIGHT, fc_weight);
        }
        switch (q.style) {
        case Style::italic:
        case Style::bold_italic:
          FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
          break;
        default:
          FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
          break;
        }
        if (q.style == Style::bold || q.style == Style::bold_italic) {
          FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
        }
        if (q.require_color) {
          FcPatternAddBool(pat, FC_COLOR, FcTrue);
        }
        if (!q.required_codepoints.empty()) {
          FcCharSet* cs = FcCharSetCreate();
          for (char32_t cp : q.required_codepoints) {
            FcCharSetAddChar(cs, static_cast<FcChar32>(cp));
          }
          FcPatternAddCharSet(pat, FC_CHARSET, cs);
          FcCharSetDestroy(cs);
        }
        FcConfigSubstitute(nullptr, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult res = FcResultMatch;
        FcFontSet* fs = FcFontSort(nullptr, pat, FcTrue, nullptr, &res);
        if (fs) {
          out.reserve(static_cast<std::size_t>(fs->nfont));
          for (int i = 0; i < fs->nfont; ++i) {
            FcPattern* m = fs->fonts[i];
            FcChar8* path = nullptr;
            if (FcPatternGetString(m, FC_FILE, 0, &path) == FcResultMatch && path) {
              Descriptor r;
              r.family = q.family;
              r.style = q.style;
              r.size_pt = q.size_pt;
              r.weight = q.weight;
              r.required_codepoints = q.required_codepoints;
              r.require_color = q.require_color;
              r.path = std::string{reinterpret_cast<const char*>(path)};
              int idx = 0;
              if (FcPatternGetInteger(m, FC_INDEX, 0, &idx) == FcResultMatch) {
                r.face_index = static_cast<std::uint32_t>(idx);
              }
              out.push_back(std::move(r));
            }
          }
          FcFontSetDestroy(fs);
        }
        FcPatternDestroy(pat);
        return out;
      }
    };

  } // namespace

  std::unique_ptr<Discover> make_fontconfig_discover() {
    return std::make_unique<FontconfigDiscover>();
  }

} // namespace fxe::font
