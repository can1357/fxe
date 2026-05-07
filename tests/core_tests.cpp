// fxe core test harness — exercises every fxe_core public API plus the small subset of
// renderer interface that doesn't need a GPU context. Hand-written assert framework keeps
// the test binary tiny so it stays runnable on every CI matrix entry.

#include <fxe/color.hpp>
#include <fxe/command_buffer.hpp>
#include <fxe/font.hpp>
#include <fxe/primitives.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>

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
#define CHECK_NEAR(a, b, eps)                                                                      \
  check(std::fabs(double(a) - double(b)) < double(eps), #a " ~= " #b, __FILE__, __LINE__)

  // FNV-1a 64-bit over arbitrary bytes. Used to pin determinism of primitive output.
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

static void build_showcase_command_buffer(fxe::command_buffer& cb, const fxe::font_info& font) {
  using namespace fxe;
  primitives::draw_line(cb, {40, 40, 0, 0}, {220, 140, 0, 0}, red, 3.0f);
  primitives::fill_rect(cb, {260, 40}, {120, 80}, 0.0f, green);
  primitives::draw_rect(cb, {410, 40}, {120, 80}, 0.0f, blue, 4.0f);
  primitives::fill_ellipse(cb, math::make_transform({170, 270, 0}, {90, 60, 1}), yellow, 1.0f, 32);
  primitives::fill_box(cb, {-0.35f, -0.25f, -0.2f}, {0.35f, 0.25f, 0.2f}, magenta);
  primitives::draw_text(cb, {300, 260}, 0.0f, "fxe", font, {.color = white, .pt = 32.0f});
}

static void build_text_sprite_command_buffer(fxe::command_buffer& cb, fxe::spritesheet& sheet) {
  using namespace fxe;
  init_default_fonts(sheet);
  const texture_id texture =
      sheet.add_texture({.size = {2, 2}, .pixels = {red, green, blue, white}});
  const texture_id sprite_id = sheet.add_sprite({.at = {0, 0}, .size = {2, 2}, .texture = texture});
  const auto& spr = sheet.sprites.at(sprite_id - 1);
  primitives::fill_rect(cb, {96, 56}, {128, 128}, 0.0f, white,
                        {.texture = spr.texture, .src = {0.0f, 0.0f}, .dst = {1.0f, 1.0f}});
  primitives::draw_text(cb, {16, 24}, 0.0f, "Sprite 42", sheet.default_font,
                        {.color = cyan, .pt = 18.0f});
}

static void test_command_buffer_basics() {
  static_assert(sizeof(fxe::vertex) == 32, "vertex layout regression");

  fxe::command_buffer cb;
  CHECK(cb.epoch == 1);
  auto [verts, idx] = cb.allocate(3, 3, fxe::vertex_topology::triangle);
  CHECK(cb.epoch == 2);
  CHECK(cb.vertex_buffer.size() == 3);
  CHECK(cb.index_buffers[0].size() == 3);
  CHECK(idx[0] == 0 && idx[1] == 0 && idx[2] == 0);
  (void)verts;

  auto* strip = cb.allocate_strip(4, fxe::vertex_topology::triangle);
  (void)strip;
  auto& tri = cb.index_buffers[0];
  CHECK(tri.size() == 9);
  CHECK(tri[3] == 3 && tri[4] == 4 && tri[5] == 5);
  CHECK(tri[6] == 4 && tri[7] == 5 && tri[8] == 6);

  // queue() across two buffers preserves vertex ordering and rebases indices.
  fxe::command_buffer src;
  fxe::primitives::fill_rect(src, {0, 0}, {1, 1}, 0.0f, fxe::white);
  fxe::command_buffer dst;
  fxe::primitives::fill_rect(dst, {0, 0}, {1, 1}, 0.0f, fxe::red);
  usize base = dst.vertex_buffer.size();
  dst.queue(src);
  CHECK(dst.vertex_buffer.size() == base + src.vertex_buffer.size());
  CHECK(dst.index_buffers[0].size() == 12);
  // Last 6 indices come from src and must be rebased by base (=4).
  for (usize i = 6; i < dst.index_buffers[0].size(); ++i) {
    CHECK(dst.index_buffers[0][i] >= base);
  }

  // transform() updates vertex positions but keeps cardinality.
  fxe::command_buffer t;
  fxe::primitives::fill_rect(t, {0, 0}, {1, 1}, 0.0f, fxe::white);
  auto [mn0, mx0] = t.get_boundaries();
  fxe::math::mat4x4 m = fxe::math::make_transform({10, 20, 0}, {2, 3, 1});
  t.transform(m);
  auto [mn1, mx1] = t.get_boundaries();
  CHECK_NEAR(mn1.x, mn0.x * 2 + 10, 0.001);
  CHECK_NEAR(mn1.y, mn0.y * 3 + 20, 0.001);
  CHECK_NEAR(mx1.x, mx0.x * 2 + 10, 0.001);

  // is_empty() / clone() ----------------------------------------------------
  fxe::command_buffer empty;
  CHECK(empty.is_empty());
  CHECK(empty.clone().is_empty());

  fxe::command_buffer src2;
  fxe::primitives::fill_rect(src2, {0, 0}, {4, 4}, 0.0f, fxe::white);
  CHECK(!src2.is_empty());
  fxe::command_buffer cloned = src2.clone();
  CHECK(cloned.vertex_buffer.size() == src2.vertex_buffer.size());
  for (usize i = 0; i != cloned.index_buffers.size(); ++i)
    CHECK(cloned.index_buffers[i].size() == src2.index_buffers[i].size());
  CHECK(cloned.epoch == src2.epoch);
  // Storage independence: mutating original must leave clone untouched.
  const usize cloned_v_before = cloned.vertex_buffer.size();
  const usize cloned_i_before = cloned.index_buffers[0].size();
  fxe::primitives::fill_rect(src2, {10, 10}, {2, 2}, 0.0f, fxe::red);
  CHECK(cloned.vertex_buffer.size() == cloned_v_before);
  CHECK(cloned.index_buffers[0].size() == cloned_i_before);
  CHECK(src2.vertex_buffer.size() > cloned_v_before);
}

static void test_color_palette() {
  using namespace fxe;
  CHECK(red.r == 255 && red.g == 0 && red.b == 0 && red.a == 255);
  CHECK(white.luminance() > 254.0f);
  CHECK(black.luminance() == 0.0f);

  // darken / lighten preserve alpha and stay clamped.
  r8g8b8a8 c{100, 150, 200, 200};
  auto d = c.darken(0.5f);
  CHECK(d.r == 50 && d.g == 75 && d.b == 100 && d.a == 200);
  auto l = c.lighten(1.0f);
  CHECK(l.r == 200 && l.g == 200 && l.b == 200 && l.a == 200);

  // luminance(target) rescales.
  auto m = c.luminance(150.0f);
  CHECK_NEAR(m.luminance(), 150.0f, 1.5f);

  // Named lookup returns transparent on miss, the right thing on hit.
  CHECK(color_by_name("RED") == red);
  CHECK(color_by_name("CORNFLOWERBLUE") == cornflowerblue);
  CHECK(color_by_name("NOPE") == transparent);

  // Lookup table is sorted (the binary search relies on it).
  for (usize i = 1; i < color_table.size(); ++i) {
    CHECK(std::string_view(color_table[i - 1].first) < std::string_view(color_table[i].first));
  }
  CHECK(color_table.size() == 150);
}

static void test_primitives_2d() {
  using namespace fxe;
  using namespace fxe::primitives;

  command_buffer cb;
  fill_rect(cb, {10, 20}, {30, 40}, 0.0f, white);
  CHECK(cb.vertex_buffer.size() == 4);
  CHECK(cb.index_buffers[0].size() == 6);
  auto [mn, mx] = cb.get_boundaries();
  CHECK_NEAR(mn.x, 10.0f, 0.01);
  CHECK_NEAR(mn.y, 20.0f, 0.01);
  CHECK_NEAR(mx.x, 40.0f, 0.01);
  CHECK_NEAR(mx.y, 60.0f, 0.01);

  command_buffer rounded;
  fill_rect_rounded(rounded, math::make_transform({0, 0, 0}, {100, 100, 1}),
                    primitives::optional_list<float, 4>{10.0f}, 0.0f,
                    primitives::color_list<4>{white, white, white, white});
  CHECK(rounded.vertex_buffer.size() > 4);
  CHECK(!rounded.index_buffers[0].empty());

  command_buffer ellipse;
  fill_ellipse(ellipse, math::make_transform({0, 0, 0}, {10, 10, 1}), red, 1.0f, 32);
  // fill_ellipse with N edges: round((N+1)*pct) ring verts + 1 centre,
  // (rings - 1) * 3 fan indices.
  CHECK(ellipse.vertex_buffer.size() == 34);
  CHECK(ellipse.index_buffers[0].size() == 32 * 3);

  // Thick world-space line emits a swept prism (8 verts, 36 indices).
  command_buffer line;
  draw_line(line, math::vec4{0, 0, 0, 1}, math::vec4{10, 0, 0, 1}, blue, 2.0f);
  CHECK(line.vertex_buffer.size() == 8);
  CHECK(line.index_buffers[0].size() == 36);

  command_buffer blur;
  blur_rect(blur, {0, 0}, {64, 64}, 0.0f, r8g8b8a8{255, 255, 255, 128}, 4.0f, {128, 128});
  CHECK(blur.vertex_buffer.size() == 4 * 26);
  CHECK(blur.index_buffers[0].size() == 6 * 26);
  CHECK(std::bit_cast<texture_id>(blur.vertex_buffer[4].uv.z) == framebuffer_texture);
}

static void test_primitives_3d() {
  using namespace fxe;
  using namespace fxe::primitives;

  command_buffer cb;
  auto m = math::make_transform({0, 0, 0}, {2, 2, 2});
  primitives::color_list<8> uniform{white, white, white, white, white, white, white, white};
  fill_box(cb, m, uniform);
  CHECK(cb.vertex_buffer.size() == 8);
  CHECK(cb.index_buffers[0].size() == 36);

  command_buffer cube;
  fill_cbox(cube, m, uniform);
  CHECK(cube.vertex_buffer.size() == 8);
  CHECK(cube.index_buffers[0].size() == 36);

  command_buffer sphere;
  fill_sphere(sphere, m, white, 1.0f, 1.0f, 8);
  CHECK(!sphere.vertex_buffer.empty());
  CHECK(!sphere.index_buffers[0].empty());

  command_buffer cylinder;
  fill_cylinder(cylinder, m, primitives::color_list<2>{white, white}, 1.0f, 16);
  CHECK(!cylinder.vertex_buffer.empty());

  command_buffer pyramid;
  fill_pyramid(pyramid, m, primitives::color_list<2>{white, red});
  CHECK(!pyramid.vertex_buffer.empty());
}

static void test_text_and_spritesheet() {
  using namespace fxe;
  spritesheet sheet;
  init_default_fonts(sheet);
  // Font construction now goes through the font module: a face is attached
  // to font_info.runtime->face and glyphs live in font::shared_glyph_cache.
  // The legacy `font_info.glyphs` per-codepoint map is no longer populated.
  CHECK(font_face_for(sheet.default_font) != nullptr);
  if (auto face = font_face_for(sheet.default_font); face) {
    CHECK(face->glyph_index(U'A') != 0);
    CHECK(face->pixel_size() > 0.0f);
    const auto m = face->metrics();
    CHECK(m.line_height > 0.0f);
  }
  CHECK(sheet.default_font.texture != null_texture);
  CHECK((sheet.default_font.texture & msprite_flag) != 0);
  const texture_id font_texture = sheet.default_font.texture & sprite_mask;
  CHECK(font_texture != null_texture);
  CHECK(font_texture <= sheet.textures.size());

  command_buffer cb;
  primitives::text_style style{.color = white, .pt = 16.0f};
  auto extent = primitives::draw_text(cb, math::vec2{0, 0}, 0.0f, "Hi", sheet.default_font, style);
  CHECK(extent.x > 0.0f);
  CHECK(extent.y > 0.0f);
  CHECK(!cb.vertex_buffer.empty());
  CHECK((std::bit_cast<texture_id>(cb.vertex_buffer.front().uv.z) &
         (msprite_flag | font_mask_flag | font_color_flag)) != 0);

  auto calc = primitives::calc_text("Hello, world!", sheet.default_font, 16.0f);
  CHECK(calc.x > 0.0f);
  CHECK(calc.y > 0.0f);

  const usize before_reload_texture_count = sheet.textures.size();
  const texture_id before_reload_texture = sheet.default_font.texture & sprite_mask;
  init_default_fonts(sheet, std::span<const u8>{});
  const texture_id after_reload_texture = sheet.default_font.texture & sprite_mask;
  CHECK(sheet.textures.size() == before_reload_texture_count + 1);
  CHECK(after_reload_texture != before_reload_texture);
  CHECK(after_reload_texture == sheet.textures.size());
  CHECK((get_font_info().texture & sprite_mask) == after_reload_texture);

  // resolve_if rolls through animated frames deterministically.
  asprite anim;
  anim.base_texture = static_cast<texture_id>(10);
  anim.delays = {0.5f, 0.5f};
  sheet.asprites.push_back(anim);
  texture_id animated = static_cast<texture_id>(asprite_flag | 1u);
  CHECK(sheet.resolve_if(animated, 0.1f) == 10);
  CHECK(sheet.resolve_if(animated, 0.7f) == 11);
}

static void test_hashable_determinism() {
  using namespace fxe;
  using namespace fxe::primitives;

  // A deterministic scene: every output bit must be stable across builds. If a primitive
  // implementation drifts, the hash drifts too.
  command_buffer cb;
  fill_rect(cb, {0, 0}, {32, 16}, 0.0f, red);
  draw_rect(cb, {2, 2}, {10, 10}, 0.5f, blue, 1.0f);
  fill_triangle(cb, math::vec4{0, 0, 0, 1}, math::vec4{1, 0, 0, 1}, math::vec4{0, 1, 0, 1}, green);
  fill_box(cb, {-1, -1, -1}, {1, 1, 1}, white);
  draw_line(cb, math::vec4{0, 0, 0, 0}, math::vec4{10, 10, 0, 0}, magenta, 0.0f);

  u64 h = hash_command_buffer(cb);
  CHECK(cb.vertex_buffer.size() > 0);
  CHECK(cb.index_buffers[0].size() > 0);
  CHECK(cb.index_buffers[1].size() > 0);
  // Re-running yields the same hash.
  command_buffer cb2;
  fill_rect(cb2, {0, 0}, {32, 16}, 0.0f, red);
  draw_rect(cb2, {2, 2}, {10, 10}, 0.5f, blue, 1.0f);
  fill_triangle(cb2, math::vec4{0, 0, 0, 1}, math::vec4{1, 0, 0, 1}, math::vec4{0, 1, 0, 1}, green);
  fill_box(cb2, {-1, -1, -1}, {1, 1, 1}, white);
  draw_line(cb2, math::vec4{0, 0, 0, 0}, math::vec4{10, 10, 0, 0}, magenta, 0.0f);
  CHECK(hash_command_buffer(cb2) == h);

  std::printf("scene-hash=0x%016llx vertex_count=%zu line_count=%zu\n",
              static_cast<unsigned long long>(h), cb.vertex_buffer.size(),
              cb.index_buffers[1].size());
}

static void test_golden_command_buffer_hashes() {
  fxe::spritesheet sheet;
  fxe::init_default_fonts(sheet);

  fxe::command_buffer showcase;
  build_showcase_command_buffer(showcase, sheet.default_font);
  const u64 showcase_hash = hash_command_buffer(showcase);
  CHECK(showcase.vertex_buffer.size() == 78);
  CHECK(showcase.index_buffers[0].size() == 186);
  CHECK(showcase_hash == 0x2996eaf4e6896e55ull);

  fxe::spritesheet text_sprite_sheet;
  fxe::command_buffer text_sprite;
  build_text_sprite_command_buffer(text_sprite, text_sprite_sheet);
  const u64 text_sprite_hash = hash_command_buffer(text_sprite);
  CHECK(text_sprite.vertex_buffer.size() == 36);
  CHECK(text_sprite.index_buffers[0].size() == 54);
  CHECK(text_sprite_hash == 0xba9a70066e194e48ull);

  std::printf("golden-showcase=0x%016llx vertices=%zu indices=%zu\n",
              static_cast<unsigned long long>(showcase_hash), showcase.vertex_buffer.size(),
              showcase.index_buffers[0].size());
  std::printf("golden-text-sprite=0x%016llx vertices=%zu indices=%zu\n",
              static_cast<unsigned long long>(text_sprite_hash), text_sprite.vertex_buffer.size(),
              text_sprite.index_buffers[0].size());
}

int main() {
  test_command_buffer_basics();
  test_color_palette();
  test_primitives_2d();
  test_primitives_3d();
  test_text_and_spritesheet();
  test_hashable_determinism();
  test_golden_command_buffer_hashes();

  std::cout << "fxe core tests: " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}
