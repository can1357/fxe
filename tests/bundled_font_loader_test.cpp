#include "../src/runtime/bundle_loader.hpp"
#include "../src/runtime/fxa_archive.hpp"

#include <fxe/font/embedded_nerd.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using fxe::runtime::fxa_archive::BundledFont;
  using fxe::runtime::fxa_archive::ManifestMetadata;
  using fxe::runtime::fxa_archive::PackOptions;

  i32 g_pass = 0;
  i32 g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
      return;
    }
    ++g_fail;
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  void write_bytes(const fs::path& path, std::span<const u8> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }

  std::vector<u8> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void set_allow_unsigned_env(bool enabled) {
#ifdef _WIN32
    _putenv_s("FXE_BUNDLE_ALLOW_UNSIGNED", enabled ? "1" : "");
#else
    if (enabled)
      setenv("FXE_BUNDLE_ALLOW_UNSIGNED", "1", 1);
    else
      unsetenv("FXE_BUNDLE_ALLOW_UNSIGNED");
#endif
  }

  std::string shell_quote(std::string_view text) {
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : text) {
      if (ch == '"')
        out += "\\\"";
      else
        out.push_back(ch);
    }
    out += '"';
    return out;
#else
    std::string out = "'";
    for (char ch : text) {
      if (ch == '\'')
        out += "'\\''";
      else
        out.push_back(ch);
    }
    out += '\'';
    return out;
#endif
  }

  fs::path make_archive(const fs::path& dir) {
    fs::create_directories(dir);
    const fs::path host = dir / "bundled-font-host.bin";
    const fs::path font_file = dir / "embedded-nerd.ttf";
    std::ofstream(host, std::ios::binary | std::ios::trunc) << "HOST";
    const auto bytes = fxe::font::embedded_nerd_font_bytes();
    CHECK(!bytes.empty());
    write_bytes(font_file, bytes);

    ManifestMetadata manifest;
    manifest.app_name = "bundled-font-test";
    manifest.version = "1.0.0";
    manifest.entry = "main.js";
    manifest.created_at = "2026-05-10T23:13:42Z";
    manifest.fonts.push_back(BundledFont{
        .family = "Inter",
        .virtual_path = "fonts/inter-400-normal.ttf",
        .weight = 400,
        .style = "normal",
    });

    PackOptions opts;
    opts.compress = false;

    std::string error;
    const bool packed = fxe::runtime::fxa_archive::pack_files(
        host.string(), {{font_file.string(), manifest.fonts.front().virtual_path}}, manifest, opts,
        &error);
    CHECK(packed);
    CHECK(error.empty());
    return host;
  }

  i32 run_child_case(const char* archive_path, const char* source_font_path) {
    set_allow_unsigned_env(true);
    const bool mounted = fxe::runtime::mount_bundle_from_argv0(archive_path);
    set_allow_unsigned_env(false);
    CHECK(mounted);
    CHECK(fxe::runtime::bundle_mounted());
    const auto fonts = fxe::runtime::bundle_fonts();
    CHECK(fonts.size() == 1u);
    if (fonts.size() != 1u)
      return 1;
    CHECK(fonts.front().family == "Inter");
    CHECK(fonts.front().virtual_path == "fonts/inter-400-normal.ttf");
    CHECK(fonts.front().weight == 400u);
    CHECK(fonts.front().style == "normal");

    const auto resolved = fxe::runtime::resolve_bundled_font("Inter", 400, "normal");
    CHECK(resolved.has_value());
    const auto resolved_casefold = fxe::runtime::resolve_bundled_font("inter", 400, "normal");
    CHECK(resolved_casefold.has_value());
    if (!resolved || !resolved_casefold)
      return 1;
    CHECK(resolved->virtual_name == fonts.front().virtual_path);
    CHECK(resolved_casefold->virtual_name == fonts.front().virtual_path);
    const auto source_bytes = read_bytes(source_font_path);
    CHECK(resolved->bytes_view.size() == source_bytes.size());
    CHECK(std::equal(resolved->bytes_view.begin(), resolved->bytes_view.end(), source_bytes.begin(),
                     source_bytes.end()));
    return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  i32 run_parent_case(const char* self) {
    const fs::path tmp = fs::temp_directory_path() / "fxe-bundled-font-loader-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    const fs::path archive = make_archive(tmp);
    const fs::path source_font = tmp / "embedded-nerd.ttf";
    std::string command = shell_quote(self);
    command += " --bundle-font-case ";
    command += shell_quote(archive.string());
    command += " ";
    command += shell_quote(source_font.string());
    const int rc = std::system(command.c_str());
    fs::remove_all(tmp);
    return rc;
  }
} // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--bundle-font-case")
    return run_child_case(argv[2], argv[3]);

  const int rc = run_parent_case(argv[0]);
  std::fprintf(stderr, "bundled_font_loader_test: %d passed, %d failed\n", g_pass, g_fail);
  return rc;
}
