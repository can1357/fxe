#include <fxe/webauthn.hpp>

#include <algorithm>

namespace fxe::webauthn {

  bool is_valid_rp_id_syntax(std::string_view rp_id) {
    if (rp_id.empty())
      return false;
    if (rp_id.front() == '.' || rp_id.back() == '.')
      return false;

    char previous = '\0';
    for (char ch : rp_id) {
      const bool is_lower = ch >= 'a' && ch <= 'z';
      const bool is_digit = ch >= '0' && ch <= '9';
      const bool is_dash = ch == '-';
      const bool is_dot = ch == '.';
      if (!(is_lower || is_digit || is_dash || is_dot))
        return false;
      if (previous == '.' && ch == '.')
        return false;
      previous = ch;
    }
    return true;
  }

  bool host_matches_rp_id(std::string_view origin_host, std::string_view rp_id) {
    if (origin_host.empty())
      return false;
    if (origin_host == rp_id)
      return true;
    if (origin_host.size() <= rp_id.size())
      return false;
    const size_t offset = origin_host.size() - rp_id.size();
    return origin_host[offset - 1] == '.' && origin_host.substr(offset) == rp_id;
  }

  bool validate_rp_id(std::string_view rp_id, std::string_view origin_host,
                      const rp_id_policy& policy) {
    if (!is_valid_rp_id_syntax(rp_id))
      return false;
    if (!host_matches_rp_id(origin_host, rp_id))
      return false;
    if (policy.allow_any)
      return true;
    return std::find(policy.allowed.begin(), policy.allowed.end(), rp_id) != policy.allowed.end();
  }

} // namespace fxe::webauthn
