// Internal helpers for the fxe debug server transport surfaces.
#pragma once

#include <fxe/debug.hpp>
#include <fxe/types.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace fxe::debug {
  // Minimal Chrome DevTools discovery metadata for the CDP WebSocket transport.
  json make_cdp_target_descriptor(std::string_view host, u16 port, std::string_view id = "fxe-main",
                                  std::string_view type = "node",
                                  std::string_view title = "fxe application");

  json make_cdp_version_descriptor();
  json make_cdp_version_descriptor(std::string_view host, u16 port);

  // Returns a complete HTTP response for DevTools discovery paths (/json,
  // /json/list, /json/version, /json/protocol), or nullopt for paths this
  // server does not own.
  std::optional<std::string> make_cdp_discovery_http_response(std::string_view path,
                                                              std::string_view host, u16 port);
} // namespace fxe::debug
