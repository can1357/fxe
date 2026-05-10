#include <fxe/crash.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  void test_defaults() {
    fxe::os::crash_options opts;
    CHECK(!opts.include_full_memory_dump);
    CHECK(opts.include_stack_memory);
    CHECK(opts.scrub_annotation_keys.empty());
  }

  void test_scrub_annotation_value() {
    auto dir = std::filesystem::temp_directory_path() / "fxe-crash-options-test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    CHECK(!ec);

    fxe::os::crash_options opts;
    opts.product_name = "fxe-test";
    opts.product_version = "1";
    opts.crash_dir = dir.string();
    opts.scrub_annotation_keys = {"token"};
    CHECK(fxe::os::crash_start(opts));
    CHECK(fxe::os::crash_detail::scrub_annotation_value("token", "secret") == "[redacted]");
    CHECK(fxe::os::crash_detail::scrub_annotation_value("safe", "visible") == "visible");

    std::filesystem::remove_all(dir, ec);
  }
} // namespace

int main() {
  test_defaults();
  test_scrub_annotation_value();
  return g_fail == 0 ? 0 : 1;
}
