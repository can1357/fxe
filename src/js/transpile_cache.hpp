#pragma once

#include <string>
#include <string_view>

namespace fxe::js {
  // Look up a previously-cached transpile result. On hit, populates `emitted`
  // and `source_map_line_offset` and returns true. On miss returns false and
  // leaves the out-params untouched.
  bool transpile_cache_lookup(std::string_view origin, std::string_view source,
                              std::string& emitted, int& source_map_line_offset);

  // Store a transpile result. Subsequent lookups with the same origin + source
  // bytes return the same (emitted, source_map_line_offset). Thread-safe.
  void transpile_cache_store(std::string_view origin, std::string_view source, std::string emitted,
                             int source_map_line_offset);

  // Reset the entire cache. Intended for tests / shutdown only.
  void transpile_cache_clear();
} // namespace fxe::js
