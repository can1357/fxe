// Platform WebAuthn backend on Linux via libfido2 over USB HID.
//
// Linux has no first-class platform passkey provider in 2026, so this driver
// targets cross-platform FIDO2 / U2F security keys. We enumerate USB HID
// devices through libfido2, open the first device that owns one of the
// allow-credential ids (assertion) or the first available device
// (registration), then issue CTAP2 commands. PIN is not solicited — if the
// device requires UV and the JS layer hasn't supplied one, the call fails
// with a descriptive error so the binding can prompt and retry.
//
// libfido2 calls block; we run them on the calling thread. cancel() is safe
// from any thread and uses fido_dev_cancel (libfido2 ≥ 1.4) to interrupt the
// in-flight request — see libfido2 fido_dev_cancel(3).
//
// References: libfido2 manuals fido_cred(3), fido_assert(3), fido_dev(3),
// and Chromium device/fido/hid/.

#include <fxe/webauthn.hpp>

#if defined(FXE_HAS_LIBFIDO2)

#include <fido.h>
#include <fido/credman.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fxe::webauthn {
  namespace {

    struct fido_init_once {
      fido_init_once() { fido_init(0); }
    };
    void ensure_initialized() {
      static fido_init_once once;
      (void)once;
    }

    int cose_alg_value(cose_algorithm a) {
      switch (a) {
      case cose_algorithm::es256:
        return COSE_ES256;
      case cose_algorithm::eddsa:
        return COSE_EDDSA;
      case cose_algorithm::rs256:
        return COSE_RS256;
      }
      return COSE_ES256;
    }

    // RAII wrappers.
    struct cred_holder {
      fido_cred_t* p = fido_cred_new();
      ~cred_holder() { fido_cred_free(&p); }
    };
    struct assert_holder {
      fido_assert_t* p = fido_assert_new();
      ~assert_holder() { fido_assert_free(&p); }
    };
    struct dev_holder {
      fido_dev_t* p = fido_dev_new();
      ~dev_holder() {
        if (p) {
          if (fido_dev_is_fido2(p) || fido_dev_is_winhello(p))
            fido_dev_close(p);
          fido_dev_free(&p);
        }
      }
    };

    std::string fido_err(const char* tag, int rv) {
      char buf[160];
      const char* msg = fido_strerr(rv);
      snprintf(buf, sizeof(buf), "platform.linux: %s: %s (rv=%d)", tag, msg ? msg : "?", rv);
      return std::string(buf);
    }

    // Open the first connected fido2 device. When `match_credentials` is
    // non-empty (assertion path), we still open the first device — libfido2
    // returns CTAP2_ERR_NO_CREDENTIALS when the credential is not present and
    // we propagate that to the caller.
    std::string open_first_device(dev_holder& dh) {
      ensure_initialized();
      fido_dev_info_t* infos = fido_dev_info_new(64);
      if (!infos)
        return "platform.linux: fido_dev_info_new failed";
      size_t n = 0;
      int rv = fido_dev_info_manifest(infos, 64, &n);
      if (rv != FIDO_OK) {
        fido_dev_info_free(&infos, 64);
        return fido_err("fido_dev_info_manifest", rv);
      }
      if (n == 0u) {
        fido_dev_info_free(&infos, 64);
        return "platform.linux: no FIDO devices found";
      }
      std::string last_err;
      for (size_t i = 0; i < n; ++i) {
        const fido_dev_info_t* info = fido_dev_info_ptr(infos, i);
        if (!info)
          continue;
        const char* path = fido_dev_info_path(info);
        if (!path)
          continue;
        rv = fido_dev_open(dh.p, path);
        if (rv == FIDO_OK) {
          fido_dev_info_free(&infos, 64);
          return {};
        }
        last_err = fido_err("fido_dev_open", rv);
      }
      fido_dev_info_free(&infos, 64);
      return last_err.empty() ? "platform.linux: failed to open any FIDO device" : last_err;
    }

    fido_opt_t uv_opt(std::string_view in) {
      if (in == "required")
        return FIDO_OPT_TRUE;
      if (in == "discouraged")
        return FIDO_OPT_FALSE;
      return FIDO_OPT_OMIT;
    }

    fido_opt_t rk_opt(std::string_view in) {
      if (in == "required")
        return FIDO_OPT_TRUE;
      if (in == "discouraged")
        return FIDO_OPT_FALSE;
      return FIDO_OPT_OMIT;
    }

    class linux_authenticator final : public platform_authenticator {
    public:
      linux_authenticator() = default;
      ~linux_authenticator() override { cancel(); }

      std::string_view backend_name() const override { return "linux.libfido2"; }

      std::string register_credential(const creation_options& opts, std::string_view origin,
                                      register_response& out) override {
        if (std::string err = validate_creation_options(opts); !err.empty())
          return err;
        // libfido2 only drives cross-platform authenticators.
        if (opts.authenticator_attachment == "platform")
          return "platform.linux: platform authenticator not available on Linux";

        const auto cd = build_client_data(client_data_type::create, opts.challenge, origin, false);
        const auto cd_hash = sha256(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(cd.json.data()), cd.json.size()));

        cred_holder ch;
        if (!ch.p)
          return "platform.linux: fido_cred_new failed";

        cose_algorithm chosen_alg = opts.pub_key_params.empty() ? cose_algorithm::es256
                                                                : opts.pub_key_params.front();
        int rv = fido_cred_set_type(ch.p, cose_alg_value(chosen_alg));
        if (rv != FIDO_OK)
          return fido_err("fido_cred_set_type", rv);
        rv = fido_cred_set_clientdata_hash(ch.p, cd_hash.data(), cd_hash.size());
        if (rv != FIDO_OK)
          return fido_err("fido_cred_set_clientdata_hash", rv);
        rv = fido_cred_set_rp(ch.p, opts.rp_id.c_str(),
                              opts.rp_name.empty() ? nullptr : opts.rp_name.c_str());
        if (rv != FIDO_OK)
          return fido_err("fido_cred_set_rp", rv);
        rv = fido_cred_set_user(ch.p, opts.user.id.empty() ? nullptr : opts.user.id.data(),
                                opts.user.id.size(),
                                opts.user.name.empty() ? nullptr : opts.user.name.c_str(),
                                opts.user.display_name.empty() ? nullptr
                                                               : opts.user.display_name.c_str(),
                                /*icon=*/nullptr);
        if (rv != FIDO_OK)
          return fido_err("fido_cred_set_user", rv);
        if (uv_opt(opts.user_verification) != FIDO_OPT_OMIT) {
          rv = fido_cred_set_uv(ch.p, uv_opt(opts.user_verification));
          if (rv != FIDO_OK)
            return fido_err("fido_cred_set_uv", rv);
        }
        if (rk_opt(opts.resident_key) != FIDO_OPT_OMIT) {
          rv = fido_cred_set_rk(ch.p, rk_opt(opts.resident_key));
          if (rv != FIDO_OK)
            return fido_err("fido_cred_set_rk", rv);
        }
        for (const auto& id : opts.exclude_credentials) {
          if (id.empty())
            continue;
          rv = fido_cred_exclude(ch.p, id.data(), id.size());
          if (rv != FIDO_OK)
            return fido_err("fido_cred_exclude", rv);
        }

        dev_holder dh;
        if (!dh.p)
          return "platform.linux: fido_dev_new failed";
        if (std::string err = open_first_device(dh); !err.empty())
          return err;

        {
          std::lock_guard<std::mutex> lock(mu_);
          active_dev_ = dh.p;
        }
        rv = fido_dev_make_cred(dh.p, ch.p, /*pin=*/nullptr);
        {
          std::lock_guard<std::mutex> lock(mu_);
          active_dev_ = nullptr;
        }
        if (rv != FIDO_OK)
          return fido_err("fido_dev_make_cred", rv);

        // Assemble the attestation object from the libfido2 outputs:
        //   authData = fido_cred_authdata_ptr (CTAP2 spec authData blob)
        //   x5c      = fido_cred_x5c_ptr (DER cert chain, single cert)
        //   sig      = fido_cred_sig_ptr (over authData||clientDataHash)
        //   fmt      = fido_cred_fmt (e.g. "packed", "fido-u2f", "none")
        const unsigned char* auth_data = fido_cred_authdata_ptr(ch.p);
        const size_t auth_data_len = fido_cred_authdata_len(ch.p);
        const char* fmt = fido_cred_fmt(ch.p);
        if (!auth_data || auth_data_len == 0u)
          return "platform.linux: empty authData";

        // Fetch the COSE-encoded public key blob the spec embeds in authData.
        if (auto parsed = parse_authenticator_data(
                std::span<const uint8_t>(auth_data, auth_data_len));
            parsed && parsed->attested) {
          out.public_key = parsed->attested->cose_public_key;
          out.credential_id = parsed->attested->credential_id;
        }
        out.algorithm = chosen_alg;

        attestation_object att;
        att.fmt = (fmt && std::strcmp(fmt, "packed") == 0) ? attestation_format::packed
                                                           : attestation_format::none;
        att.auth_data.assign(auth_data, auth_data + auth_data_len);
        // For "packed", reconstruct attStmt: { alg, sig, x5c }.
        if (att.fmt == attestation_format::packed) {
          // We delegate to the core encoder which only supports "none" or
          // "packed" + an opaque att_stmt CBOR blob. Build the packed stmt
          // manually using runtime cbor.
          const unsigned char* sig = fido_cred_sig_ptr(ch.p);
          const size_t sig_len = fido_cred_sig_len(ch.p);
          const unsigned char* x5c = fido_cred_x5c_ptr(ch.p);
          const size_t x5c_len = fido_cred_x5c_len(ch.p);
          if (sig && sig_len > 0u) {
            // Build the packed attStmt as a CBOR map. Use core attestation
            // encoder for everything else by passing the encoded att_stmt
            // through the att_stmt_cbor field.
            namespace cbor = fxe::runtime::cbor;
            cbor::cmap stmt;
            stmt.push_back({std::string("alg"),
                            cbor::value(static_cast<int64_t>(cose_alg_value(chosen_alg)))});
            stmt.push_back({std::string("sig"),
                            cbor::value(std::vector<uint8_t>(sig, sig + sig_len))});
            if (x5c && x5c_len > 0u) {
              cbor::array x5c_arr;
              x5c_arr.push_back(cbor::value(std::vector<uint8_t>(x5c, x5c + x5c_len)));
              stmt.push_back({std::string("x5c"), cbor::value(std::move(x5c_arr))});
            }
            att.att_stmt_cbor = cbor::encode(cbor::value(std::move(stmt)));
          } else {
            att.fmt = attestation_format::none;
          }
        }
        out.attestation_object = encode_attestation_object(att);
        if (out.attestation_object.empty())
          return "platform.linux: failed to encode attestation object";
        out.client_data_json.assign(cd.json.begin(), cd.json.end());
        return {};
      }

      std::string assert_credential(const request_options& opts, std::string_view origin,
                                    assert_response& out) override {
        if (std::string err = validate_request_options(opts); !err.empty())
          return err;

        const auto cd = build_client_data(client_data_type::get, opts.challenge, origin, false);
        const auto cd_hash = sha256(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(cd.json.data()), cd.json.size()));

        assert_holder ah;
        if (!ah.p)
          return "platform.linux: fido_assert_new failed";

        int rv = fido_assert_set_clientdata_hash(ah.p, cd_hash.data(), cd_hash.size());
        if (rv != FIDO_OK)
          return fido_err("fido_assert_set_clientdata_hash", rv);
        rv = fido_assert_set_rp(ah.p, opts.rp_id.c_str());
        if (rv != FIDO_OK)
          return fido_err("fido_assert_set_rp", rv);
        for (const auto& id : opts.allow_credentials) {
          if (id.empty())
            continue;
          rv = fido_assert_allow_cred(ah.p, id.data(), id.size());
          if (rv != FIDO_OK)
            return fido_err("fido_assert_allow_cred", rv);
        }
        if (uv_opt(opts.user_verification) != FIDO_OPT_OMIT) {
          rv = fido_assert_set_uv(ah.p, uv_opt(opts.user_verification));
          if (rv != FIDO_OK)
            return fido_err("fido_assert_set_uv", rv);
        }

        dev_holder dh;
        if (!dh.p)
          return "platform.linux: fido_dev_new failed";
        if (std::string err = open_first_device(dh); !err.empty())
          return err;

        {
          std::lock_guard<std::mutex> lock(mu_);
          active_dev_ = dh.p;
        }
        rv = fido_dev_get_assert(dh.p, ah.p, /*pin=*/nullptr);
        {
          std::lock_guard<std::mutex> lock(mu_);
          active_dev_ = nullptr;
        }
        if (rv != FIDO_OK)
          return fido_err("fido_dev_get_assert", rv);
        if (fido_assert_count(ah.p) == 0u)
          return "platform.linux: no assertion produced";

        const size_t idx = 0;
        const unsigned char* auth_data = fido_assert_authdata_ptr(ah.p, idx);
        const size_t auth_data_len = fido_assert_authdata_len(ah.p, idx);
        const unsigned char* sig = fido_assert_sig_ptr(ah.p, idx);
        const size_t sig_len = fido_assert_sig_len(ah.p, idx);
        const unsigned char* user = fido_assert_user_id_ptr(ah.p, idx);
        const size_t user_len = fido_assert_user_id_len(ah.p, idx);
        const unsigned char* cred = fido_assert_id_ptr(ah.p, idx);
        const size_t cred_len = fido_assert_id_len(ah.p, idx);
        if (!auth_data || !sig || auth_data_len == 0u || sig_len == 0u)
          return "platform.linux: assertion missing fields";

        out.authenticator_data.assign(auth_data, auth_data + auth_data_len);
        out.signature.assign(sig, sig + sig_len);
        if (user && user_len > 0u)
          out.user_handle.assign(user, user + user_len);
        if (cred && cred_len > 0u)
          out.credential_id.assign(cred, cred + cred_len);
        out.client_data_json.assign(cd.json.begin(), cd.json.end());
        return {};
      }

      void cancel() override {
        fido_dev_t* dev = nullptr;
        {
          std::lock_guard<std::mutex> lock(mu_);
          dev = active_dev_;
        }
        if (dev) {
          // libfido2's cancel is safe to call from another thread.
          fido_dev_cancel(dev);
        }
      }

    private:
      std::mutex mu_;
      fido_dev_t* active_dev_ = nullptr;
    };

  } // namespace

  bool platform_authenticator::is_available() {
    ensure_initialized();
    return true;
  }

  bool platform_authenticator::is_user_verifying_platform_available() {
    // Linux has no platform passkey provider; security keys aren't UV
    // platform authenticators per the spec definition.
    return false;
  }

  std::unique_ptr<platform_authenticator> platform_authenticator::create() {
    return std::unique_ptr<platform_authenticator>(new linux_authenticator());
  }

} // namespace fxe::webauthn

#endif // FXE_HAS_LIBFIDO2
