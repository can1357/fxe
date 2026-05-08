#include <fxe/offscreen.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>

#include <cstdint>
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

  std::vector<u8> render_split_scene(fxe::offscreen_renderer& renderer,
                                     const fxe::offscreen_options& opts, bool with_blur) {
    renderer.begin_frame();
    constexpr fxe::r8g8b8a8 left{240, 16, 16, 255};
    constexpr fxe::r8g8b8a8 right{16, 16, 240, 255};
    const float w = static_cast<float>(opts.width);
    const float h = static_cast<float>(opts.height);
    fxe::primitives::fill_rect(renderer, {0.0f, 0.0f}, {w * 0.5f, h}, 0.25f, left);
    fxe::primitives::fill_rect(renderer, {w * 0.5f, 0.0f}, {w * 0.5f, h}, 0.25f, right);
    if (with_blur) {
      fxe::primitives::blur_rect(renderer, {w * 0.375f, 0.0f}, {w * 0.25f, h}, 0.0f,
                                 fxe::r8g8b8a8{0, 0, 0, 0}, 4.0f, {w, h});
    }
    renderer.end_frame();
    return renderer.read_rgba8();
  }

  u32 rgb_abs_diff(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * width + x) * 4u;
    const u32 dr = a[i + 0] > b[i + 0] ? a[i + 0] - b[i + 0] : b[i + 0] - a[i + 0];
    const u32 dg = a[i + 1] > b[i + 1] ? a[i + 1] - b[i + 1] : b[i + 1] - a[i + 1];
    const u32 db = a[i + 2] > b[i + 2] ? a[i + 2] - b[i + 2] : b[i + 2] - a[i + 2];
    return dr + dg + db;
  }

  u64 region_rgb_abs_diff(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 x0,
                          u32 x1, u32 y0, u32 y1) {
    u64 sad = 0;
    for (u32 y = y0; y < y1; ++y) {
      for (u32 x = x0; x < x1; ++x)
        sad += rgb_abs_diff(a, b, width, x, y);
    }
    return sad;
  }

  u32 max_rgb_abs_diff(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 x0,
                       u32 x1, u32 y0, u32 y1) {
    u32 peak = 0;
    for (u32 y = y0; y < y1; ++y) {
      for (u32 x = x0; x < x1; ++x) {
        const u32 d = rgb_abs_diff(a, b, width, x, y);
        if (d > peak)
          peak = d;
      }
    }
    return peak;
  }

} // namespace

int main() {
#if FXE_HAS_WGPU
  try {
    fxe::offscreen_options opts{};
    opts.width = 32;
    opts.height = 16;
    opts.multisample = 1;
    opts.enable_depth = true;

    auto renderer = fxe::offscreen_renderer::create(opts);
    const std::vector<u8> base_pixels = render_split_scene(*renderer, opts, false);
    const std::vector<u8> blur_pixels = render_split_scene(*renderer, opts, true);
    CHECK(base_pixels.size() == opts.width * opts.height * 4u);
    CHECK(blur_pixels.size() == base_pixels.size());

    const u32 mid = opts.width / 2u;
    const u64 boundary_sad = region_rgb_abs_diff(base_pixels, blur_pixels, opts.width, mid - 2u,
                                                 mid + 2u, 0u, opts.height);
    const u64 edge_sad =
        region_rgb_abs_diff(base_pixels, blur_pixels, opts.width, 0u, 2u, 0u, opts.height) +
        region_rgb_abs_diff(base_pixels, blur_pixels, opts.width, opts.width - 2u, opts.width, 0u,
                            opts.height);
    const u32 boundary_peak =
        max_rgb_abs_diff(base_pixels, blur_pixels, opts.width, mid - 2u, mid + 2u, 0u, opts.height);

    CHECK(boundary_sad > 2500u);
    CHECK(boundary_peak > 24u);
    CHECK(boundary_sad > edge_sad);
    for (usize i = 0; i + 3 < blur_pixels.size(); i += 4)
      CHECK(blur_pixels[i + 3] >= 240);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "wgpu blur smoke skipped: %s\n", e.what());
    return 0;
  }
#else
  std::fprintf(stderr, "wgpu blur smoke skipped: FXE_HAS_WGPU=0\n");
#endif
  return g_fail == 0 ? 0 : 1;
}
