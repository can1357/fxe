#include <fxe/webauthn.hpp>

#include <array>
#include <string_view>

namespace fxe::webauthn {
  namespace {

    bool one_of(std::string_view value, std::initializer_list<std::string_view> allowed) {
      for (std::string_view candidate : allowed) {
        if (candidate == value)
          return true;
      }
      return false;
    }

  } // namespace

  std::string validate_creation_options(const creation_options& o) {
    if (o.rp_id.empty() || !is_valid_rp_id_syntax(o.rp_id))
      return "creation: rp.id is invalid";
    if (o.rp_name.empty())
      return "creation: rp.name is required";
    if (o.user.id.empty() || o.user.id.size() > 64u)
      return "creation: user.id must be 1-64 bytes";
    if (o.user.name.empty())
      return "creation: user.name is required";
    if (o.challenge.size() < 16u)
      return "creation: challenge must be at least 16 bytes";
    if (o.pub_key_params.empty())
      return "creation: pubKeyCredParams is required";
    if (!one_of(o.authenticator_attachment, {"", "platform", "cross-platform"}))
      return "creation: authenticatorAttachment is invalid";
    if (!one_of(o.user_verification, {"", "discouraged", "preferred", "required"}))
      return "creation: userVerification is invalid";
    if (!one_of(o.attestation, {"", "none", "indirect", "direct", "enterprise"}))
      return "creation: attestation is invalid";
    if (!one_of(o.resident_key, {"", "discouraged", "preferred", "required"}))
      return "creation: residentKey is invalid";
    return {};
  }

  std::string validate_request_options(const request_options& o) {
    if (o.rp_id.empty() || !is_valid_rp_id_syntax(o.rp_id))
      return "request: rp.id is invalid";
    if (o.challenge.size() < 16u)
      return "request: challenge must be at least 16 bytes";
    if (!one_of(o.user_verification, {"", "discouraged", "preferred", "required"}))
      return "request: userVerification is invalid";
    return {};
  }

} // namespace fxe::webauthn
