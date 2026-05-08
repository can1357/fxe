#include <fxe/webauthn.hpp>

#include "runtime/cbor.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace fxe::webauthn {
  namespace cbor = fxe::runtime::cbor;

  namespace {

    void append_u16_be(std::vector<uint8_t>& out, uint16_t value) {
      out.push_back(static_cast<uint8_t>(value >> 8));
      out.push_back(static_cast<uint8_t>(value));
    }

    void append_u32_be(std::vector<uint8_t>& out, uint32_t value) {
      out.push_back(static_cast<uint8_t>(value >> 24));
      out.push_back(static_cast<uint8_t>(value >> 16));
      out.push_back(static_cast<uint8_t>(value >> 8));
      out.push_back(static_cast<uint8_t>(value));
    }

    std::optional<std::pair<cbor::value, std::vector<uint8_t>>>
    parse_cbor_prefix(std::span<const uint8_t> bytes) {
      for (size_t len = 1; len <= bytes.size(); ++len) {
        const auto decoded = cbor::decode(bytes.data(), len);
        if (!decoded)
          continue;
        auto canonical = cbor::encode(*decoded);
        if (canonical.size() == len)
          return std::pair<cbor::value, std::vector<uint8_t>>{std::move(*decoded),
                                                              std::move(canonical)};
      }
      return std::nullopt;
    }

  } // namespace

  std::vector<uint8_t> serialize_authenticator_data(const authenticator_data& d) {
    std::vector<uint8_t> out;
    size_t reserve = 37u + d.extensions.size();
    uint8_t flags = d.flags;
    if (d.attested)
      flags = static_cast<uint8_t>(flags | flag_attested_cred);
    if (!d.extensions.empty())
      flags = static_cast<uint8_t>(flags | flag_extension_data);
    if (d.attested) {
      reserve += 18u + d.attested->credential_id.size() + d.attested->cose_public_key.size();
    }
    out.reserve(reserve);
    out.insert(out.end(), d.rp_id_hash.begin(), d.rp_id_hash.end());
    out.push_back(flags);
    append_u32_be(out, d.sign_count);
    if (d.attested) {
      out.insert(out.end(), d.attested->aaguid.begin(), d.attested->aaguid.end());
      append_u16_be(out, static_cast<uint16_t>(d.attested->credential_id.size()));
      out.insert(out.end(), d.attested->credential_id.begin(), d.attested->credential_id.end());
      out.insert(out.end(), d.attested->cose_public_key.begin(), d.attested->cose_public_key.end());
    }
    out.insert(out.end(), d.extensions.begin(), d.extensions.end());
    return out;
  }

  std::optional<authenticator_data> parse_authenticator_data(std::span<const uint8_t> bytes) {
    if (bytes.size() < 37u)
      return std::nullopt;

    authenticator_data out;
    for (size_t i = 0; i < out.rp_id_hash.size(); ++i)
      out.rp_id_hash[i] = bytes[i];
    out.flags = bytes[32];
    out.sign_count = (static_cast<uint32_t>(bytes[33]) << 24) |
                     (static_cast<uint32_t>(bytes[34]) << 16) |
                     (static_cast<uint32_t>(bytes[35]) << 8) | static_cast<uint32_t>(bytes[36]);

    size_t offset = 37;
    if ((out.flags & flag_attested_cred) != 0u) {
      if (bytes.size() < offset + 18u)
        return std::nullopt;
      attested_credential_data attested;
      for (size_t i = 0; i < attested.aaguid.size(); ++i)
        attested.aaguid[i] = bytes[offset + i];
      offset += attested.aaguid.size();
      const uint16_t credential_id_len = static_cast<uint16_t>(
          (static_cast<uint16_t>(bytes[offset]) << 8) | static_cast<uint16_t>(bytes[offset + 1]));
      offset += 2;
      if (bytes.size() < offset + credential_id_len)
        return std::nullopt;
      attested.credential_id.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                    bytes.begin() +
                                        static_cast<std::ptrdiff_t>(offset + credential_id_len));
      offset += credential_id_len;
      const auto cose = parse_cbor_prefix(bytes.subspan(offset));
      if (!cose)
        return std::nullopt;
      attested.cose_public_key = cose->second;
      offset += attested.cose_public_key.size();
      out.attested = std::move(attested);
    }

    if ((out.flags & flag_extension_data) != 0u) {
      const auto extensions = parse_cbor_prefix(bytes.subspan(offset));
      if (!extensions)
        return std::nullopt;
      out.extensions = extensions->second;
      offset += out.extensions.size();
    }

    if (offset != bytes.size())
      return std::nullopt;
    return out;
  }

} // namespace fxe::webauthn
