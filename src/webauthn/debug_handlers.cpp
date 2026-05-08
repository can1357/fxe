#include "debug_handlers.hpp"

#include <fxe/debug.hpp>
#include <fxe/webauthn.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/private_access.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fxe::webauthn::debug {
  namespace {
    using json = fxe::debug::json;
    using err_code = fxe::debug::err_code;
    using dispatch_context = fxe::debug::dispatch_context;
    using dispatch_error = fxe::debug::dispatch_error;

    struct pk_guard {
      mbedtls_pk_context ctx;
      pk_guard() {
        mbedtls_pk_init(&ctx);
      }
      ~pk_guard() {
        mbedtls_pk_free(&ctx);
      }
    };

    struct virtual_authenticator_impl_mirror {
      std::map<std::vector<uint8_t>, virtual_credential> credentials;
      mbedtls_ctr_drbg_context ctr_drbg;
      mbedtls_entropy_context entropy;
      bool use_real_entropy = true;
      bool user_verified = true;
      uint64_t deterministic_state = 0xF1E0D1C0ULL;
    };

    struct virtual_authenticator_layout {
      std::unique_ptr<virtual_authenticator_impl_mirror> impl_;
    };

    static_assert(sizeof(virtual_authenticator_layout) == sizeof(virtual_authenticator));
    static_assert(alignof(virtual_authenticator_layout) == alignof(virtual_authenticator));

    struct authenticator_entry {
      virtual_authenticator* authenticator = nullptr;
      bool automatic_presence_simulation = true;
      bool has_resident_key = true;
      bool has_user_verification = true;
      std::string transport = "internal";
    };

    std::atomic<bool> g_enabled{false};
    std::mutex& registry_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<std::string, authenticator_entry>& registry() {
      static std::unordered_map<std::string, authenticator_entry> ids;
      return ids;
    }

    std::atomic<uint64_t>& next_authenticator_id() {
      static std::atomic<uint64_t> next{1};
      return next;
    }

    [[noreturn]] void invalid_params(std::string message) {
      throw dispatch_error{err_code::invalid_params, std::move(message), ""};
    }

    std::string mbedtls_err_str(int rc) {
      std::array<char, 256> buf{};
      mbedtls_strerror(rc, buf.data(), buf.size());
      return buf.data();
    }

    virtual_authenticator_impl_mirror& mirrored_impl(virtual_authenticator& authenticator) {
      // Debug-only escape hatch: Phase 2 needs credential install/remove APIs before
      // the public virtual_authenticator surface grows dedicated mutators.
      auto* layout = reinterpret_cast<virtual_authenticator_layout*>(&authenticator);
      return *layout->impl_;
    }

    std::string base64url_encode(std::span<const uint8_t> bytes) {
      static constexpr char kAlphabet[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      std::string out;
      out.reserve(((bytes.size() + 2u) / 3u) * 4u);
      size_t i = 0;
      while (i + 3u <= bytes.size()) {
        const uint32_t n = (static_cast<uint32_t>(bytes[i]) << 16u) |
                           (static_cast<uint32_t>(bytes[i + 1u]) << 8u) |
                           static_cast<uint32_t>(bytes[i + 2u]);
        out.push_back(kAlphabet[(n >> 18u) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12u) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 6u) & 0x3Fu]);
        out.push_back(kAlphabet[n & 0x3Fu]);
        i += 3u;
      }
      const size_t rem = bytes.size() - i;
      if (rem == 1u) {
        const uint32_t n = static_cast<uint32_t>(bytes[i]) << 16u;
        out.push_back(kAlphabet[(n >> 18u) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12u) & 0x3Fu]);
      } else if (rem == 2u) {
        const uint32_t n =
            (static_cast<uint32_t>(bytes[i]) << 16u) | (static_cast<uint32_t>(bytes[i + 1u]) << 8u);
        out.push_back(kAlphabet[(n >> 18u) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12u) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 6u) & 0x3Fu]);
      }
      return out;
    }

    int base64url_value(char ch) {
      if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
      if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
      if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
      if (ch == '-')
        return 62;
      if (ch == '_')
        return 63;
      return -1;
    }

    std::vector<uint8_t> base64url_decode(std::string_view text) {
      std::vector<uint8_t> out;
      out.reserve((text.size() * 3u) / 4u);
      uint32_t buffer = 0;
      int bits = 0;
      for (char ch : text) {
        if (ch == '=')
          invalid_params("base64url values must be unpadded");
        const int value = base64url_value(ch);
        if (value < 0)
          invalid_params("invalid base64url value");
        buffer = (buffer << 6u) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFFu));
        }
      }
      if (bits == 1)
        invalid_params("invalid base64url length");
      return out;
    }

    std::string require_string(const json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_string())
        invalid_params("missing string field: " + std::string(key));
      return it->get<std::string>();
    }

    bool require_bool(const json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_boolean())
        invalid_params("missing boolean field: " + std::string(key));
      return it->get<bool>();
    }

    const json& require_object(const json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_object())
        invalid_params("missing object field: " + std::string(key));
      return *it;
    }

    uint32_t require_u32(const json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_number_integer())
        invalid_params("missing integer field: " + std::string(key));
      const auto value = it->get<int64_t>();
      if (value < 0 || value > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
        invalid_params("integer field out of range: " + std::string(key));
      return static_cast<uint32_t>(value);
    }

    authenticator_entry resolve_authenticator_entry(std::string_view authenticator_id) {
      std::lock_guard<std::mutex> lock(registry_mutex());
      auto it = registry().find(std::string(authenticator_id));
      if (it == registry().end())
        invalid_params("unknown authenticatorId");
      return it->second;
    }

    std::vector<std::string> authenticator_ids_for(virtual_authenticator* authenticator) {
      std::vector<std::string> out;
      std::lock_guard<std::mutex> lock(registry_mutex());
      for (const auto& [id, entry] : registry()) {
        if (entry.authenticator == authenticator)
          out.push_back(id);
      }
      return out;
    }

    std::vector<uint8_t> pkcs8_der_from_credential(const virtual_credential& credential) {
      pk_guard pk;
      const int setup_rc = mbedtls_pk_setup(&pk.ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
      if (setup_rc != 0)
        invalid_params("failed to initialize key export: " + mbedtls_err_str(setup_rc));
      auto* ec = mbedtls_pk_ec(pk.ctx);
      if (ec == nullptr)
        invalid_params("failed to initialize EC key export");
      int rc = mbedtls_ecp_group_load(&ec->MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
      if (rc == 0)
        rc =
            mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
                                    credential.public_key.x.data(), credential.public_key.x.size());
      if (rc == 0)
        rc =
            mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                                    credential.public_key.y.data(), credential.public_key.y.size());
      if (rc == 0)
        rc = mbedtls_mpi_lset(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1);
      if (rc == 0)
        rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(d), credential.private_key_d.data(),
                                     credential.private_key_d.size());
      if (rc != 0)
        invalid_params("failed to export credential private key: " + mbedtls_err_str(rc));
      std::array<unsigned char, 4096> der{};
      const int written = mbedtls_pk_write_key_der(&pk.ctx, der.data(), der.size());
      if (written < 0)
        invalid_params("failed to export credential private key: " + mbedtls_err_str(written));
      return std::vector<uint8_t>(der.end() - written, der.end());
    }

    virtual_credential credential_from_json(const json& credential_json,
                                            const authenticator_entry& entry) {
      virtual_credential credential;
      credential.credential_id = base64url_decode(require_string(credential_json, "credentialId"));
      credential.resident = require_bool(credential_json, "isResidentCredential");
      credential.rp_id = require_string(credential_json, "rpId");
      credential.sign_count = require_u32(credential_json, "signCount");
      credential.user_verified = mirrored_impl(*entry.authenticator).user_verified;
      credential.public_key.alg = cose_algorithm::es256;
      credential.public_key.crv = 1;

      if (auto it = credential_json.find("userHandle");
          it != credential_json.end() && !it->is_null()) {
        if (!it->is_string())
          invalid_params("userHandle must be a base64url string");
        credential.user.id = base64url_decode(it->get<std::string>());
      }

      const auto private_key_der = base64url_decode(require_string(credential_json, "privateKey"));
      pk_guard pk;
      auto& rng = mirrored_impl(default_virtual_authenticator()).ctr_drbg;
      const int parse_rc =
          mbedtls_pk_parse_key(&pk.ctx, private_key_der.data(), private_key_der.size(), nullptr, 0,
                               mbedtls_ctr_drbg_random, &rng);
      if (parse_rc != 0)
        invalid_params("invalid PKCS#8 private key: " + mbedtls_err_str(parse_rc));
      const auto type = mbedtls_pk_get_type(&pk.ctx);
      if (type != MBEDTLS_PK_ECKEY && type != MBEDTLS_PK_ECKEY_DH)
        invalid_params("privateKey must be a P-256 EC key");
      auto* ec = mbedtls_pk_ec(pk.ctx);
      if (ec == nullptr || ec->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1)
        invalid_params("privateKey must be a P-256 EC key");

      credential.private_key_d.resize(32u);
      int rc = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(d), credential.private_key_d.data(),
                                        credential.private_key_d.size());
      if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
                                      credential.public_key.x.data(),
                                      credential.public_key.x.size());
      }
      if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                                      credential.public_key.y.data(),
                                      credential.public_key.y.size());
      }
      if (rc != 0)
        invalid_params("failed to decode privateKey: " + mbedtls_err_str(rc));

      const auto rp_hash = sha256(std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(credential.rp_id.data()), credential.rp_id.size()));
      credential.rp_id_hash.assign(rp_hash.begin(), rp_hash.end());
      return credential;
    }

    json credential_to_json(const virtual_credential& credential) {
      json out{json::object()};
      out["credentialId"] = base64url_encode(credential.credential_id);
      out["isResidentCredential"] = credential.resident;
      out["rpId"] = credential.rp_id;
      out["privateKey"] = base64url_encode(pkcs8_der_from_credential(credential));
      out["signCount"] = credential.sign_count;
      if (credential.resident && !credential.user.id.empty())
        out["userHandle"] = base64url_encode(credential.user.id);
      return out;
    }

    std::optional<virtual_credential> find_credential(const virtual_authenticator& authenticator,
                                                      std::span<const uint8_t> credential_id) {
      for (const auto& credential : authenticator.list_credentials()) {
        if (credential.credential_id.size() == credential_id.size() &&
            std::equal(credential.credential_id.begin(), credential.credential_id.end(),
                       credential_id.begin()))
          return credential;
      }
      return std::nullopt;
    }

    void emit_event(std::string_view method, const std::string& authenticator_id,
                    const virtual_credential& credential) {
      if (!g_enabled.load(std::memory_order_acquire))
        return;
      auto* srv = fxe::debug::active_server();
      if (!srv)
        return;
      json params{json::object()};
      params["authenticatorId"] = authenticator_id;
      params["credential"] = credential_to_json(credential);
      srv->emit_event(method, std::move(params));
    }

    json h_enable(dispatch_context&, const json&) {
      g_enabled.store(true, std::memory_order_release);
      return json{json::object()};
    }

    json h_disable(dispatch_context&, const json&) {
      g_enabled.store(false, std::memory_order_release);
      std::lock_guard<std::mutex> lock(registry_mutex());
      registry().clear();
      return json{json::object()};
    }

    json h_add_virtual_authenticator(dispatch_context&, const json& params) {
      const json& options = require_object(params, "options");
      authenticator_entry entry;
      entry.authenticator = &default_virtual_authenticator();
      entry.transport = options.value("transport", std::string("internal"));
      entry.has_resident_key = options.value("hasResidentKey", true);
      entry.has_user_verification = options.value("hasUserVerification", true);
      entry.automatic_presence_simulation = options.value("automaticPresenceSimulation", true);
      const bool is_user_verified = options.value("isUserVerified", true);
      entry.authenticator->set_user_verified(is_user_verified);

      const std::string authenticator_id = std::to_string(next_authenticator_id().fetch_add(1));
      {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[authenticator_id] = entry;
      }
      return json{{"authenticatorId", authenticator_id}};
    }

    json h_remove_virtual_authenticator(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      std::lock_guard<std::mutex> lock(registry_mutex());
      if (registry().erase(authenticator_id) == 0)
        invalid_params("unknown authenticatorId");
      return json{json::object()};
    }

    json h_add_credential(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const auto entry = resolve_authenticator_entry(authenticator_id);
      const auto credential = credential_from_json(require_object(params, "credential"), entry);
      auto& impl = mirrored_impl(*entry.authenticator);
      impl.credentials[credential.credential_id] = credential;
      emit_event("WebAuthn.credentialAdded", authenticator_id, credential);
      return json{json::object()};
    }

    json h_get_credential(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const auto entry = resolve_authenticator_entry(authenticator_id);
      const auto credential_id = base64url_decode(require_string(params, "credentialId"));
      const auto credential = find_credential(*entry.authenticator, credential_id);
      if (!credential)
        invalid_params("unknown credentialId");
      return json{{"credential", credential_to_json(*credential)}};
    }

    json h_get_credentials(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const auto entry = resolve_authenticator_entry(authenticator_id);
      json credentials{json::array()};
      for (const auto& credential : entry.authenticator->list_credentials())
        credentials.push_back(credential_to_json(credential));
      return json{{"credentials", std::move(credentials)}};
    }

    json h_remove_credential(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const auto entry = resolve_authenticator_entry(authenticator_id);
      const auto credential_id = base64url_decode(require_string(params, "credentialId"));
      auto& impl = mirrored_impl(*entry.authenticator);
      if (impl.credentials.erase(credential_id) == 0)
        invalid_params("unknown credentialId");
      return json{json::object()};
    }

    json h_clear_credentials(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const auto entry = resolve_authenticator_entry(authenticator_id);
      entry.authenticator->clear();
      return json{json::object()};
    }

    json h_set_user_verified(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const auto entry = resolve_authenticator_entry(authenticator_id);
      entry.authenticator->set_user_verified(require_bool(params, "isUserVerified"));
      return json{json::object()};
    }

    json h_set_automatic_presence_simulation(dispatch_context&, const json& params) {
      const auto authenticator_id = require_string(params, "authenticatorId");
      const bool enabled = require_bool(params, "enabled");
      std::lock_guard<std::mutex> lock(registry_mutex());
      auto it = registry().find(authenticator_id);
      if (it == registry().end())
        invalid_params("unknown authenticatorId");
      // Phase 2 always simulates user presence; retain the flag for wire compatibility.
      it->second.automatic_presence_simulation = enabled;
      return json{json::object()};
    }

    constexpr std::array<std::string_view, 11> kCapabilities = {
        "WebAuthn.enable",
        "WebAuthn.disable",
        "WebAuthn.addVirtualAuthenticator",
        "WebAuthn.removeVirtualAuthenticator",
        "WebAuthn.addCredential",
        "WebAuthn.getCredential",
        "WebAuthn.getCredentials",
        "WebAuthn.removeCredential",
        "WebAuthn.clearCredentials",
        "WebAuthn.setUserVerified",
        "WebAuthn.setAutomaticPresenceSimulation",
    };

    constexpr std::array<registered_method, 11> kHandlers = {{
        {"WebAuthn.enable", &h_enable},
        {"WebAuthn.disable", &h_disable},
        {"WebAuthn.addVirtualAuthenticator", &h_add_virtual_authenticator},
        {"WebAuthn.removeVirtualAuthenticator", &h_remove_virtual_authenticator},
        {"WebAuthn.addCredential", &h_add_credential},
        {"WebAuthn.getCredential", &h_get_credential},
        {"WebAuthn.getCredentials", &h_get_credentials},
        {"WebAuthn.removeCredential", &h_remove_credential},
        {"WebAuthn.clearCredentials", &h_clear_credentials},
        {"WebAuthn.setUserVerified", &h_set_user_verified},
        {"WebAuthn.setAutomaticPresenceSimulation", &h_set_automatic_presence_simulation},
    }};
  } // namespace

  std::span<const registered_method> handler_table() {
    return kHandlers;
  }

  std::span<const std::string_view> schema_capabilities() {
    return kCapabilities;
  }

  void on_credential_registered(std::string_view, const register_response&,
                                const virtual_credential& credential) {
    for (const auto& authenticator_id : authenticator_ids_for(&default_virtual_authenticator()))
      emit_event("WebAuthn.credentialAdded", authenticator_id, credential);
  }

  void on_credential_asserted(std::string_view, const assert_response&,
                              const virtual_credential& credential) {
    for (const auto& authenticator_id : authenticator_ids_for(&default_virtual_authenticator()))
      emit_event("WebAuthn.credentialAsserted", authenticator_id, credential);
  }

  void register_webauthn_dispatch_handlers() {}
} // namespace fxe::webauthn::debug
