#include <fxe/webauthn.hpp>

#include "runtime/cbor.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fxe::webauthn {
  namespace cbor = fxe::runtime::cbor;

  namespace {

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

    std::optional<std::string> decode_text(const cbor::value* value) {
      if (!value)
        return std::nullopt;
      const auto* text = std::get_if<std::string>(&static_cast<const cbor::storage&>(*value));
      if (!text)
        return std::nullopt;
      return *text;
    }

    std::optional<std::vector<uint8_t>> decode_bytes(const cbor::value* value) {
      if (!value)
        return std::nullopt;
      const auto* bytes =
          std::get_if<std::vector<uint8_t>>(&static_cast<const cbor::storage&>(*value));
      if (!bytes)
        return std::nullopt;
      return *bytes;
    }

  } // namespace

  std::vector<uint8_t> encode_cose_ec2(const cose_ec2_key& k) {
    if (k.crv != 1)
      return {};
    const cbor::cmap cose_key = {
        {int64_t(1), cbor::value(uint64_t(2))},
        {int64_t(3), cbor::value(static_cast<int64_t>(k.alg))},
        {int64_t(-1), cbor::value(uint64_t(1))},
        {int64_t(-2), cbor::value(std::vector<uint8_t>(k.x.begin(), k.x.end()))},
        {int64_t(-3), cbor::value(std::vector<uint8_t>(k.y.begin(), k.y.end()))},
    };
    return cbor::encode(cbor::value(cose_key));
  }

  std::optional<cose_ec2_key> decode_cose_ec2(std::span<const uint8_t> bytes) {
    const auto decoded = cbor::decode(bytes.data(), bytes.size());
    if (!decoded)
      return std::nullopt;
    const auto* cmap = std::get_if<cbor::cmap>(&static_cast<const cbor::storage&>(*decoded));
    if (!cmap)
      return std::nullopt;

    const auto* kty = cbor::find(*cmap, int64_t(1));
    const auto* alg = cbor::find(*cmap, int64_t(3));
    const auto* crv = cbor::find(*cmap, int64_t(-1));
    const auto* x = cbor::find(*cmap, int64_t(-2));
    const auto* y = cbor::find(*cmap, int64_t(-3));
    if (!kty || !crv || !x || !y)
      return std::nullopt;

    const auto* kty_value = std::get_if<uint64_t>(&static_cast<const cbor::storage&>(*kty));
    if (!kty_value || *kty_value != 2u)
      return std::nullopt;

    const auto* crv_value = std::get_if<uint64_t>(&static_cast<const cbor::storage&>(*crv));
    if (!crv_value || *crv_value != 1u)
      return std::nullopt;

    const auto* x_bytes = std::get_if<std::vector<uint8_t>>(&static_cast<const cbor::storage&>(*x));
    const auto* y_bytes = std::get_if<std::vector<uint8_t>>(&static_cast<const cbor::storage&>(*y));
    if (!x_bytes || !y_bytes || x_bytes->size() != 32u || y_bytes->size() != 32u)
      return std::nullopt;

    cose_ec2_key out;
    out.crv = 1;
    out.alg = cose_algorithm::es256;
    if (alg) {
      if (const auto* alg_value = std::get_if<int64_t>(&static_cast<const cbor::storage&>(*alg))) {
        if (*alg_value != static_cast<int64_t>(cose_algorithm::es256))
          return std::nullopt;
      } else {
        return std::nullopt;
      }
    }
    for (size_t i = 0; i < 32u; ++i) {
      out.x[i] = (*x_bytes)[i];
      out.y[i] = (*y_bytes)[i];
    }
    return out;
  }

  std::vector<uint8_t> encode_attestation_object(const attestation_object& a) {
    std::string fmt;
    switch (a.fmt) {
    case attestation_format::none:
      fmt = "none";
      break;
    case attestation_format::packed:
      fmt = "packed";
      break;
    default:
      return {};
    }

    cbor::value att_stmt_value = cbor::cmap{};
    if (!a.att_stmt_cbor.empty()) {
      const auto decoded = cbor::decode(a.att_stmt_cbor.data(), a.att_stmt_cbor.size());
      if (!decoded)
        return {};
      att_stmt_value = *decoded;
    }

    const cbor::cmap outer = {
        {std::string("fmt"), cbor::value(fmt)},
        {std::string("authData"), cbor::value(a.auth_data)},
        {std::string("attStmt"), std::move(att_stmt_value)},
    };
    return cbor::encode(cbor::value(outer));
  }

  std::optional<attestation_object> decode_attestation_object(std::span<const uint8_t> bytes) {
    const auto decoded = cbor::decode(bytes.data(), bytes.size());
    if (!decoded)
      return std::nullopt;

    const auto& storage = static_cast<const cbor::storage&>(*decoded);
    if (!std::holds_alternative<cbor::cmap>(storage) && !std::holds_alternative<cbor::map>(storage))
      return std::nullopt;

    const auto fmt = decode_text(find_text_key(*decoded, "fmt"));
    const auto auth_data = decode_bytes(find_text_key(*decoded, "authData"));
    const auto* att_stmt = find_text_key(*decoded, "attStmt");
    if (!fmt || !auth_data || !att_stmt)
      return std::nullopt;

    attestation_object out;
    if (*fmt == "none") {
      out.fmt = attestation_format::none;
    } else if (*fmt == "packed") {
      out.fmt = attestation_format::packed;
    } else {
      return std::nullopt;
    }
    out.auth_data = *auth_data;
    out.att_stmt_cbor = cbor::encode(*att_stmt);
    return out;
  }

} // namespace fxe::webauthn
