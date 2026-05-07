#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <v8.h>

namespace fxe::runtime {
  struct node_compat_asset {
    std::string canonical_specifier;
    std::string asset_path;
    std::string_view source;
  };

  void install_node_compat(v8::Isolate* iso, v8::Local<v8::Context> ctx);
  bool is_node_builtin_specifier(std::string_view specifier);
  std::optional<node_compat_asset> resolve_node_compat_asset(std::string_view specifier);
  std::optional<node_compat_asset> resolve_node_compat_asset_path(std::string_view asset_path);
  std::optional<node_compat_asset> resolve_unenv_pathe_asset();
  void throw_node_compat_disabled(v8::Isolate* iso, std::string_view specifier);

} // namespace fxe::runtime
