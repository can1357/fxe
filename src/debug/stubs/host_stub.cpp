// Link-time stubs for fxe::js::host accessor methods invoked by fxe_debug.
// Compiled into fxe_debug ONLY when FXE_ENABLE_V8 is OFF; in V8 builds the
// real implementations live in src/js/v8_host.cpp.
//
// Every dispatch.cpp / server.cpp call site guards on `cx.host != nullptr`,
// and in non-V8 builds no host is ever attached, so these bodies are
// unreachable at runtime. They exist purely so fxe_debug links cleanly when
// the host implementation isn't available.

#include <fxe/v8_host.hpp>

#include <cstddef>

namespace fxe::js {

  window* host::active_window() const noexcept {
    return nullptr;
  }

  renderer* host::active_renderer() const noexcept {
    return nullptr;
  }

  std::vector<window*> host::windows() const {
    return {};
  }

  window* host::window_at(std::size_t) const noexcept {
    return nullptr;
  }

  renderer* host::renderer_for(window*) const noexcept {
    return nullptr;
  }

} // namespace fxe::js
