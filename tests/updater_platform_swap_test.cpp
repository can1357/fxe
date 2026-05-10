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
} // namespace

int main() {
  namespace fs = std::filesystem;
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path tmp =
      fs::temp_directory_path() / ("fxe-updater-swap-test-" + std::to_string(nonce));
  fs::remove_all(tmp);
  fs::create_directories(tmp);

#if defined(__APPLE__)
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
  CHECK(argv.size() >= 3 && argv[2].find("while kill -0 ") != std::string::npos);
  CHECK(argv.size() >= 3 &&
        argv[2].find("2>/dev/null; do /bin/sleep 0.1; done;") != std::string::npos);
  CHECK(argv.size() >= 3 &&
        argv[2].find("'" + staged_canonical.string() + "'") != std::string::npos);
  CHECK(argv.size() >= 3 &&
        argv[2].find("'" + destination_canonical.string() + "'") != std::string::npos);
  fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
#else
  fxe::runtime::detail::set_platform_swap_destination_override_for_tests(std::nullopt);
  std::string error;
  CHECK(fxe::runtime::updater::perform_platform_swap("ignored.app", error, true));
  CHECK(error.empty());
  CHECK(fxe::runtime::detail::last_platform_swap_argv().empty());
#endif

  fs::remove_all(tmp);
  std::fprintf(stderr, "updater_platform_swap_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
