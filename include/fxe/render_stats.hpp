#pragma once

#include <cstdint>

namespace fxe {
  // Per-thread cumulative counters for the JS reactive reconciler and the
  // C++ command_buffer::queue path. The reconciler bumps cache_hits / rebuilds
  // / cache_misses / frames itself; queue_calls and the *_submitted counters
  // are bumped by command_buffer::queue.
  struct render_stats {
    std::uint64_t vertices_submitted = 0;
    std::uint64_t indices_submitted = 0;
    std::uint64_t queue_calls = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t rebuilds = 0;
    std::uint64_t frames = 0;
    void reset() noexcept;
  };

  // Thread-local storage. Every JS isolate runs single-threaded so the
  // reconciler observes a consistent view across one frame.
  render_stats& current_render_stats() noexcept;
} // namespace fxe
