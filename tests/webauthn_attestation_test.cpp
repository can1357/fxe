#include <fxe/webauthn.hpp>

#include "runtime/cbor.hpp"

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

  const cbor::value* find_text_key(const cbor::value& container, std::string_view key) {
    const auto& storage = static_cast<const cbor::storage&>(container);
    if (const auto* cmap = std::get_if<cbor::cmap>(&storage))
      return cbor::find(*cmap, key);
    if (const auto* map = std::get_if<cbor::map>(&storage)) {
      const auto it = map->find(std::string(key));
      return it == map->end() ? nullptr : &it->second;
    }
    return nullptr;
  }

} // namespace

int main() {
  webauthn::attestation_object none_obj;
  none_obj.fmt = webauthn::attestation_format::none;
  none_obj.auth_data.assign(70u, 0x5a);
  const std::vector<uint8_t> none_encoded = webauthn::encode_attestation_object(none_obj);
  CHECK(!none_encoded.empty());
  CHECK(none_encoded[0] == 0xa3);
  CHECK(none_encoded.size() > 16u);
  CHECK(none_encoded[1] == 0x63);
  CHECK(none_encoded[2] == 'f');
  CHECK(none_encoded[3] == 'm');
  CHECK(none_encoded[4] == 't');
  CHECK(none_encoded[10] == 0x67);
  CHECK(none_encoded[11] == 'a');
  CHECK(none_encoded[17] == 't');
  CHECK(none_encoded[19] == 0x68);
  CHECK(none_encoded[20] == 'a');
  CHECK(none_encoded[27] == 'a');
  const auto none_decoded = webauthn::decode_attestation_object(none_encoded);
  CHECK(none_decoded.has_value());
  if (none_decoded) {
    CHECK(none_decoded->fmt == webauthn::attestation_format::none);
    CHECK(none_decoded->auth_data == none_obj.auth_data);
    CHECK(none_decoded->att_stmt_cbor == std::vector<uint8_t>({0xa0}));
  }

  const std::vector<uint8_t> packed_stmt = cbor::encode(cbor::value(cbor::cmap{
      {std::string("alg"), cbor::value(int64_t(-7))},
      {std::string("sig"), cbor::value(std::vector<uint8_t>(64u, 0x11))},
  }));
  webauthn::attestation_object packed_obj;
  packed_obj.fmt = webauthn::attestation_format::packed;
  packed_obj.auth_data.assign(70u, 0x11);
  packed_obj.att_stmt_cbor = packed_stmt;
  const std::vector<uint8_t> packed_encoded = webauthn::encode_attestation_object(packed_obj);
  CHECK(!packed_encoded.empty());
  CHECK(packed_encoded[0] == 0xa3);
  const auto packed_decoded = webauthn::decode_attestation_object(packed_encoded);
  CHECK(packed_decoded.has_value());
  if (packed_decoded) {
    CHECK(packed_decoded->fmt == webauthn::attestation_format::packed);
    CHECK(packed_decoded->auth_data == packed_obj.auth_data);
    const auto stmt =
        cbor::decode(packed_decoded->att_stmt_cbor.data(), packed_decoded->att_stmt_cbor.size());
    CHECK(stmt.has_value());
    if (stmt) {
      const auto* alg = find_text_key(*stmt, "alg");
      const auto* sig = find_text_key(*stmt, "sig");
      CHECK(alg != nullptr);
      CHECK(sig != nullptr);
      CHECK(alg && std::get_if<int64_t>(&static_cast<const cbor::storage&>(*alg)) &&
            *std::get_if<int64_t>(&static_cast<const cbor::storage&>(*alg)) == -7);
      CHECK(sig && std::get_if<std::vector<uint8_t>>(&static_cast<const cbor::storage&>(*sig)) &&
            std::get_if<std::vector<uint8_t>>(&static_cast<const cbor::storage&>(*sig))->size() ==
                64u);
    }
  }

  CHECK(!webauthn::decode_attestation_object(std::vector<uint8_t>{0xff, 0x00}).has_value());
  const std::vector<uint8_t> missing_fmt = cbor::encode(cbor::value(cbor::map{
      {"authData", cbor::value(std::vector<uint8_t>{0x00})},
      {"attStmt", cbor::value(cbor::map{})},
  }));
  CHECK(!webauthn::decode_attestation_object(missing_fmt).has_value());
  const std::vector<uint8_t> unknown_fmt = cbor::encode(cbor::value(cbor::map{
      {"fmt", cbor::value(std::string("tpm"))},
      {"authData", cbor::value(std::vector<uint8_t>{0x00})},
      {"attStmt", cbor::value(cbor::map{})},
  }));
  CHECK(!webauthn::decode_attestation_object(unknown_fmt).has_value());

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
