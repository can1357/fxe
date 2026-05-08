#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fxe::webauthn {

  enum class cose_algorithm : int { es256 = -7, eddsa = -8, rs256 = -257 };

  struct cose_ec2_key {
    cose_algorithm alg = cose_algorithm::es256;
    int crv = 1;
    std::array<uint8_t, 32> x{};
    std::array<uint8_t, 32> y{};
  };

  std::vector<uint8_t> encode_cose_ec2(const cose_ec2_key& k);
  std::optional<cose_ec2_key> decode_cose_ec2(std::span<const uint8_t> bytes);

  enum class client_data_type { create, get };

  struct client_data {
    std::string json;
    std::array<uint8_t, 32> hash{};
  };

  client_data build_client_data(client_data_type type, std::span<const uint8_t> challenge,
                                std::string_view origin, bool cross_origin = false);

  std::array<uint8_t, 32> sha256(std::span<const uint8_t> bytes);

  struct attested_credential_data {
    std::array<uint8_t, 16> aaguid{};
    std::vector<uint8_t> credential_id;
    std::vector<uint8_t> cose_public_key;
  };

  struct authenticator_data {
    std::array<uint8_t, 32> rp_id_hash{};
    uint8_t flags = 0;
    uint32_t sign_count = 0;
    std::optional<attested_credential_data> attested;
    std::vector<uint8_t> extensions;
  };

  inline constexpr uint8_t flag_user_present = 0x01;
  inline constexpr uint8_t flag_user_verified = 0x04;
  inline constexpr uint8_t flag_backup_eligible = 0x08;
  inline constexpr uint8_t flag_backup_state = 0x10;
  inline constexpr uint8_t flag_attested_cred = 0x40;
  inline constexpr uint8_t flag_extension_data = 0x80;

  std::vector<uint8_t> serialize_authenticator_data(const authenticator_data& d);
  std::optional<authenticator_data> parse_authenticator_data(std::span<const uint8_t> bytes);

  enum class attestation_format { none, packed };

  struct attestation_object {
    attestation_format fmt = attestation_format::none;
    std::vector<uint8_t> auth_data;
    std::vector<uint8_t> att_stmt_cbor;
  };

  std::vector<uint8_t> encode_attestation_object(const attestation_object& a);
  std::optional<attestation_object> decode_attestation_object(std::span<const uint8_t> bytes);

  struct rp_id_policy {
    std::vector<std::string> allowed;
    bool allow_any = false;
  };

  bool validate_rp_id(std::string_view rp_id, std::string_view origin_host,
                      const rp_id_policy& policy);

  bool is_valid_rp_id_syntax(std::string_view rp_id);
  bool host_matches_rp_id(std::string_view origin_host, std::string_view rp_id);

  struct user_entity {
    std::vector<uint8_t> id;
    std::string name;
    std::string display_name;
  };

  struct creation_options {
    std::string rp_id;
    std::string rp_name;
    user_entity user;
    std::vector<uint8_t> challenge;
    std::vector<cose_algorithm> pub_key_params;
    std::vector<std::vector<uint8_t>> exclude_credentials;
    std::string authenticator_attachment;
    std::string user_verification;
    std::string resident_key;
    std::string attestation;
    uint64_t timeout_ms = 0;
  };

  struct request_options {
    std::string rp_id;
    std::vector<uint8_t> challenge;
    std::vector<std::vector<uint8_t>> allow_credentials;
    std::string user_verification;
    uint64_t timeout_ms = 0;
  };

  std::string validate_creation_options(const creation_options& o);
  std::string validate_request_options(const request_options& o);

  struct virtual_credential {
    std::vector<uint8_t> credential_id;
    std::vector<uint8_t> rp_id_hash;
    std::string rp_id;
    user_entity user;
    std::vector<uint8_t> private_key_d;
    cose_ec2_key public_key;
    uint32_t sign_count = 0;
    bool user_verified = true;
    bool resident = true;
  };

  struct register_response {
    std::vector<uint8_t> credential_id;
    std::vector<uint8_t> attestation_object;
    std::vector<uint8_t> client_data_json;
    std::vector<uint8_t> public_key;
    cose_algorithm algorithm = cose_algorithm::es256;
  };

  struct assert_response {
    std::vector<uint8_t> credential_id;
    std::vector<uint8_t> authenticator_data;
    std::vector<uint8_t> client_data_json;
    std::vector<uint8_t> signature;
    std::vector<uint8_t> user_handle;
  };

  class virtual_authenticator {
  public:
    static const std::array<uint8_t, 16>& aaguid();

    virtual_authenticator();
    explicit virtual_authenticator(uint64_t deterministic_seed);
    ~virtual_authenticator();
    virtual_authenticator(const virtual_authenticator&) = delete;
    virtual_authenticator& operator=(const virtual_authenticator&) = delete;

    std::string register_credential(const creation_options& opts, std::string_view origin,
                                    register_response& out);
    std::string assert_credential(const request_options& opts, std::string_view origin,
                                  assert_response& out);

    std::vector<virtual_credential> list_credentials() const;
    void clear();
    bool set_user_verified(bool verified);

    static bool verify_es256(const cose_ec2_key& pub, std::span<const uint8_t> message,
                             std::span<const uint8_t> signature_der);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
  };

  // Platform backend that drives real authenticators (Touch ID / Face ID /
  // Windows Hello / USB security keys / etc.). Selected per OS:
  //   macOS  → ASAuthorizationController (AuthenticationServices.framework)
  //   Win32  → webauthn.dll (runtime-bound; requires API v1+)
  //   Linux  → libfido2 over USB HID (when FXE_HAS_LIBFIDO2)
  //
  // Threading contract: register_credential / assert_credential are blocking
  // and may take seconds (user gesture). On macOS they MUST be called from a
  // worker thread because the underlying delegate fires on the main queue;
  // calling from main would deadlock. cancel() is safe from any thread.
  class platform_authenticator {
  public:
    // True when a backend is compiled in for this OS.
    static bool is_available();
    // True when the OS reports a user-verifying platform authenticator
    // (TouchID/FaceID/Windows Hello). Polls the OS each call.
    static bool is_user_verifying_platform_available();
    // Returns nullptr when is_available() is false.
    static std::unique_ptr<platform_authenticator> create();

    virtual ~platform_authenticator() = default;
    platform_authenticator(const platform_authenticator&) = delete;
    platform_authenticator& operator=(const platform_authenticator&) = delete;

    virtual std::string_view backend_name() const = 0;
    virtual std::string register_credential(const creation_options& opts, std::string_view origin,
                                            register_response& out) = 0;
    virtual std::string assert_credential(const request_options& opts, std::string_view origin,
                                          assert_response& out) = 0;
    virtual void cancel() = 0;

  protected:
    platform_authenticator() = default;
  };
} // namespace fxe::webauthn
