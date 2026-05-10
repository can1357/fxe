#include <fxe/color.hpp>
#include <fxe/command_buffer.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>

#include <bit>
#include <cstdio>
#include <iostream>

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

  u64 fnv1a(const void* data, usize n) noexcept {
    constexpr u64 prime = 0x100000001b3ull;
    u64 h = 0xcbf29ce484222325ull;
    auto* p = static_cast<const u8*>(data);
    for (usize i = 0; i != n; ++i) {
      h ^= p[i];
      h *= prime;
    }
    return h;
  }

  u64 hash_command_buffer(const fxe::command_buffer& cb) noexcept {
    u64 h = fnv1a(cb.vertex_buffer.data(), cb.vertex_buffer.size() * sizeof(fxe::vertex));
    for (const auto& idx : cb.index_buffers) {
      u64 ih = fnv1a(idx.data(), idx.size() * sizeof(u32));
      h ^= ih + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    }
    return h;
  }
} // namespace

int main() {
  using namespace fxe;

  command_buffer cb;
  primitives::draw_shadow_rect(cb, 10.0f, 12.0f, 36.0f, 20.0f, 0.0f, r8g8b8a8{12, 24, 40, 96}, 5.0f,
                               3.0f, 4.0f, -2.0f, 128.0f, 96.0f);
  primitives::draw_shadow_rect_rounded(
      cb, 18.0f, 16.0f, 44.0f, 28.0f, primitives::optional_list<float, 4>{8.0f, 12.0f, 10.0f, 6.0f},
      0.1f, r8g8b8a8{20, 30, 60, 120}, 6.0f, 2.0f, -3.0f, 1.5f, 128.0f, 96.0f);
  primitives::draw_inner_shadow_rect_rounded(
      cb, 14.0f, 10.0f, 52.0f, 36.0f,
      primitives::optional_list<float, 4>{10.0f, 14.0f, 12.0f, 8.0f}, 0.2f,
      r8g8b8a8{8, 16, 32, 144}, 4.0f, 3.0f, 2.0f, -1.0f, 128.0f, 96.0f);

  const u64 hash = hash_command_buffer(cb);
  CHECK(!cb.vertex_buffer.empty());
  CHECK(!cb.index_buffers[0].empty());
  CHECK(std::bit_cast<texture_id>(cb.vertex_buffer[4].uv.z) == framebuffer_texture);
#if defined(__APPLE__)
  CHECK(hash == 0x5c2dc64966463808ull);
#endif
  std::printf("core-shadow-hash=0x%016llx vertices=%zu indices=%zu\n",
              static_cast<unsigned long long>(hash), cb.vertex_buffer.size(),
              cb.index_buffers[0].size());
  std::cout << "fxe core shadow tests: " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}
