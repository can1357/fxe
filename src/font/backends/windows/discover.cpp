// Windows directory-scan discovery. Scans %WINDIR%\Fonts and the per-user
// fonts directory for files whose family name matches the descriptor.
// Loosely follows Ghostty's `freetype_windows` strategy.

#include <fxe/font/discover.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fxe::font {
  namespace {

    std::vector<std::filesystem::path> font_dirs() {
      std::vector<std::filesystem::path> out;
#ifdef _WIN32
      char buf[MAX_PATH] = {0};
      if (GetWindowsDirectoryA(buf, MAX_PATH) > 0) {
        out.emplace_back(std::filesystem::path{buf} / "Fonts");
      }
      if (const char* local = std::getenv("LOCALAPPDATA")) {
        out.emplace_back(std::filesystem::path{local} / "Microsoft" / "Windows" / "Fonts");
      }
#endif
      return out;
    }

    std::string lowered(std::string s) {
      for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    class WindowsDiscover final : public Discover {
    public:
      [[nodiscard]] std::vector<Descriptor> find(const Descriptor& q) override {
        std::vector<Descriptor> out;
        const std::string family_lc = lowered(q.family);
        for (const auto& d : font_dirs()) {
          std::error_code ec;
          if (!std::filesystem::exists(d, ec))
            continue;
          for (const auto& entry : std::filesystem::directory_iterator(d, ec)) {
            if (!entry.is_regular_file())
              continue;
            const auto& p = entry.path();
            const auto ext = lowered(p.extension().string());
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc")
              continue;
            const std::string stem_lc = lowered(p.stem().string());
            if (!family_lc.empty() && stem_lc.find(family_lc) == std::string::npos)
              continue;
            Descriptor r;
            r.family = q.family;
            r.style = q.style;
            r.size_pt = q.size_pt;
            r.weight = q.weight;
            r.required_codepoints = q.required_codepoints;
            r.require_color = q.require_color;
            r.path = p.string();
            out.push_back(std::move(r));
          }
        }
        return out;
      }
    };

  } // namespace

  std::unique_ptr<Discover> make_win32_discover() {
    return std::make_unique<WindowsDiscover>();
  }

} // namespace fxe::font
