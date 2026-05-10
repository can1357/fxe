#pragma once

#include <fxe/window.hpp>

namespace fxe {
  class glfw_window;

  void install_macos_gesture_hooks(void* nsview, glfw_window* w);
  void install_win32_pointer_hooks(void* hwnd, glfw_window* w);
  inline void glfw_window_inject_gesture_event(window* w, input_event ev) {
    if (!w)
      return;
    w->inject(ev);
  }
} // namespace fxe
