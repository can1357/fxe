#include <fxe/webauthn.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/private_access.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fxe::webauthn {
  namespace {

    struct ecdsa_guard {
      mbedtls_ecdsa_context ctx;
      ecdsa_guard() {
        mbedtls_ecdsa_init(&ctx);
      }
      ~ecdsa_guard() {
        mbedtls_ecdsa_free(&ctx);
      }
    };

    struct pk_guard {
      mbedtls_pk_context ctx;
      pk_guard() {
        mbedtls_pk_init(&ctx);
      }
      ~pk_guard() {
        mbedtls_pk_free(&ctx);
      }
    };

    std::string mbedtls_err_str(int rc) {
      std::array<char, 256> buf{};
      mbedtls_strerror(rc, buf.data(), buf.size());
      return buf.data();
    }

    std::optional<std::string> origin_host(std::string_view origin) {
      const size_t scheme = origin.find("://");
      if (scheme == std::string_view::npos)
        return std::nullopt;
      const size_t host_begin = scheme + 3u;
      if (host_begin >= origin.size())
        return std::nullopt;
      const size_t host_end = origin.find_first_of("/:?#", host_begin);
      std::string_view host_port = origin.substr(host_begin, host_end - host_begin);
      if (host_port.empty())
        return std::nullopt;
      const size_t colon = host_port.find(':');
      const std::string_view host = host_port.substr(0, colon);
      if (host.empty())
        return std::nullopt;
      return std::string(host);
    }

    bool contains_es256(const std::vector<cose_algorithm>& algorithms) {
      return std::find(algorithms.begin(), algorithms.end(), cose_algorithm::es256) !=
             algorithms.end();
    }

    int deterministic_entropy(void* data, unsigned char* output, size_t len) {
      auto* state = static_cast<uint64_t*>(data);
      for (size_t i = 0; i < len; ++i) {
        uint64_t x = *state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        *state = x;
        const uint64_t mixed = x * 0x2545F4914F6CDD1DULL;
        output[i] = static_cast<unsigned char>(mixed >> ((i % 8u) * 8u));
      }
      return 0;
    }

    bool fill_random(mbedtls_ctr_drbg_context& ctr_drbg, std::span<uint8_t> bytes,
                     std::string& error) {
      const int rc =
          mbedtls_ctr_drbg_random(&ctr_drbg, bytes.data(), static_cast<size_t>(bytes.size()));
      if (rc != 0) {
        error = "rng failed: " + mbedtls_err_str(rc);
        return false;
      }
      return true;
    }

    bool load_public_key(mbedtls_ecdsa_context& ctx, const cose_ec2_key& pub) {
      if (pub.crv != 1)
        return false;
      int rc = mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
      if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), pub.x.data(),
                                     pub.x.size());
      }
      if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), pub.y.data(),
                                     pub.y.size());
      }
      if (rc == 0)
        rc = mbedtls_mpi_lset(&ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1);
      return rc == 0;
    }

    bool load_private_key(mbedtls_ecdsa_context& ctx, const virtual_credential& cred) {
      if (!load_public_key(ctx, cred.public_key))
        return false;
      return mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(d), cred.private_key_d.data(),
                                     cred.private_key_d.size()) == 0;
    }

    std::vector<uint8_t> spki_der_from_keypair(const mbedtls_ecdsa_context& key) {
      pk_guard pk;
      if (mbedtls_pk_setup(&pk.ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
        return {};
      auto* ec = mbedtls_pk_ec(pk.ctx);
      if (ec == nullptr)
        return {};
      int rc = mbedtls_ecp_group_copy(&ec->MBEDTLS_PRIVATE(grp), &key.MBEDTLS_PRIVATE(grp));
      if (rc == 0) {
        rc = mbedtls_ecp_copy(&ec->MBEDTLS_PRIVATE(Q), &key.MBEDTLS_PRIVATE(Q));
      }
      if (rc == 0) {
        rc = mbedtls_mpi_copy(&ec->MBEDTLS_PRIVATE(d), &key.MBEDTLS_PRIVATE(d));
      }
      if (rc != 0)
        return {};

      std::array<unsigned char, 256> buf{};
      const int written = mbedtls_pk_write_pubkey_der(&pk.ctx, buf.data(), buf.size());
      if (written < 0)
        return {};
      return std::vector<uint8_t>(buf.end() - written, buf.end());
    }

  } // namespace

  struct virtual_authenticator::impl {
    std::map<std::vector<uint8_t>, virtual_credential> credentials;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool use_real_entropy = true;
    bool user_verified = true;
    uint64_t deterministic_state = 0xF1E0D1C0ULL;

    explicit impl(uint64_t seed) {
      mbedtls_ctr_drbg_init(&ctr_drbg);
      mbedtls_entropy_init(&entropy);
      static const unsigned char personalization[] = "fxe-virt";
      if (seed != 0u) {
        use_real_entropy = false;
        deterministic_state = seed;
        (void)mbedtls_ctr_drbg_seed(&ctr_drbg, deterministic_entropy, &deterministic_state,
                                    personalization, sizeof(personalization) - 1u);
      } else {
        use_real_entropy = true;
        (void)mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, personalization,
                                    sizeof(personalization) - 1u);
      }
    }

    ~impl() {
      mbedtls_ctr_drbg_free(&ctr_drbg);
      mbedtls_entropy_free(&entropy);
    }
  };

  const std::array<uint8_t, 16>& virtual_authenticator::aaguid() {
    static const std::array<uint8_t, 16> k_aaguid = {'F', 'X', 'E', 'V', 'I', 'R', 'T', 0,
                                                     0,   0,   0,   0,   0,   0,   0,   0};
    return k_aaguid;
  }

  virtual_authenticator::virtual_authenticator() : impl_(std::make_unique<impl>(0)) {}

  virtual_authenticator::virtual_authenticator(uint64_t deterministic_seed)
      : impl_(std::make_unique<impl>(deterministic_seed)) {}

  virtual_authenticator::~virtual_authenticator() = default;

  std::string virtual_authenticator::register_credential(const creation_options& opts,
                                                         std::string_view origin,
                                                         register_response& out) {
    if (std::string error = validate_creation_options(opts); !error.empty())
      return error;
    if (!contains_es256(opts.pub_key_params))
      return "unsupported algorithm";
    const auto host = origin_host(origin);
    if (!host || !host_matches_rp_id(*host, opts.rp_id))
      return "origin host does not match rp.id";

    const client_data client =
        build_client_data(client_data_type::create, opts.challenge, origin, false);

    ecdsa_guard key;
    int rc = mbedtls_ecdsa_genkey(&key.ctx, MBEDTLS_ECP_DP_SECP256R1, mbedtls_ctr_drbg_random,
                                  &impl_->ctr_drbg);
    if (rc != 0)
      return "key generation failed: " + mbedtls_err_str(rc);

    cose_ec2_key pub;
    rc = mbedtls_mpi_write_binary(&key.ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), pub.x.data(),
                                  pub.x.size());
    if (rc == 0) {
      rc = mbedtls_mpi_write_binary(&key.ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), pub.y.data(),
                                    pub.y.size());
    }
    if (rc != 0)
      return "public key export failed: " + mbedtls_err_str(rc);

    std::vector<uint8_t> private_key_d(32u);
    rc = mbedtls_mpi_write_binary(&key.ctx.MBEDTLS_PRIVATE(d), private_key_d.data(),
                                  private_key_d.size());
    if (rc != 0)
      return "private key export failed: " + mbedtls_err_str(rc);

    std::vector<uint8_t> credential_id(32u);
    std::string rng_error;
    if (!fill_random(impl_->ctr_drbg, credential_id, rng_error))
      return rng_error;

    const auto rp_hash_array = sha256(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(opts.rp_id.data()), opts.rp_id.size()));
    const std::vector<uint8_t> cose_public_key = encode_cose_ec2(pub);
    if (cose_public_key.empty())
      return "failed to encode public key";

    attested_credential_data attested;
    attested.aaguid = aaguid();
    attested.credential_id = credential_id;
    attested.cose_public_key = cose_public_key;

    authenticator_data auth_data;
    auth_data.rp_id_hash = rp_hash_array;
    auth_data.flags = flag_user_present;
    if (impl_->user_verified)
      auth_data.flags = static_cast<uint8_t>(auth_data.flags | flag_user_verified);
    auth_data.flags = static_cast<uint8_t>(auth_data.flags | flag_attested_cred);
    auth_data.sign_count = 0;
    auth_data.attested = attested;

    attestation_object attestation;
    attestation.fmt = attestation_format::none;
    attestation.auth_data = serialize_authenticator_data(auth_data);
    if (attestation.auth_data.empty())
      return "failed to serialize authenticator data";
    out.attestation_object = encode_attestation_object(attestation);
    if (out.attestation_object.empty())
      return "failed to encode attestation object";

    virtual_credential credential;
    credential.credential_id = credential_id;
    credential.rp_id_hash.assign(rp_hash_array.begin(), rp_hash_array.end());
    credential.rp_id = opts.rp_id;
    credential.user = opts.user;
    credential.private_key_d = private_key_d;
    credential.public_key = pub;
    credential.sign_count = 0;
    credential.user_verified = impl_->user_verified;
    credential.resident = true;
    impl_->credentials[credential_id] = credential;

    out.credential_id = credential_id;
    out.client_data_json.assign(client.json.begin(), client.json.end());
    out.public_key = spki_der_from_keypair(key.ctx);
    if (out.public_key.empty())
      return "failed to encode public key";
    out.algorithm = cose_algorithm::es256;
    return {};
  }

  std::string virtual_authenticator::assert_credential(const request_options& opts,
                                                       std::string_view origin,
                                                       assert_response& out) {
    if (std::string error = validate_request_options(opts); !error.empty())
      return error;
    const auto host = origin_host(origin);
    if (!host || !host_matches_rp_id(*host, opts.rp_id))
      return "origin host does not match rp.id";

    const client_data client =
        build_client_data(client_data_type::get, opts.challenge, origin, false);

    virtual_credential* credential = nullptr;
    if (!opts.allow_credentials.empty()) {
      for (const auto& id : opts.allow_credentials) {
        const auto it = impl_->credentials.find(id);
        if (it != impl_->credentials.end() && it->second.rp_id == opts.rp_id) {
          credential = &it->second;
          break;
        }
      }
    } else {
      for (auto& [id, entry] : impl_->credentials) {
        (void)id;
        if (entry.resident && entry.rp_id == opts.rp_id) {
          credential = &entry;
          break;
        }
      }
    }
    if (credential == nullptr)
      return "no credential available";

    credential->sign_count += 1;

    authenticator_data auth_data;
    for (size_t i = 0; i < auth_data.rp_id_hash.size(); ++i)
      auth_data.rp_id_hash[i] = credential->rp_id_hash[i];
    auth_data.flags = flag_user_present;
    if (impl_->user_verified)
      auth_data.flags = static_cast<uint8_t>(auth_data.flags | flag_user_verified);
    auth_data.sign_count = credential->sign_count;
    out.authenticator_data = serialize_authenticator_data(auth_data);

    std::vector<uint8_t> signed_message = out.authenticator_data;
    signed_message.insert(signed_message.end(), client.hash.begin(), client.hash.end());
    const auto digest = sha256(signed_message);

    ecdsa_guard signer;
    if (!load_private_key(signer.ctx, *credential))
      return "failed to load credential key";

    std::array<unsigned char, 128> signature_buf{};
    size_t signature_len = 0;
    const int rc = mbedtls_ecdsa_write_signature(
        &signer.ctx, MBEDTLS_MD_SHA256, digest.data(), digest.size(), signature_buf.data(),
        signature_buf.size(), &signature_len, mbedtls_ctr_drbg_random, &impl_->ctr_drbg);
    if (rc != 0)
      return "signature failed: " + mbedtls_err_str(rc);

    out.credential_id = credential->credential_id;
    out.client_data_json.assign(client.json.begin(), client.json.end());
    out.signature.assign(signature_buf.begin(), signature_buf.begin() + signature_len);
    out.user_handle = credential->user.id;
    return {};
  }

  std::vector<virtual_credential> virtual_authenticator::list_credentials() const {
    std::vector<virtual_credential> out;
    out.reserve(impl_->credentials.size());
    for (const auto& [id, credential] : impl_->credentials) {
      (void)id;
      out.push_back(credential);
    }
    return out;
  }

  void virtual_authenticator::clear() {
    impl_->credentials.clear();
  }

  bool virtual_authenticator::set_user_verified(bool verified) {
    impl_->user_verified = verified;
    return impl_->user_verified;
  }

  bool virtual_authenticator::verify_es256(const cose_ec2_key& pub,
                                           std::span<const uint8_t> message,
                                           std::span<const uint8_t> signature_der) {
    ecdsa_guard verifier;
    if (!load_public_key(verifier.ctx, pub))
      return false;
    const auto digest = sha256(message);
    return mbedtls_ecdsa_read_signature(&verifier.ctx, digest.data(), digest.size(),
                                        signature_der.data(), signature_der.size()) == 0;
  }

} // namespace fxe::webauthn
