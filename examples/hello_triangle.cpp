#include <fxe/primitives.hpp>
#include <fxe/renderer.hpp>
#include <fxe/types.hpp>
#include <fxe/window.hpp>

#include <cstddef>

int main() {
  auto win = fxe::create_window({640, 480, false, true, "fxe hello triangle"});
  auto renderer = fxe::create_renderer(*win, {.vsync = false});

  usize last_vertex_count = 0;
  for (int frame = 0; frame != 3 && !win->should_close(); ++frame) {
    win->poll();
    renderer->begin_frame();
    fxe::primitives::fill_triangle(*renderer, {-0.6f, -0.5f, 0.0f, 1.0f}, {0.6f, -0.5f, 0.0f, 1.0f},
                                   {0.0f, 0.6f, 0.0f, 1.0f}, fxe::cyan);
    renderer->end_frame();
    last_vertex_count = renderer->vertex_buffer.size();
  }

  return last_vertex_count == 3 ? 0 : 1;
}
