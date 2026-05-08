#include <fxe/offscreen.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>

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

} // namespace

int main() {
#if FXE_HAS_WGPU
  try {
    fxe::offscreen_options opts{};
    opts.width = 8;
    opts.height = 4;
    opts.multisample = 1;
    opts.enable_depth = true;

    auto renderer = fxe::offscreen_renderer::create(opts);
    renderer->begin_frame();
    constexpr fxe::r8g8b8a8 left{240, 16, 16, 255};
    constexpr fxe::r8g8b8a8 right{16, 16, 240, 255};
    fxe::primitives::fill_rect(*renderer, {0.0f, 0.0f}, {4.0f, 4.0f}, 0.25f, left);
    fxe::primitives::fill_rect(*renderer, {4.0f, 0.0f}, {4.0f, 4.0f}, 0.25f, right);
    fxe::primitives::blur_rect(*renderer, {0.0f, 0.0f}, {8.0f, 4.0f}, 0.0f,
                               fxe::r8g8b8a8{0, 0, 0, 0}, 2.0f, {8.0f, 4.0f});
    renderer->end_frame();

    const std::vector<u8> pixels = renderer->read_rgba8();
    CHECK(pixels.size() == 8u * 4u * 4u);
    const auto px = [&](u32 x, u32 y, u32 c) -> unsigned {
      return pixels[(static_cast<usize>(y) * opts.width + x) * 4u + c];
    };
    CHECK(px(3, 2, 0) > 20);
    CHECK(px(3, 2, 2) > 20);
    CHECK(px(4, 2, 0) > 20);
    CHECK(px(4, 2, 2) > 20);
    CHECK(px(1, 2, 0) != px(6, 2, 0));
    for (usize i = 0; i + 3 < pixels.size(); i += 4)
      CHECK(pixels[i + 3] >= 240);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "wgpu blur smoke skipped: %s\n", e.what());
    return 0;
  }
#else
  std::fprintf(stderr, "wgpu blur smoke skipped: FXE_HAS_WGPU=0\n");
#endif
  return g_fail == 0 ? 0 : 1;
}
