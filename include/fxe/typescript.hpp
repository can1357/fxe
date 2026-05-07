#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace v8 {
  class Isolate;
}

namespace fxe::js {
  struct typescript_transpile_result {
    bool ok = false;
    std::string source;
    std::string message;
    int source_map_line_offset = 0;
  };

  bool is_typescript_path(const std::filesystem::path& path);

  // Runs the embedded TypeScript compiler inside the supplied V8 isolate. This
  // is real tsc emit, not regex stripping: it handles parameter properties,
  // access modifiers, type-only imports, and local const enum inlining.
  typescript_transpile_result transpile_typescript(v8::Isolate* isolate, std::string_view source,
                                                   std::string_view origin = "<typescript>");

  void dispose_typescript_compiler(v8::Isolate* isolate) noexcept;

  // Extract the inline source-map JSON from a transpiled JS payload. Returns
  // an empty string if no inline `sourceMappingURL` is present.
  std::string extract_inline_source_map(std::string_view js);
} // namespace fxe::js
