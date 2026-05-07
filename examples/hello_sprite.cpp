#include <fxe/primitives.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/window.hpp>

#include <bit>

int main() {
  auto win = fxe::create_window({320, 240, false, false, "fxe hello sprite"});
  auto renderer = fxe::create_renderer(*win, {.vsync = false});

  fxe::spritesheet sheet;
  const fxe::texture_id texture =
      sheet.add_texture({.size = {2, 2}, .pixels = {fxe::red, fxe::green, fxe::blue, fxe::white}});
  const fxe::texture_id sprite =
      sheet.add_sprite({.at = {0, 0}, .size = {2, 2}, .texture = texture});
  const auto& entry = sheet.sprites.at(sprite - 1);

  renderer->begin_frame();
  fxe::primitives::fill_rect(*renderer, {96, 56}, {128, 128}, 0.0f, fxe::white,
                             {.texture = entry.texture, .src = {0.0f, 0.0f}, .dst = {1.0f, 1.0f}});
  renderer->end_frame();

  return renderer->vertex_buffer.size() == 4 &&
                 std::bit_cast<fxe::texture_id>(renderer->vertex_buffer[0].uv.z) == texture
             ? 0
             : 1;
}
