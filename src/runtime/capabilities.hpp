#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::runtime {

  struct capability_set {
    struct webauthn_policy {
      std::vector<std::string> rp_ids;
      std::string attestation = "none";
      std::string user_verification = "preferred";
      std::vector<std::string> transports;
      bool allow_virtual_authenticator = false;
    };

    // Each entry: nullopt = allow-all (legacy default), {} = deny-all, populated = allowlist.
    std::optional<std::vector<std::string>> fs_allow;  // path prefixes (canonicalised)
    std::optional<std::vector<std::string>> net_allow; // host[:port] entries
    std::optional<bool> shell_allow;                   // nullopt = allow, false = deny
    std::optional<bool> native_allow;                  // reserved for plugin loading
    std::optional<webauthn_policy> webauthn_allow;
  };

  // Register the policy for a window. window_id is the bind_window holder pointer or 0
  // for the implicit "default" policy applied when no window has registered yet.
  //
  // FXE v1 uses a single V8 isolate for all windows, so bindings cannot yet select
  // a per-window policy at call time. Until per-window isolates land, the effective
  // isolate policy is the most-permissive merge of all registered window policies:
  // an operation is allowed if any registered window policy allows it. If no window
  // has registered, all operations are allowed to preserve the legacy default.
  void register_window_capabilities(int window_id, capability_set policy);
  void unregister_window_capabilities(int window_id);

  // Returns true iff the operation is allowed under the most-permissive merged policy.
  bool fs_path_allowed(std::string_view absolute_or_relative_path);
  bool net_host_allowed(std::string_view url);
  bool shell_allowed();
  bool native_allowed();
  std::optional<capability_set::webauthn_policy> webauthn_allowed(std::string_view rp_id);

} // namespace fxe::runtime
