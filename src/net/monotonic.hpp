#pragma once

#include <fxe/types.hpp>

#include <chrono>

namespace fxe::net {

  // Steady-clock millisecond timestamp, used for HTTP/2 + WebSocket
  // timeout/idle bookkeeping. Wraps the same `steady_clock` calls every net
  // module had open-coded.
  inline i64 monotonic_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
        .count();
  }

} // namespace fxe::net
