#include <fxe/primitives.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/window.hpp>

int main() {
  auto win = fxe::create_window({640, 480, false, false, "fxe primitives showcase"});
  auto renderer = fxe::create_renderer(*win, {.vsync = false});

  fxe::spritesheet sheet;
  fxe::init_default_fonts(sheet);

  renderer->begin_frame();
  fxe::primitives::draw_line(*renderer, {40, 40, 0, 0}, {220, 140, 0, 0}, fxe::red, 3.0f);
  fxe::primitives::fill_rect(*renderer, {260, 40}, {120, 80}, 0.0f, fxe::green);
  fxe::primitives::draw_rect(*renderer, {410, 40}, {120, 80}, 0.0f, fxe::blue, 4.0f);
  fxe::primitives::fill_ellipse(*renderer, fxe::math::make_transform({170, 270, 0}, {90, 60, 1}),
                                fxe::yellow, 1.0f, 32);
  fxe::primitives::fill_box(*renderer, {-0.35f, -0.25f, -0.2f}, {0.35f, 0.25f, 0.2f}, fxe::magenta);
  fxe::primitives::draw_text(*renderer, {300, 260}, 0.0f, "fxe", sheet.default_font,
                             {.color = fxe::white, .pt = 32.0f});
  renderer->end_frame();

  return renderer->is_empty() ? 1 : 0;
}
