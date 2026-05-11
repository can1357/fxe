#include "os/os.hpp"

#include <cstdio>
#include <string_view>

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
  const char* backend = fxe::os::ime_backend();
  const std::string_view value = backend ? std::string_view(backend) : std::string_view{};
  CHECK(!value.empty());
  CHECK(value == "imm32" || value == "tsf" || value == "ibus" || value == "fcitx" ||
        value == "none");
  return g_fail == 0 ? 0 : 1;
}
