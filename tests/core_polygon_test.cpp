#include <fxe/color.hpp>
#include <fxe/command_buffer.hpp>
#include <fxe/log.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>

#include <array>

namespace {
  int g_fail = 0;

  void check(bool ok, const char* expr, int line) {
    if (!ok) {
      ++g_fail;
      FXE_ERROR("test.core_polygon", "check failed at line {}: {}", line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __LINE__)

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
      const u64 ih = fnv1a(idx.data(), idx.size() * sizeof(u32));
      h ^= ih + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    }
    return h;
  }
} // namespace

int main() {
  using namespace fxe;
  using namespace fxe::primitives;

  command_buffer cb;
  const std::array<math::vec2, 4> square{
      {{4.0f, 4.0f}, {20.0f, 4.0f}, {20.0f, 18.0f}, {4.0f, 18.0f}}};
  fill_polygon(cb, square, r8g8b8a8{0xff, 0x80, 0x20, 0xff}, 0.05f);

  const std::array<math::vec2, 3> triangle{{{26.0f, 5.0f}, {40.0f, 18.0f}, {18.0f, 22.0f}}};
  stroke_polygon(cb, triangle, r8g8b8a8{0x10, 0xc0, 0xff, 0xff}, 3.0f, true, line_join::round,
                 line_cap::round, 0.1f);

  path_2d dashed;
  dashed.move_to(6.0f, 30.0f).line_to(24.0f, 30.0f).line_to(42.0f, 12.0f).line_to(60.0f, 12.0f);
  const std::array<float, 2> dash{{6.0f, 4.0f}};
  stroke_path(cb, dashed, r8g8b8a8{0xff, 0xff, 0xff, 0xff}, 2.5f, line_join::miter, line_cap::round,
              0.15f, dash, 1.5f);

  const u64 hash = hash_command_buffer(cb);
  CHECK(!cb.vertex_buffer.empty());
  CHECK(!cb.index_buffers[static_cast<usize>(vertex_topology::triangle)].empty());
  CHECK(hash == 0x29368fa6dde1dfe6ull);
  return g_fail == 0 ? 0 : 1;
}
