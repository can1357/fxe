#include "runtime/updater.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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
  const fs::path tmp =
      fs::temp_directory_path() /
      ("fxe-updater-linux-signature-test-" + std::to_string(static_cast<long long>(getpid())));
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  const fs::path minisign_artifact = tmp / "app.bin";
  write_text(minisign_artifact, "payload");
  write_text(
      minisign_artifact.string() + ".minisig",
      "untrusted comment: release signer\n"
      "RWQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
      "trusted comment: timestamp:0\n"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\n");

  fs::path sig_path;
  std::string flavor;
  CHECK(fxe::runtime::detail::detect_detached_signature(minisign_artifact, sig_path, flavor));
  CHECK(sig_path == minisign_artifact.string() + ".minisig");
  CHECK(flavor == "minisign");

  const fs::path gpg_artifact = tmp / "pkg.tar";
  write_text(gpg_artifact, "payload");
  write_text(gpg_artifact.string() + ".asc",
             "-----BEGIN PGP SIGNATURE-----\n-----END PGP SIGNATURE-----\n");

  sig_path.clear();
  flavor.clear();
  CHECK(fxe::runtime::detail::detect_detached_signature(gpg_artifact, sig_path, flavor));
  CHECK(sig_path == gpg_artifact.string() + ".asc");
  CHECK(flavor == "gpg");

  const fs::path unsigned_artifact = tmp / "unsigned.bin";
  write_text(unsigned_artifact, "payload");
  sig_path.clear();
  flavor.clear();
  CHECK(!fxe::runtime::detail::detect_detached_signature(unsigned_artifact, sig_path, flavor));
  CHECK(sig_path.empty());
  CHECK(flavor.empty());

  std::string error;
  CHECK(fxe::runtime::detail::verify_platform_code_signature(minisign_artifact, "", "", error));
  CHECK(error.empty());

  error.clear();
  CHECK(!fxe::runtime::detail::verify_platform_code_signature(
      gpg_artifact, "0123456789abcdef0123456789abcdef01234567", (tmp / "missing.pub").string(),
      error));
  CHECK(!error.empty());

  fs::remove_all(tmp);
  std::fprintf(stderr, "updater_linux_signature_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
