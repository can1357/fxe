#include <fxe/webauthn.hpp>

#include "runtime/cbor.hpp"

#include <mbedtls/sha256.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
  namespace cbor = fxe::runtime::cbor;
  namespace webauthn = fxe::webauthn;

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  std::array<uint8_t, 32> bytes32_array(uint8_t start) {
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = static_cast<uint8_t>(start + i);
    return out;
  }

  std::array<uint8_t, 16> bytes16_array(uint8_t start) {
    std::array<uint8_t, 16> out{};
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = static_cast<uint8_t>(start + i);
    return out;
  }

} // namespace

int main() {
  const webauthn::cose_ec2_key cose_key = [] {
    webauthn::cose_ec2_key key;
    key.x = bytes32_array(0x10);
    key.y = bytes32_array(0x80);
    return key;
  }();
  const std::vector<uint8_t> cose_bytes = webauthn::encode_cose_ec2(cose_key);
  CHECK(!cose_bytes.empty());
  CHECK(cose_bytes[0] == 0xa5);
  CHECK(cose_bytes.size() > 1u);
  CHECK(cose_bytes[1] == 0x01);
  const auto decoded_cose = webauthn::decode_cose_ec2(cose_bytes);
  CHECK(decoded_cose.has_value());
  if (decoded_cose) {
    CHECK(decoded_cose->alg == webauthn::cose_algorithm::es256);
    CHECK(decoded_cose->crv == 1);
    CHECK(decoded_cose->x == cose_key.x);
    CHECK(decoded_cose->y == cose_key.y);
  }

  const std::vector<uint8_t> extensions =
      cbor::encode(cbor::value(cbor::cmap{{std::string("ext"), cbor::value(true)}}));

  webauthn::authenticator_data auth_data;
  auth_data.rp_id_hash = bytes32_array(0x20);
  auth_data.flags =
      static_cast<uint8_t>(webauthn::flag_user_present | webauthn::flag_user_verified);
  auth_data.sign_count = 0x01020304u;
  auth_data.attested = webauthn::attested_credential_data{
      bytes16_array(0x90), std::vector<uint8_t>{0xaa, 0xbb, 0xcc}, cose_bytes};
  auth_data.extensions = extensions;

  const std::vector<uint8_t> serialized = webauthn::serialize_authenticator_data(auth_data);
  CHECK(serialized.size() == 37u + 16u + 2u + 3u + cose_bytes.size() + extensions.size());
  CHECK(serialized[0] == 0x20);
  CHECK(serialized[31] == 0x3f);
  CHECK(serialized[32] ==
        static_cast<uint8_t>(webauthn::flag_user_present | webauthn::flag_user_verified |
                             webauthn::flag_attested_cred | webauthn::flag_extension_data));
  CHECK(serialized[33] == 0x01);
  CHECK(serialized[34] == 0x02);
  CHECK(serialized[35] == 0x03);
  CHECK(serialized[36] == 0x04);
  CHECK(serialized[37] == 0x90);
  CHECK(serialized[52] == 0x9f);
  CHECK(serialized[53] == 0x00);
  CHECK(serialized[54] == 0x03);
  CHECK(serialized[55] == 0xaa);
  CHECK(serialized[56] == 0xbb);
  CHECK(serialized[57] == 0xcc);

  const auto parsed = webauthn::parse_authenticator_data(serialized);
  CHECK(parsed.has_value());
  if (parsed) {
    CHECK(parsed->rp_id_hash == auth_data.rp_id_hash);
    CHECK(parsed->flags ==
          static_cast<uint8_t>(webauthn::flag_user_present | webauthn::flag_user_verified |
                               webauthn::flag_attested_cred | webauthn::flag_extension_data));
    CHECK(parsed->sign_count == auth_data.sign_count);
    CHECK(parsed->attested.has_value());
    if (parsed->attested) {
      CHECK(parsed->attested->aaguid == auth_data.attested->aaguid);
      CHECK(parsed->attested->credential_id == auth_data.attested->credential_id);
      CHECK(parsed->attested->cose_public_key == auth_data.attested->cose_public_key);
    }
    CHECK(parsed->extensions == extensions);
  }

  CHECK(!webauthn::parse_authenticator_data(std::span<const uint8_t>(serialized.data(), 36u))
             .has_value());

  const std::vector<uint8_t> challenge_create = {0x00, 0x01, 0x02};
  const auto client_create = webauthn::build_client_data(
      webauthn::client_data_type::create, challenge_create, "https://example.com", false);
  CHECK(client_create.json.find("\"type\":\"webauthn.create\"") != std::string::npos);
  CHECK(client_create.json.find("\"challenge\":\"AAEC\"") != std::string::npos);
  CHECK(client_create.json.find("\"origin\":\"https://example.com\"") != std::string::npos);
  CHECK(client_create.json.find("crossOrigin") == std::string::npos);
  std::array<uint8_t, 32> create_hash{};
  (void)mbedtls_sha256(reinterpret_cast<const unsigned char*>(client_create.json.data()),
                       client_create.json.size(), create_hash.data(), 0);
  CHECK(client_create.hash == create_hash);

  const std::vector<uint8_t> challenge_get = {0xff};
  const auto client_get = webauthn::build_client_data(webauthn::client_data_type::get,
                                                      challenge_get, "https://example.com", true);
  CHECK(client_get.json.find("\"type\":\"webauthn.get\"") != std::string::npos);
  CHECK(client_get.json.find("\"challenge\":\"_w\"") != std::string::npos);
  CHECK(client_get.json.find("\"crossOrigin\":true") != std::string::npos);
  std::array<uint8_t, 32> get_hash{};
  (void)mbedtls_sha256(reinterpret_cast<const unsigned char*>(client_get.json.data()),
                       client_get.json.size(), get_hash.data(), 0);
  CHECK(client_get.hash == get_hash);

  const std::array<uint8_t, 32> empty_sha = webauthn::sha256(std::span<const uint8_t>());
  const std::array<uint8_t, 32> expected_empty = {
      0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
      0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
      0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
  };
  CHECK(empty_sha == expected_empty);

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
