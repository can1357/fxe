// Server-internal hooks shared between server.cpp and dispatch.cpp.
//
// These functions read/write members of `server::impl`. They are kept out of
// the public header to avoid bloating <fxe/debug.hpp>.

#pragma once

namespace fxe::debug {
  class server;

  namespace detail {
    void server_set_pause(server* s, bool paused, bool single_step);
    void server_set_console_enabled(server* s, bool on);
  } // namespace detail
} // namespace fxe::debug
