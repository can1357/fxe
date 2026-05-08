#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

#if !defined(__APPLE__)
int main() {
  return EXIT_SUCCESS;
}
#else
namespace {

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  struct temp_dir_guard {
    fs::path path;
    ~temp_dir_guard() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  std::string shell_quote(const fs::path& path) {
    std::string s = path.string();
    std::string out = "'";
    for (char c : s) {
      if (c == '\'')
        out += "'\\''";
      else
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
  }

  fs::path make_temp_dir() {
    std::mt19937_64 rng(std::random_device{}());
    for (int attempt = 0; attempt != 32; ++attempt) {
      fs::path candidate =
          fs::temp_directory_path() / ("fxe-pack-webauthn-" + std::to_string(rng()));
      std::error_code ec;
      if (fs::create_directories(candidate, ec))
        return candidate;
    }
    std::fprintf(stderr, "failed to create temp directory\n");
    std::exit(EXIT_FAILURE);
  }

  void write_file(const fs::path& path, std::string_view body) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      std::fprintf(stderr, "failed to write %s\n", path.string().c_str());
      std::exit(EXIT_FAILURE);
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }

  std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "failed to read %s\n", path.string().c_str());
      std::exit(EXIT_FAILURE);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  bool run_pack(const fs::path& fxe_pack, const fs::path& fxe_run, const fs::path& entry,
                const fs::path& out, const std::vector<std::string>& extra_args) {
    std::string command = "FXE_RUN=" + shell_quote(fxe_run) + " " + shell_quote(fxe_pack) + " " +
                          shell_quote(entry) + " --platform macos --out " + shell_quote(out);
    for (const auto& arg : extra_args)
      command += " " + shell_quote(fs::path(arg));
    return std::system(command.c_str()) == 0;
  }

} // namespace

int main() {
  const char* fxe_pack_env = std::getenv("FXE_PACK");
  const char* fxe_run_env = std::getenv("FXE_RUN");
  CHECK(fxe_pack_env != nullptr);
  CHECK(fxe_run_env != nullptr);
  if (!fxe_pack_env || !fxe_run_env)
    return EXIT_FAILURE;

  temp_dir_guard temp{make_temp_dir()};
  const fs::path fxe_pack = fs::path(fxe_pack_env);
  const fs::path fxe_run = fs::path(fxe_run_env);
  const fs::path entry = temp.path / "entry.ts";
  write_file(entry, "console.log('hi');\n");

  const fs::path dev_app = temp.path / "Test.app";
  CHECK(run_pack(fxe_pack, fxe_run, entry, dev_app,
                 {"--webauthn-rp-id", "one.example", "--webauthn-rp-id", "two.example",
                  "--webauthn-mode", "developer"}));
  const fs::path dev_entitlements = dev_app / "Contents" / "entitlements.plist";
  CHECK(fs::exists(dev_entitlements));
  if (fs::exists(dev_entitlements)) {
    const std::string entitlements = slurp(dev_entitlements);
    CHECK(entitlements.find("webcredentials:one.example?mode=developer") != std::string::npos);
    CHECK(entitlements.find("webcredentials:two.example?mode=developer") != std::string::npos);
  }

  const fs::path prod_app = temp.path / "TestProd.app";
  CHECK(run_pack(fxe_pack, fxe_run, entry, prod_app,
                 {"--webauthn-rp-id", "one.example", "--webauthn-rp-id", "two.example",
                  "--webauthn-mode", "production"}));
  const fs::path prod_entitlements = prod_app / "Contents" / "entitlements.plist";
  CHECK(fs::exists(prod_entitlements));
  if (fs::exists(prod_entitlements)) {
    const std::string entitlements = slurp(prod_entitlements);
    CHECK(entitlements.find("webcredentials:one.example</string>") != std::string::npos);
    CHECK(entitlements.find("webcredentials:two.example</string>") != std::string::npos);
    CHECK(entitlements.find("?mode=developer") == std::string::npos);
  }

  const fs::path none_app = temp.path / "TestNone.app";
  CHECK(run_pack(fxe_pack, fxe_run, entry, none_app, {}));
  CHECK(!fs::exists(none_app / "Contents" / "entitlements.plist"));

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
