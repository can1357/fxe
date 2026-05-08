#include "runtime/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fxe/types.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace fxe::runtime {
  namespace {
    namespace fs = std::filesystem;

    std::mutex g_mu;
    std::unordered_map<int, capability_set> g_policies;

    std::string ascii_lower(std::string s) {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    bool starts_with(std::string_view s, std::string_view prefix) {
      return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    std::string canonical_path(std::string_view raw) {
      std::error_code ec;
      auto canonical = fs::weakly_canonical(fs::path(std::string(raw)), ec);
      if (!ec)
        return canonical.lexically_normal().generic_string();

      ec.clear();
      auto absolute = fs::absolute(fs::path(std::string(raw)), ec);
      if (!ec)
        return absolute.lexically_normal().generic_string();

      return fs::path(std::string(raw)).lexically_normal().generic_string();
    }

    std::optional<std::vector<std::string>>
    canonical_fs_allow(std::optional<std::vector<std::string>> allow) {
      if (!allow)
        return std::nullopt;
      std::vector<std::string> out;
      out.reserve(allow->size());
      for (const auto& prefix : *allow)
        out.push_back(canonical_path(prefix));
      return out;
    }

    std::optional<std::vector<std::string>>
    normalise_net_allow(std::optional<std::vector<std::string>> allow) {
      if (!allow)
        return std::nullopt;
      std::vector<std::string> out;
      out.reserve(allow->size());
      for (auto entry : *allow)
        out.push_back(ascii_lower(std::move(entry)));
      return out;
    }

    int attestation_rank(std::string_view value) {
      if (value == "direct")
        return 2;
      if (value == "indirect")
        return 1;
      return 0;
    }

    std::string attestation_name(int rank) {
      switch (rank) {
      case 2:
        return "direct";
      case 1:
        return "indirect";
      default:
        return "none";
      }
    }

    int user_verification_rank(std::string_view value) {
      if (value == "required")
        return 2;
      if (value == "preferred")
        return 1;
      if (value == "discouraged")
        return 0;
      return 1;
    }

    std::string user_verification_name(int rank) {
      switch (rank) {
      case 2:
        return "required";
      case 1:
        return "preferred";
      default:
        return "discouraged";
      }
    }

    capability_set::webauthn_policy
    normalise_webauthn_policy(capability_set::webauthn_policy policy) {
      for (auto& rp_id : policy.rp_ids)
        rp_id = ascii_lower(std::move(rp_id));
      for (auto& transport : policy.transports)
        transport = ascii_lower(std::move(transport));
      policy.attestation = attestation_name(attestation_rank(policy.attestation));
      policy.user_verification =
          user_verification_name(user_verification_rank(policy.user_verification));
      return policy;
    }

    capability_set normalise_policy(capability_set policy) {
      policy.fs_allow = canonical_fs_allow(std::move(policy.fs_allow));
      policy.net_allow = normalise_net_allow(std::move(policy.net_allow));
      if (policy.webauthn_allow)
        policy.webauthn_allow = normalise_webauthn_policy(std::move(*policy.webauthn_allow));
      return policy;
    }

    struct parsed_endpoint {
      std::string host;
      std::string host_port;
    };

    parsed_endpoint parse_endpoint(std::string_view url) {
      usize start = 0;
      if (auto scheme = url.find("://"); scheme != std::string_view::npos)
        start = scheme + 3;
      else if (starts_with(url, "//"))
        start = 2;

      usize end = url.find_first_of("/?#", start);
      if (end == std::string_view::npos)
        end = url.size();

      std::string_view authority = url.substr(start, end - start);
      if (auto at = authority.rfind('@'); at != std::string_view::npos)
        authority.remove_prefix(at + 1);

      std::string host_port = ascii_lower(std::string(authority));
      std::string host = host_port;
      if (!host_port.empty() && host_port.front() == '[') {
        if (auto close = host_port.find(']'); close != std::string::npos)
          host = host_port.substr(0, close + 1);
      } else if (auto colon = host_port.find(':'); colon != std::string::npos) {
        host = host_port.substr(0, colon);
      }
      return {std::move(host), std::move(host_port)};
    }

    bool entry_has_port(std::string_view entry) {
      if (entry.empty())
        return false;
      if (entry.front() == '[') {
        auto close = entry.find(']');
        return close != std::string_view::npos && close + 1 < entry.size() &&
               entry[close + 1] == ':';
      }
      return entry.find(':') != std::string_view::npos;
    }

    bool fs_policy_allows(const capability_set& policy, std::string_view canonical) {
      if (!policy.fs_allow)
        return true;
      if (policy.fs_allow->empty())
        return false;
      return std::any_of(policy.fs_allow->begin(), policy.fs_allow->end(),
                         [&](const auto& prefix) { return starts_with(canonical, prefix); });
    }

    bool net_policy_allows(const capability_set& policy, const parsed_endpoint& endpoint) {
      if (!policy.net_allow)
        return true;
      if (policy.net_allow->empty())
        return false;
      return std::any_of(
          policy.net_allow->begin(), policy.net_allow->end(), [&](const auto& entry) {
            return entry_has_port(entry) ? entry == endpoint.host_port : entry == endpoint.host;
          });
    }

    bool webauthn_policy_matches(const capability_set::webauthn_policy& policy,
                                 std::string_view rp_id) {
      if (policy.allow_virtual_authenticator && policy.rp_ids.empty())
        return true;
      return std::any_of(policy.rp_ids.begin(), policy.rp_ids.end(),
                         [&](const auto& allowed) { return allowed == rp_id; });
    }
  } // namespace

  void register_window_capabilities(int window_id, capability_set policy) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_policies[window_id] = normalise_policy(std::move(policy));
  }

  void unregister_window_capabilities(int window_id) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_policies.erase(window_id);
  }

  bool fs_path_allowed(std::string_view absolute_or_relative_path) {
    const auto canonical = canonical_path(absolute_or_relative_path);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_policies.empty())
      return true;
    for (const auto& entry : g_policies) {
      const auto& policy = entry.second;
      if (fs_policy_allows(policy, canonical))
        return true;
    }
    return false;
  }

  bool net_host_allowed(std::string_view url) {
    const auto endpoint = parse_endpoint(url);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_policies.empty())
      return true;
    for (const auto& entry : g_policies) {
      const auto& policy = entry.second;
      if (net_policy_allows(policy, endpoint))
        return true;
    }
    return false;
  }

  bool shell_allowed() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_policies.empty())
      return true;
    for (const auto& entry : g_policies) {
      const auto& policy = entry.second;
      if (!policy.shell_allow || *policy.shell_allow)
        return true;
    }
    return false;
  }

  bool native_allowed() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_policies.empty())
      return true;
    for (const auto& entry : g_policies) {
      const auto& policy = entry.second;
      if (!policy.native_allow || *policy.native_allow)
        return true;
    }
    return false;
  }

  std::optional<capability_set::webauthn_policy> webauthn_allowed(std::string_view rp_id) {
    const std::string lowered = ascii_lower(std::string(rp_id));
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_policies.empty())
      return std::nullopt;

    std::optional<capability_set::webauthn_policy> merged;
    for (const auto& entry : g_policies) {
      const auto& policy = entry.second;
      if (!policy.webauthn_allow || !webauthn_policy_matches(*policy.webauthn_allow, lowered))
        continue;
      if (!merged) {
        merged = *policy.webauthn_allow;
        merged->rp_ids = {lowered};
        continue;
      }
      merged->attestation =
          attestation_name(std::max(attestation_rank(merged->attestation),
                                    attestation_rank(policy.webauthn_allow->attestation)));
      merged->user_verification = user_verification_name(
          std::max(user_verification_rank(merged->user_verification),
                   user_verification_rank(policy.webauthn_allow->user_verification)));
      merged->allow_virtual_authenticator =
          merged->allow_virtual_authenticator || policy.webauthn_allow->allow_virtual_authenticator;
      merged->transports.insert(merged->transports.end(), policy.webauthn_allow->transports.begin(),
                                policy.webauthn_allow->transports.end());
      std::sort(merged->transports.begin(), merged->transports.end());
      merged->transports.erase(std::unique(merged->transports.begin(), merged->transports.end()),
                               merged->transports.end());
    }
    return merged;
  }
} // namespace fxe::runtime
