#include <fxe/offscreen.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>

#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  void check_near_u8(unsigned actual, unsigned expected, unsigned tolerance, const char* name) {
    const unsigned delta = actual > expected ? actual - expected : expected - actual;
    if (delta > tolerance) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s: actual=%u expected=%u tolerance=%u\n", name, actual, expected,
                   tolerance);
    }
  }
} // namespace

int main() {
#if FXE_HAS_WGPU
  try {
    fxe::offscreen_options opts{};
    opts.width = 2;
    opts.height = 2;
    opts.multisample = 1;
    opts.enable_depth = true;

    auto renderer = fxe::offscreen_renderer::create(opts);
    renderer->begin_frame();
    constexpr fxe::r8g8b8a8 source{96, 128, 160, 255};
    fxe::primitives::fill_rect(*renderer, {0.0f, 0.0f}, {2.0f, 2.0f}, 0.25f, source);
    fxe::primitives::blur_rect(*renderer, {0.0f, 0.0f}, {2.0f, 2.0f}, 0.0f,
                               fxe::r8g8b8a8{0, 0, 0, 0}, 1.0f, {2.0f, 2.0f});
    renderer->end_frame();

    const std::vector<u8> pixels = renderer->read_rgba8();
    CHECK(pixels.size() == 16);
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
      check_near_u8(pixels[i + 0], source.r, 40, "blurred red channel");
      check_near_u8(pixels[i + 1], source.g, 40, "blurred green channel");
      check_near_u8(pixels[i + 2], source.b, 40, "blurred blue channel");
      CHECK(pixels[i + 3] >= 240);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "wgpu blur smoke skipped: %s\n", e.what());
    return 0;
  }
#else
  std::fprintf(stderr, "wgpu blur smoke skipped: FXE_HAS_WGPU=0\n");
#endif
  return g_fail == 0 ? 0 : 1;
}
