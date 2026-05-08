// Platform WebAuthn backend on Windows via webauthn.dll.
//
// webauthn.dll ships with Windows 10 1903 and later. We runtime-bind via
// LoadLibraryW so the binary still links on older OSes (the create() factory
// returns nullptr when WebAuthNGetApiVersionNumber is unavailable). All work
// happens on the calling thread; the dll itself blocks until the user
// completes the gesture (Windows Hello PIN, biometric, security-key tap,
// or smart-card). cancel() works from any thread by calling
// WebAuthNCancelCurrentOperation with a per-request cancellation GUID.
//
// Reference: Microsoft `webauthn.h` and Chromium `device/fido/win/`.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fxe/webauthn.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::webauthn {
  namespace {

    // -------------- minimal webauthn.h definitions --------------
    // Mirrors the public Microsoft webauthn.h. Defined here so we don't take
    // a hard SDK dependency at build time. Only struct versions and fields
    // we actually use are declared.

    using WEBAUTHN_HRESULT = HRESULT;

    struct WEBAUTHN_RP_ENTITY_INFORMATION {
      DWORD dwVersion;
      LPCWSTR pwszId;
      LPCWSTR pwszName;
      LPCWSTR pwszIcon;
    };
    constexpr DWORD WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION = 1;

    struct WEBAUTHN_USER_ENTITY_INFORMATION {
      DWORD dwVersion;
      DWORD cbId;
      PBYTE pbId;
      LPCWSTR pwszName;
      LPCWSTR pwszIcon;
      LPCWSTR pwszDisplayName;
    };
    constexpr DWORD WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION = 1;

    struct WEBAUTHN_COSE_CREDENTIAL_PARAMETER {
      DWORD dwVersion;
      LPCWSTR pwszCredentialType;
      LONG lAlg;
    };
    constexpr DWORD WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION = 1;

    struct WEBAUTHN_COSE_CREDENTIAL_PARAMETERS {
      DWORD cCredentialParameters;
      WEBAUTHN_COSE_CREDENTIAL_PARAMETER* pCredentialParameters;
    };

    struct WEBAUTHN_CLIENT_DATA {
      DWORD dwVersion;
      DWORD cbClientDataJSON;
      PBYTE pbClientDataJSON;
      LPCWSTR pwszHashAlgId;
    };
    constexpr DWORD WEBAUTHN_CLIENT_DATA_CURRENT_VERSION = 1;

    struct WEBAUTHN_CREDENTIAL {
      DWORD dwVersion;
      DWORD cbId;
      PBYTE pbId;
      LPCWSTR pwszCredentialType;
    };
    constexpr DWORD WEBAUTHN_CREDENTIAL_CURRENT_VERSION = 1;

    struct WEBAUTHN_CREDENTIAL_EX {
      DWORD dwVersion;
      DWORD cbId;
      PBYTE pbId;
      LPCWSTR pwszCredentialType;
      DWORD dwTransports;
    };
    constexpr DWORD WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION = 1;

    struct WEBAUTHN_CREDENTIAL_LIST {
      DWORD cCredentials;
      WEBAUTHN_CREDENTIAL_EX** ppCredentials;
    };

    struct WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS {
      DWORD dwVersion;
      DWORD dwTimeoutMilliseconds;
      WEBAUTHN_CREDENTIAL excludeCredentials_unused; // legacy v1 field
      DWORD dwExtensionsUnused;                      // legacy
      void* pExtensionsUnused;
      DWORD dwAuthenticatorAttachment;
      BOOL bRequireResidentKey;
      DWORD dwUserVerificationRequirement;
      DWORD dwAttestationConveyancePreference;
      DWORD dwFlags;
      GUID* pCancellationId;
      WEBAUTHN_CREDENTIAL_LIST* pExcludeCredentialList;
      DWORD dwEnterpriseAttestation;
      DWORD dwLargeBlobSupport;
      BOOL bPreferResidentKey;
      BOOL bBrowserInPrivateMode;
    };
    constexpr DWORD WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_4 = 4;

    struct WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS {
      DWORD dwVersion;
      DWORD dwTimeoutMilliseconds;
      WEBAUTHN_CREDENTIAL allowCredentials_unused;
      DWORD dwExtensionsUnused;
      void* pExtensionsUnused;
      DWORD dwAuthenticatorAttachment;
      DWORD dwUserVerificationRequirement;
      DWORD dwFlags;
      LPCWSTR pwszU2fAppId;
      BOOL* pbU2fAppIdUsed;
      GUID* pCancellationId;
      WEBAUTHN_CREDENTIAL_LIST* pAllowCredentialList;
      DWORD dwCredLargeBlobOperation;
      DWORD cbCredLargeBlob;
      PBYTE pbCredLargeBlob;
    };
    constexpr DWORD WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_5 = 5;

    struct WEBAUTHN_X5C {
      DWORD cbData;
      PBYTE pbData;
    };

    struct WEBAUTHN_CREDENTIAL_ATTESTATION {
      DWORD dwVersion;
      LPCWSTR pwszFormatType;
      DWORD cbAuthenticatorData;
      PBYTE pbAuthenticatorData;
      DWORD cbAttestation;
      PBYTE pbAttestation;
      DWORD dwAttestationDecodeType;
      void* pvAttestationDecode;
      DWORD cbAttestationObject;
      PBYTE pbAttestationObject;
      DWORD cbCredentialId;
      PBYTE pbCredentialId;
      // additional v3+ fields exist; we only access v1-v3 fields above.
      DWORD dwExtensions;
      void* pExtensions;
      DWORD dwUsedTransport;
      BOOL bEpAtt;
      BOOL bLargeBlobSupported;
      BOOL bResidentKey;
      BOOL bUnsignedExtensionOutputs;
      DWORD cbUnsignedExtensionOutputs;
      PBYTE pbUnsignedExtensionOutputs;
    };

    struct WEBAUTHN_ASSERTION {
      DWORD dwVersion;
      DWORD cbAuthenticatorData;
      PBYTE pbAuthenticatorData;
      DWORD cbSignature;
      PBYTE pbSignature;
      WEBAUTHN_CREDENTIAL Credential;
      DWORD cbUserId;
      PBYTE pbUserId;
      DWORD dwExtensions;
      void* pExtensions;
      DWORD cbCredLargeBlob;
      PBYTE pbCredLargeBlob;
      DWORD dwCredLargeBlobStatus;
      DWORD cbHmacSecret;
      PBYTE pbHmacSecret;
      DWORD dwUsedTransport;
      DWORD cbUnsignedExtensionOutputs;
      PBYTE pbUnsignedExtensionOutputs;
    };

    constexpr DWORD WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY = 0;
    constexpr DWORD WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM = 1;
    constexpr DWORD WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM = 2;

    constexpr DWORD WEBAUTHN_USER_VERIFICATION_REQUIREMENT_ANY = 0;
    constexpr DWORD WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED = 1;
    constexpr DWORD WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED = 2;
    constexpr DWORD WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED = 3;

    constexpr DWORD WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_ANY = 0;
    constexpr DWORD WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE = 1;
    constexpr DWORD WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT = 2;
    constexpr DWORD WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT = 3;

    constexpr DWORD WEBAUTHN_ENTERPRISE_ATTESTATION_NONE = 0;
    constexpr DWORD WEBAUTHN_ENTERPRISE_ATTESTATION_VENDOR_FACILITATED = 1;
    constexpr DWORD WEBAUTHN_ENTERPRISE_ATTESTATION_PLATFORM_MANAGED = 2;

    constexpr LONG WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256 = -7;
    constexpr LONG WEBAUTHN_COSE_ALGORITHM_EDDSA = -8;
    constexpr LONG WEBAUTHN_COSE_ALGORITHM_RSASSA_PKCS1_V1_5_WITH_SHA256 = -257;

    using PFN_WebAuthNGetApiVersionNumber = DWORD(WINAPI*)();
    using PFN_WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable =
        HRESULT(WINAPI*)(BOOL* pbIsUserVerifyingPlatformAuthenticatorAvailable);
    using PFN_WebAuthNAuthenticatorMakeCredential = HRESULT(WINAPI*)(
        HWND hWnd, const WEBAUTHN_RP_ENTITY_INFORMATION*, const WEBAUTHN_USER_ENTITY_INFORMATION*,
        const WEBAUTHN_COSE_CREDENTIAL_PARAMETERS*, const WEBAUTHN_CLIENT_DATA*,
        const WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS*,
        WEBAUTHN_CREDENTIAL_ATTESTATION** ppWebAuthNCredentialAttestation);
    using PFN_WebAuthNAuthenticatorGetAssertion =
        HRESULT(WINAPI*)(HWND hWnd, LPCWSTR pwszRpId, const WEBAUTHN_CLIENT_DATA*,
                         const WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS*,
                         WEBAUTHN_ASSERTION** ppWebAuthNAssertion);
    using PFN_WebAuthNFreeCredentialAttestation = void(WINAPI*)(WEBAUTHN_CREDENTIAL_ATTESTATION*);
    using PFN_WebAuthNFreeAssertion = void(WINAPI*)(WEBAUTHN_ASSERTION*);
    using PFN_WebAuthNGetCancellationId = HRESULT(WINAPI*)(GUID*);
    using PFN_WebAuthNCancelCurrentOperation = HRESULT(WINAPI*)(const GUID*);
    using PFN_WebAuthNGetErrorName = LPCWSTR(WINAPI*)(HRESULT);

    struct webauthn_dll {
      HMODULE module = nullptr;
      DWORD api_version = 0;
      PFN_WebAuthNGetApiVersionNumber get_api_version = nullptr;
      PFN_WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable is_uvpa_available = nullptr;
      PFN_WebAuthNAuthenticatorMakeCredential make_credential = nullptr;
      PFN_WebAuthNAuthenticatorGetAssertion get_assertion = nullptr;
      PFN_WebAuthNFreeCredentialAttestation free_attestation = nullptr;
      PFN_WebAuthNFreeAssertion free_assertion = nullptr;
      PFN_WebAuthNGetCancellationId get_cancel_id = nullptr;
      PFN_WebAuthNCancelCurrentOperation cancel_current = nullptr;
      PFN_WebAuthNGetErrorName get_error_name = nullptr;
      bool valid() const {
        return module && make_credential && get_assertion && free_attestation && free_assertion;
      }
    };

    const webauthn_dll& dll() {
      static webauthn_dll g = [] {
        webauthn_dll w;
        w.module = LoadLibraryW(L"webauthn.dll");
        if (!w.module)
          return w;
#define RESOLVE(field, name)                                                                       \
  w.field = reinterpret_cast<decltype(w.field)>(GetProcAddress(w.module, name))
        RESOLVE(get_api_version, "WebAuthNGetApiVersionNumber");
        RESOLVE(is_uvpa_available, "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable");
        RESOLVE(make_credential, "WebAuthNAuthenticatorMakeCredential");
        RESOLVE(get_assertion, "WebAuthNAuthenticatorGetAssertion");
        RESOLVE(free_attestation, "WebAuthNFreeCredentialAttestation");
        RESOLVE(free_assertion, "WebAuthNFreeAssertion");
        RESOLVE(get_cancel_id, "WebAuthNGetCancellationId");
        RESOLVE(cancel_current, "WebAuthNCancelCurrentOperation");
        RESOLVE(get_error_name, "WebAuthNGetErrorName");
#undef RESOLVE
        if (w.get_api_version)
          w.api_version = w.get_api_version();
        return w;
      }();
      return g;
    }

    // -------------- helpers --------------

    std::wstring widen(std::string_view utf8) {
      if (utf8.empty())
        return std::wstring();
      const int needed =
          MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
      if (needed <= 0)
        return std::wstring();
      std::wstring out(static_cast<size_t>(needed), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                          needed);
      return out;
    }

    std::string narrow(LPCWSTR ws) {
      if (!ws || !*ws)
        return std::string();
      const int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
      if (needed <= 0)
        return std::string();
      std::string out(static_cast<size_t>(needed - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr);
      return out;
    }

    DWORD attachment_value(std::string_view in) {
      if (in == "platform")
        return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
      if (in == "cross-platform")
        return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM;
      return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    }

    DWORD uv_value(std::string_view in) {
      if (in == "required")
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
      if (in == "discouraged")
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
      if (in == "preferred")
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
      return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_ANY;
    }

    DWORD attestation_value(std::string_view in) {
      if (in == "direct")
        return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT;
      if (in == "indirect")
        return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT;
      if (in == "enterprise")
        return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT; // mapped via flag
      return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;
    }

    LONG cose_alg_value(cose_algorithm a) {
      switch (a) {
      case cose_algorithm::es256:
        return WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256;
      case cose_algorithm::eddsa:
        return WEBAUTHN_COSE_ALGORITHM_EDDSA;
      case cose_algorithm::rs256:
        return WEBAUTHN_COSE_ALGORITHM_RSASSA_PKCS1_V1_5_WITH_SHA256;
      }
      return WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256;
    }

    HWND foreground_or_desktop() {
      HWND h = GetForegroundWindow();
      if (h)
        return h;
      return GetDesktopWindow();
    }

    std::string format_hresult(const webauthn_dll& w, HRESULT hr) {
      char buf[256];
      const char* name = "?";
      std::string named;
      if (w.get_error_name) {
        if (LPCWSTR ws = w.get_error_name(hr)) {
          named = narrow(ws);
          if (!named.empty())
            name = named.c_str();
        }
      }
      snprintf(buf, sizeof(buf), "webauthn.dll/%s (hr=0x%08lx)", name,
               static_cast<unsigned long>(hr));
      return std::string(buf);
    }

    // RAII cleanup for the heap-allocated structs returned by webauthn.dll.
    struct attestation_release {
      const webauthn_dll* w;
      WEBAUTHN_CREDENTIAL_ATTESTATION* p;
      ~attestation_release() {
        if (p && w && w->free_attestation)
          w->free_attestation(p);
      }
    };
    struct assertion_release {
      const webauthn_dll* w;
      WEBAUTHN_ASSERTION* p;
      ~assertion_release() {
        if (p && w && w->free_assertion)
          w->free_assertion(p);
      }
    };

    // -------------- backend implementation --------------

    class win32_authenticator final : public platform_authenticator {
    public:
      win32_authenticator() {
        std::memset(&cancel_id_, 0, sizeof(cancel_id_));
      }
      ~win32_authenticator() override {
        cancel();
      }

      std::string_view backend_name() const override {
        return "win32.webauthn.dll";
      }

      std::string register_credential(const creation_options& opts, std::string_view origin,
                                      register_response& out) override {
        const webauthn_dll& w = dll();
        if (!w.valid())
          return "platform.win32: webauthn.dll not available";
        if (std::string err = validate_creation_options(opts); !err.empty())
          return err;

        const auto cd = build_client_data(client_data_type::create, opts.challenge, origin, false);
        const std::wstring rp_id_w = widen(opts.rp_id);
        const std::wstring rp_name_w = widen(opts.rp_name);
        const std::wstring user_name_w = widen(opts.user.name);
        const std::wstring user_display_w = widen(opts.user.display_name);

        WEBAUTHN_RP_ENTITY_INFORMATION rp{};
        rp.dwVersion = WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION;
        rp.pwszId = rp_id_w.c_str();
        rp.pwszName = rp_name_w.c_str();

        std::vector<uint8_t> user_id_storage(opts.user.id);
        WEBAUTHN_USER_ENTITY_INFORMATION user{};
        user.dwVersion = WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION;
        user.cbId = static_cast<DWORD>(user_id_storage.size());
        user.pbId = user_id_storage.empty() ? nullptr : user_id_storage.data();
        user.pwszName = user_name_w.c_str();
        user.pwszDisplayName = user_display_w.c_str();

        std::vector<WEBAUTHN_COSE_CREDENTIAL_PARAMETER> params;
        params.reserve(opts.pub_key_params.size());
        for (cose_algorithm a : opts.pub_key_params) {
          WEBAUTHN_COSE_CREDENTIAL_PARAMETER p{};
          p.dwVersion = WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION;
          p.pwszCredentialType = L"public-key";
          p.lAlg = cose_alg_value(a);
          params.push_back(p);
        }
        WEBAUTHN_COSE_CREDENTIAL_PARAMETERS coses{};
        coses.cCredentialParameters = static_cast<DWORD>(params.size());
        coses.pCredentialParameters = params.data();

        std::vector<uint8_t> client_data_bytes(cd.json.begin(), cd.json.end());
        WEBAUTHN_CLIENT_DATA cdata{};
        cdata.dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
        cdata.cbClientDataJSON = static_cast<DWORD>(client_data_bytes.size());
        cdata.pbClientDataJSON = client_data_bytes.data();
        cdata.pwszHashAlgId = L"SHA-256";

        // Exclude credentials list (CredentialList Ex form).
        std::vector<WEBAUTHN_CREDENTIAL_EX> excl_storage;
        std::vector<std::vector<uint8_t>> excl_id_storage;
        std::vector<WEBAUTHN_CREDENTIAL_EX*> excl_ptrs;
        excl_storage.reserve(opts.exclude_credentials.size());
        excl_id_storage.reserve(opts.exclude_credentials.size());
        for (const auto& id : opts.exclude_credentials) {
          excl_id_storage.push_back(id);
          WEBAUTHN_CREDENTIAL_EX e{};
          e.dwVersion = WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION;
          e.cbId = static_cast<DWORD>(excl_id_storage.back().size());
          e.pbId = excl_id_storage.back().data();
          e.pwszCredentialType = L"public-key";
          e.dwTransports = 0; // any
          excl_storage.push_back(e);
        }
        for (auto& e : excl_storage)
          excl_ptrs.push_back(&e);
        WEBAUTHN_CREDENTIAL_LIST excl_list{};
        excl_list.cCredentials = static_cast<DWORD>(excl_ptrs.size());
        excl_list.ppCredentials = excl_ptrs.data();

        WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS mco{};
        mco.dwVersion = WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_4;
        mco.dwTimeoutMilliseconds =
            opts.timeout_ms > 0u ? static_cast<DWORD>(opts.timeout_ms) : 60000u;
        mco.dwAuthenticatorAttachment = attachment_value(opts.authenticator_attachment);
        mco.bRequireResidentKey = opts.resident_key == "required" ? TRUE : FALSE;
        mco.bPreferResidentKey = opts.resident_key == "preferred" ? TRUE : FALSE;
        mco.dwUserVerificationRequirement = uv_value(opts.user_verification);
        mco.dwAttestationConveyancePreference = attestation_value(opts.attestation);
        mco.dwEnterpriseAttestation = opts.attestation == "enterprise"
                                          ? WEBAUTHN_ENTERPRISE_ATTESTATION_VENDOR_FACILITATED
                                          : WEBAUTHN_ENTERPRISE_ATTESTATION_NONE;
        if (!excl_ptrs.empty())
          mco.pExcludeCredentialList = &excl_list;

        // Per-request cancellation GUID.
        GUID cancel_id;
        std::memset(&cancel_id, 0, sizeof(cancel_id));
        if (w.get_cancel_id) {
          if (FAILED(w.get_cancel_id(&cancel_id)))
            std::memset(&cancel_id, 0, sizeof(cancel_id));
        }
        mco.pCancellationId =
            (cancel_id.Data1 || cancel_id.Data2 || cancel_id.Data3) ? &cancel_id : nullptr;
        {
          std::lock_guard<std::mutex> lock(mu_);
          cancel_id_ = cancel_id;
          have_cancel_id_ = (mco.pCancellationId != nullptr);
        }

        WEBAUTHN_CREDENTIAL_ATTESTATION* result = nullptr;
        const HRESULT hr =
            w.make_credential(foreground_or_desktop(), &rp, &user, &coses, &cdata, &mco, &result);
        attestation_release guard{&w, result};
        {
          std::lock_guard<std::mutex> lock(mu_);
          have_cancel_id_ = false;
        }
        if (FAILED(hr))
          return format_hresult(w, hr);
        if (!result || !result->pbAttestationObject || !result->pbCredentialId)
          return "platform.win32: empty attestation result";

        out.credential_id.assign(result->pbCredentialId,
                                 result->pbCredentialId + result->cbCredentialId);
        out.attestation_object.assign(result->pbAttestationObject,
                                      result->pbAttestationObject + result->cbAttestationObject);
        out.client_data_json = std::move(client_data_bytes);
        if (auto att = decode_attestation_object(out.attestation_object)) {
          if (auto ad = parse_authenticator_data(att->auth_data); ad && ad->attested) {
            out.public_key = ad->attested->cose_public_key;
          }
        }
        out.algorithm = cose_algorithm::es256;
        return {};
      }

      std::string assert_credential(const request_options& opts, std::string_view origin,
                                    assert_response& out) override {
        const webauthn_dll& w = dll();
        if (!w.valid())
          return "platform.win32: webauthn.dll not available";
        if (std::string err = validate_request_options(opts); !err.empty())
          return err;

        const auto cd = build_client_data(client_data_type::get, opts.challenge, origin, false);
        const std::wstring rp_id_w = widen(opts.rp_id);

        std::vector<uint8_t> client_data_bytes(cd.json.begin(), cd.json.end());
        WEBAUTHN_CLIENT_DATA cdata{};
        cdata.dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
        cdata.cbClientDataJSON = static_cast<DWORD>(client_data_bytes.size());
        cdata.pbClientDataJSON = client_data_bytes.data();
        cdata.pwszHashAlgId = L"SHA-256";

        std::vector<WEBAUTHN_CREDENTIAL_EX> allow_storage;
        std::vector<std::vector<uint8_t>> allow_id_storage;
        std::vector<WEBAUTHN_CREDENTIAL_EX*> allow_ptrs;
        allow_storage.reserve(opts.allow_credentials.size());
        allow_id_storage.reserve(opts.allow_credentials.size());
        for (const auto& id : opts.allow_credentials) {
          allow_id_storage.push_back(id);
          WEBAUTHN_CREDENTIAL_EX e{};
          e.dwVersion = WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION;
          e.cbId = static_cast<DWORD>(allow_id_storage.back().size());
          e.pbId = allow_id_storage.back().data();
          e.pwszCredentialType = L"public-key";
          e.dwTransports = 0;
          allow_storage.push_back(e);
        }
        for (auto& e : allow_storage)
          allow_ptrs.push_back(&e);
        WEBAUTHN_CREDENTIAL_LIST allow_list{};
        allow_list.cCredentials = static_cast<DWORD>(allow_ptrs.size());
        allow_list.ppCredentials = allow_ptrs.data();

        WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS gao{};
        gao.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_5;
        gao.dwTimeoutMilliseconds =
            opts.timeout_ms > 0u ? static_cast<DWORD>(opts.timeout_ms) : 60000u;
        gao.dwUserVerificationRequirement = uv_value(opts.user_verification);
        if (!allow_ptrs.empty())
          gao.pAllowCredentialList = &allow_list;

        GUID cancel_id;
        std::memset(&cancel_id, 0, sizeof(cancel_id));
        if (w.get_cancel_id) {
          if (FAILED(w.get_cancel_id(&cancel_id)))
            std::memset(&cancel_id, 0, sizeof(cancel_id));
        }
        gao.pCancellationId =
            (cancel_id.Data1 || cancel_id.Data2 || cancel_id.Data3) ? &cancel_id : nullptr;
        {
          std::lock_guard<std::mutex> lock(mu_);
          cancel_id_ = cancel_id;
          have_cancel_id_ = (gao.pCancellationId != nullptr);
        }

        WEBAUTHN_ASSERTION* result = nullptr;
        const HRESULT hr =
            w.get_assertion(foreground_or_desktop(), rp_id_w.c_str(), &cdata, &gao, &result);
        assertion_release guard{&w, result};
        {
          std::lock_guard<std::mutex> lock(mu_);
          have_cancel_id_ = false;
        }
        if (FAILED(hr))
          return format_hresult(w, hr);
        if (!result || !result->pbAuthenticatorData || !result->pbSignature)
          return "platform.win32: empty assertion result";

        out.authenticator_data.assign(result->pbAuthenticatorData,
                                      result->pbAuthenticatorData + result->cbAuthenticatorData);
        out.signature.assign(result->pbSignature, result->pbSignature + result->cbSignature);
        if (result->Credential.pbId && result->Credential.cbId)
          out.credential_id.assign(result->Credential.pbId,
                                   result->Credential.pbId + result->Credential.cbId);
        if (result->pbUserId && result->cbUserId)
          out.user_handle.assign(result->pbUserId, result->pbUserId + result->cbUserId);
        out.client_data_json = std::move(client_data_bytes);
        return {};
      }

      void cancel() override {
        const webauthn_dll& w = dll();
        if (!w.cancel_current)
          return;
        GUID id;
        bool have = false;
        {
          std::lock_guard<std::mutex> lock(mu_);
          id = cancel_id_;
          have = have_cancel_id_;
        }
        if (have)
          w.cancel_current(&id);
      }

    private:
      std::mutex mu_;
      GUID cancel_id_{};
      bool have_cancel_id_ = false;
    };

  } // namespace

  bool platform_authenticator::is_available() {
    return dll().valid() && dll().api_version >= 1;
  }

  bool platform_authenticator::is_user_verifying_platform_available() {
    const webauthn_dll& w = dll();
    if (!w.valid() || !w.is_uvpa_available)
      return false;
    BOOL out = FALSE;
    if (FAILED(w.is_uvpa_available(&out)))
      return false;
    return out == TRUE;
  }

  std::unique_ptr<platform_authenticator> platform_authenticator::create() {
    if (!is_available())
      return nullptr;
    return std::unique_ptr<platform_authenticator>(new win32_authenticator());
  }

} // namespace fxe::webauthn
