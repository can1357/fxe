#include "runtime/updater.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }

  bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
  }
} // namespace

int main() {
  namespace fs = std::filesystem;
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path tmp =
      fs::temp_directory_path() / ("fxe-updater-swap-test-" + std::to_string(nonce));
  fs::remove_all(tmp);
  fs::create_directories(tmp);

#if defined(__APPLE__)
  {
    const fs::path staged = tmp / "staged" / "FXETest.app";
    const fs::path destination = tmp / "Applications" / "FXETest.app";
    fs::create_directories(staged / "Contents" / "MacOS");
    fs::create_directories(destination / "Contents" / "MacOS");
    write_text(staged / "Contents" / "MacOS" / "FXETest", "#!/bin/sh\nexit 0\n");
    write_text(destination / "Contents" / "MacOS" / "FXETest", "#!/bin/sh\nexit 0\n");

    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(destination);
    std::string error;
    CHECK(fxe::runtime::updater::perform_platform_swap(staged.string(), error, true));
    CHECK(error.empty());

    const auto& argv = fxe::runtime::detail::last_platform_swap_argv();
    const fs::path staged_canonical = fs::weakly_canonical(staged);
    const fs::path destination_canonical = fs::weakly_canonical(destination);
    CHECK(argv.size() == 3);
    CHECK(argv.size() >= 1 && argv[0] == "/bin/sh");
    CHECK(argv.size() >= 2 && argv[1] == "-c");
    CHECK(argv.size() >= 3 && contains(argv[2], "while kill -0 "));
    CHECK(argv.size() >= 3 && contains(argv[2], "2>/dev/null; do /bin/sleep 0.1; done;"));
    CHECK(argv.size() >= 3 && contains(argv[2], "'" + staged_canonical.string() + "'"));
    CHECK(argv.size() >= 3 && contains(argv[2], "'" + destination_canonical.string() + "'"));
    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
  }
#else
  fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
  std::string error;
  CHECK(fxe::runtime::updater::perform_platform_swap("ignored.app", error, true));
  CHECK(error.empty());
  CHECK(fxe::runtime::detail::last_platform_swap_argv().empty());
#endif

  {
    const fs::path staged = tmp / "staged" / "Foo.AppImage";
    const fs::path destination = tmp / "bin" / "Foo.AppImage";
    fs::create_directories(staged.parent_path());
    fs::create_directories(destination.parent_path());
    write_text(staged, "appimage");
    write_text(destination, "current");

    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(destination);
    std::string error;
    CHECK(fxe::runtime::updater::perform_platform_swap(staged.string(), error, true));
    CHECK(error.empty());

    const auto& argv = fxe::runtime::detail::last_platform_swap_argv();
    const fs::path staged_canonical = fs::weakly_canonical(staged);
    const fs::path destination_canonical = fs::weakly_canonical(destination);
    CHECK(argv.size() == 3);
    CHECK(argv.size() >= 1 && argv[0] == "/bin/sh");
    CHECK(argv.size() >= 2 && argv[1] == "-c");
    CHECK(argv.size() >= 3 && contains(argv[2], "while kill -0 "));
    CHECK(argv.size() >= 3 && contains(argv[2], "/bin/mv -f"));
    CHECK(argv.size() >= 3 && contains(argv[2], "/bin/chmod +x"));
    CHECK(argv.size() >= 3 && contains(argv[2], "exec '" + destination_canonical.string() + "'"));
    CHECK(argv.size() >= 3 && contains(argv[2], "'" + staged_canonical.string() + "'"));
    CHECK(argv.size() >= 3 && contains(argv[2], "'" + destination_canonical.string() + "'"));
    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
  }

  {
    const fs::path staged = tmp / "staged" / "Foo.msi";
    fs::create_directories(staged.parent_path());
    write_text(staged, "msi");

    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
    std::string error;
    CHECK(fxe::runtime::updater::perform_platform_swap(staged.string(), error, true));
    CHECK(error.empty());

    const auto& argv = fxe::runtime::detail::last_platform_swap_argv();
    const fs::path staged_canonical = fs::weakly_canonical(staged);
    CHECK(argv.size() == 7);
    CHECK(argv.size() >= 1 && argv[0] == "msiexec.exe");
    CHECK(argv.size() >= 2 && argv[1] == "/i");
    CHECK(argv.size() >= 3 && argv[2] == staged_canonical.string());
    CHECK(argv.size() >= 4 && argv[3] == "/qn");
    CHECK(argv.size() >= 5 && argv[4] == "/norestart");
    CHECK(argv.size() >= 6 && argv[5] == "/L*v");
  }

  {
    const fs::path staged = tmp / "staged" / "Foo.exe";
    const fs::path destination = tmp / "bin" / "Foo.exe";
    fs::create_directories(staged.parent_path());
    fs::create_directories(destination.parent_path());
    write_text(staged, "next");
    write_text(destination, "current");

    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(destination);
    std::string error;
    CHECK(fxe::runtime::updater::perform_platform_swap(staged.string(), error, true));
    CHECK(error.empty());

    const auto& argv = fxe::runtime::detail::last_platform_swap_argv();
    const fs::path staged_canonical = fs::weakly_canonical(staged);
    const fs::path destination_canonical = fs::weakly_canonical(destination);
    CHECK(argv.size() == 3);
    CHECK(argv.size() >= 1 && argv[0] == "cmd.exe");
    CHECK(argv.size() >= 2 && argv[1] == "/c");
    CHECK(argv.size() >= 3 && contains(argv[2], "tasklist /FI \"PID eq %pid%\""));
    CHECK(argv.size() >= 3 && contains(argv[2], "timeout /t 1 /nobreak >NUL"));
    CHECK(argv.size() >= 3 && contains(argv[2], "move /Y"));
    CHECK(argv.size() >= 3 && contains(argv[2], "start \"\""));
    CHECK(argv.size() >= 3 && contains(argv[2], "\"" + staged_canonical.string() + "\""));
    CHECK(argv.size() >= 3 && contains(argv[2], "\"" + destination_canonical.string() + "\""));
    fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "updater_platform_swap_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
