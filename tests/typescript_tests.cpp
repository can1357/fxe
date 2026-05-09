#include "../src/audio/audio.hpp"
#include <fxe/v8_host.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifndef FXE_V8_ICUDTL_PATH
#define FXE_V8_ICUDTL_PATH ""
#endif

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

  void capture_console(void* user, std::string_view, std::string_view text) {
    auto* out = static_cast<std::string*>(user);
    out->append(text);
    out->push_back('\n');
  }

  std::string_view basename(std::string_view path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
  }

  void run_source_map_stack_test(fxe::js::host& host) {
    namespace fs = std::filesystem;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path path =
        fs::temp_directory_path() / ("fxe_source_map_stack_" + std::to_string(suffix) + ".ts");
    {
      std::ofstream out(path, std::ios::binary);
      out << "const value: number = 41;\n"
             "function explode(): never {\n"
             "  throw new Error('source-map-stack-sentinel ' + value);\n"
             "}\n"
             "explode();\n";
    }

    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec).lexically_normal().string();
    const std::string expected_frame = (ec ? path.lexically_normal().string() : canonical) + ":3:";

    auto result = host.run_module_file(path);
    CHECK(!result.ok);
    if (result.ok) {
      std::fprintf(stderr, "source-map stack test unexpectedly succeeded\n");
    } else {
      CHECK(result.message.find("source-map-stack-sentinel") != std::string::npos);
      CHECK(result.message.find(expected_frame) != std::string::npos);
      if (result.message.find(expected_frame) == std::string::npos)
        std::fprintf(stderr, "%s\n", result.message.c_str());
    }
    fs::remove(path, ec);
  }
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(
        stderr,
        "usage: %s <typescript-smoke.ts> [<typescript-modules-smoke.ts>] [<module-test.ts>...]\n",
        argv[0]);
    return 2;
  }

  fxe::js::initialize(argv[0], FXE_V8_ICUDTL_PATH);
  std::string console_buffer;
  {
    fxe::js::host host;
    host.set_console_sink(capture_console, &console_buffer);

    const size_t smoke_mark = console_buffer.size();
    auto result = host.run_file(argv[1]);
    CHECK(result.ok);
    if (!result.ok)
      std::fprintf(stderr, "%s\n", result.message.c_str());
    CHECK(std::string_view(console_buffer).substr(smoke_mark).find("ts-smoke=7:6") !=
          std::string_view::npos);

    if (argc >= 3) {
      const size_t modules_mark = console_buffer.size();
      result = host.run_module_file(argv[2]);
      CHECK(result.ok);
      if (!result.ok)
        std::fprintf(stderr, "%s\n", result.message.c_str());
      CHECK(std::string_view(console_buffer)
                .substr(modules_mark)
                .find("ts-modules=hi fxe-modules-aux|main=true|file=true") !=
            std::string_view::npos);
    }

    run_source_map_stack_test(host);

    for (int i = 3; i < argc; ++i) {
      const size_t module_mark = console_buffer.size();
      result = host.run_module_file(argv[i]);
      CHECK(result.ok);
      if (!result.ok)
        std::fprintf(stderr, "%s\n", result.message.c_str());
      if (basename(argv[i]) == "bind_all_tests.ts") {
        CHECK(std::string_view(console_buffer).substr(module_mark).find("bind-tests=") !=
              std::string_view::npos);
        fxe::audio::shutdown();
        fxe::js::shutdown();
        std::cout << "fxe TypeScript tests: " << g_pass << " passed, " << g_fail << " failed\n";
        std::exit(g_fail == 0 ? 0 : 1);
      }
    }
  }

  fxe::audio::shutdown();
  fxe::js::shutdown();

  std::cout << "fxe TypeScript tests: " << g_pass << " passed, " << g_fail << " failed\n";
  std::exit(g_fail == 0 ? 0 : 1);
}
