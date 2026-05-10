#include <fxe/log.hpp>
#include <fxe/system_fonts.hpp>
#include <fxe/types.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
namespace {
  struct font_attempt {
    std::filesystem::path path;
    std::string reason;
  };

  static bool g_logged = false;

  std::string errno_message(const char* fallback) {
    return errno != 0 ? std::strerror(errno) : fallback;
  }

  void log_font_attempts_once(const std::vector<font_attempt>& attempts) {
    if (g_logged)
      return;
    g_logged = true;

    if (attempts.empty()) {
      FXE_ERROR("font.discovery", "failed to load a system font; (no candidates)");
      return;
    }
    std::string detail;
    for (const auto& attempt : attempts) {
      detail += "\n  ";
      detail += attempt.path.string();
      detail += ": ";
      detail += attempt.reason;
    }
    FXE_ERROR("font.discovery", "failed to load a system font; attempted paths:{}", detail);
  }
} // namespace
namespace fxe {
  std::vector<std::filesystem::path> system_font_paths() {
    namespace fs = std::filesystem;
    std::vector<fs::path> out;
#if defined(__APPLE__)
    // Modern macOS (Sonoma+). SFNS is the system font. Helvetica/Menlo are
    // .ttc collections — stb_truetype reads index 0 by default.
    out.emplace_back("/System/Library/Fonts/SFNS.ttf");
    out.emplace_back("/System/Library/Fonts/SFNSMono.ttf");
    out.emplace_back("/System/Library/Fonts/SFNSRounded.ttf");
    out.emplace_back("/System/Library/Fonts/Helvetica.ttc");
    out.emplace_back("/System/Library/Fonts/HelveticaNeue.ttc");
    out.emplace_back("/System/Library/Fonts/Menlo.ttc");
    out.emplace_back("/System/Library/Fonts/Geneva.ttf");
    out.emplace_back("/System/Library/Fonts/Supplemental/Arial.ttf");
    out.emplace_back("/Library/Fonts/Arial Unicode.ttf");
#elif defined(_WIN32)
    out.emplace_back("C:\\Windows\\Fonts\\segoeui.ttf");
    out.emplace_back("C:\\Windows\\Fonts\\arial.ttf");
    out.emplace_back("C:\\Windows\\Fonts\\tahoma.ttf");
    out.emplace_back("C:\\Windows\\Fonts\\verdana.ttf");
    out.emplace_back("C:\\Windows\\Fonts\\consola.ttf");
#else
    // Common Linux distro paths.
    out.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    out.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
    out.emplace_back("/usr/share/fonts/dejavu/DejaVuSans.ttf");
    out.emplace_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    out.emplace_back("/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    out.emplace_back("/usr/share/fonts/google-noto/NotoSans-Regular.ttf");
    out.emplace_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    out.emplace_back("/usr/share/fonts/TTF/Hack-Regular.ttf");
#endif
    return out;
  }

  std::optional<std::vector<u8>>
  try_load_first_available(std::span<const std::filesystem::path> candidates,
                           std::filesystem::path* loaded_path) {
    namespace fs = std::filesystem;
    std::vector<font_attempt> attempts;
    attempts.reserve(candidates.size());
    for (const auto& p : candidates) {
      std::error_code ec;
      const bool exists = fs::exists(p, ec);
      if (ec) {
        attempts.push_back({p, "stat() failed: " + ec.message()});
        continue;
      }
      if (!exists) {
        attempts.push_back({p, "stat() failed: file does not exist"});
        continue;
      }
      const bool regular = fs::is_regular_file(p, ec);
      if (ec) {
        attempts.push_back({p, "stat() failed: " + ec.message()});
        continue;
      }
      if (!regular) {
        attempts.push_back({p, "stat() failed: not a regular file"});
        continue;
      }

      errno = 0;
      std::ifstream in(p, std::ios::binary | std::ios::ate);
      if (!in) {
        attempts.push_back({p, "open() failed: " + errno_message("unknown error")});
        continue;
      }
      auto sz = in.tellg();
      if (sz <= 0) {
        attempts.push_back({p, "parse failed: empty file"});
        continue;
      }
      in.seekg(0);
      if (!in) {
        attempts.push_back({p, "parse failed: seek failed"});
        continue;
      }
      std::vector<u8> buf(static_cast<usize>(sz));
      if (!in.read(reinterpret_cast<char*>(buf.data()), sz)) {
        attempts.push_back({p, "parse failed: read failed"});
        continue;
      }
      if (loaded_path)
        *loaded_path = p;
      return buf;
    }
    log_font_attempts_once(attempts);
    return std::nullopt;
  }

  std::optional<std::vector<u8>> load_default_system_font(std::filesystem::path* loaded_path) {
    auto candidates = system_font_paths();
    return try_load_first_available(candidates, loaded_path);
  }
} // namespace fxe
