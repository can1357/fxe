#include <cstdio>
#include <cstdlib>
#include <string>

#include "../tools/fxe-pack/cli_detail.hpp"

namespace {

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

} // namespace

int main() {
  using fxe_pack::cli_detail::installer_value;
  using fxe_pack::cli_detail::InstallerFormat;
  using fxe_pack::cli_detail::map_entitlement_shorthand;
  using fxe_pack::cli_detail::parse_installer_value;

  CHECK(map_entitlement_shorthand("camera") == "com.apple.security.device.camera");
  CHECK(map_entitlement_shorthand("unknown-foo").empty());
  CHECK(parse_installer_value("pkg") == InstallerFormat::Pkg);
  CHECK(installer_value(InstallerFormat::Pkg) == "pkg");

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
