#include <fxe/webauthn.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
  namespace webauthn = fxe::webauthn;

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  std::vector<uint8_t> filled(size_t n, uint8_t value) {
    return std::vector<uint8_t>(n, value);
  }

} // namespace

int main() {
  webauthn::virtual_authenticator auth(0xC0FFEEu);

  webauthn::creation_options create;
  create.rp_id = "example.com";
  create.rp_name = "Example";
  create.user.id = {'u', '-', '1'};
  create.user.name = "alice";
  create.user.display_name = "Alice";
  create.challenge = filled(16u, 0x00);
  create.pub_key_params = {webauthn::cose_algorithm::es256};
  create.attestation = "none";

  webauthn::register_response first_register;
  const std::string register_error =
      auth.register_credential(create, "https://example.com", first_register);
  CHECK(register_error.empty());
  CHECK(first_register.credential_id.size() == 32u);

  const auto attestation = webauthn::decode_attestation_object(first_register.attestation_object);
  CHECK(attestation.has_value());
  webauthn::cose_ec2_key public_key;
  if (attestation) {
    const auto auth_data = webauthn::parse_authenticator_data(attestation->auth_data);
    CHECK(auth_data.has_value());
    if (auth_data) {
      CHECK((auth_data->flags & webauthn::flag_attested_cred) != 0u);
      CHECK(auth_data->attested.has_value());
      if (auth_data->attested) {
        CHECK(auth_data->attested->credential_id == first_register.credential_id);
        CHECK(auth_data->attested->aaguid == webauthn::virtual_authenticator::aaguid());
        const auto decoded_key = webauthn::decode_cose_ec2(auth_data->attested->cose_public_key);
        CHECK(decoded_key.has_value());
        if (decoded_key)
          public_key = *decoded_key;
      }
    }
  }

  webauthn::request_options request;
  request.rp_id = "example.com";
  request.challenge = filled(16u, 0x01);
  request.allow_credentials = {first_register.credential_id};
  request.user_verification = "preferred";

  webauthn::assert_response assertion;
  const std::string assert_error =
      auth.assert_credential(request, "https://example.com", assertion);
  CHECK(assert_error.empty());
  const auto parsed_assertion = webauthn::parse_authenticator_data(assertion.authenticator_data);
  CHECK(parsed_assertion.has_value());
  if (parsed_assertion) {
    CHECK(parsed_assertion->sign_count == 1u);
    CHECK((parsed_assertion->flags & webauthn::flag_user_present) != 0u);
    CHECK((parsed_assertion->flags & webauthn::flag_user_verified) != 0u);
  }

  const auto client_hash = webauthn::sha256(assertion.client_data_json);
  std::vector<uint8_t> signed_message = assertion.authenticator_data;
  signed_message.insert(signed_message.end(), client_hash.begin(), client_hash.end());
  CHECK(webauthn::virtual_authenticator::verify_es256(public_key, signed_message,
                                                      assertion.signature));

  std::vector<uint8_t> tampered_json = assertion.client_data_json;
  tampered_json[0] ^= 0x01;
  const auto tampered_hash = webauthn::sha256(tampered_json);
  std::vector<uint8_t> tampered_message = assertion.authenticator_data;
  tampered_message.insert(tampered_message.end(), tampered_hash.begin(), tampered_hash.end());
  CHECK(!webauthn::virtual_authenticator::verify_es256(public_key, tampered_message,
                                                       assertion.signature));

  const auto credentials = auth.list_credentials();
  CHECK(credentials.size() == 1u);

  webauthn::register_response second_register;
  const std::string second_error =
      auth.register_credential(create, "https://example.com", second_register);
  CHECK(second_error.empty());
  CHECK(second_register.credential_id.size() == 32u);
  CHECK(second_register.credential_id != first_register.credential_id);

  auth.clear();
  CHECK(auth.list_credentials().empty());
  webauthn::assert_response after_clear;
  const std::string clear_error =
      auth.assert_credential(request, "https://example.com", after_clear);
  CHECK(clear_error.find("no credential") != std::string::npos);

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
