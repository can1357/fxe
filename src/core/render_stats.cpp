#include <fxe/render_stats.hpp>

namespace fxe {
  void render_stats::reset() noexcept {
    *this = render_stats{};
  }

  render_stats& current_render_stats() noexcept {
    thread_local render_stats tls{};
    return tls;
  }
} // namespace fxe
