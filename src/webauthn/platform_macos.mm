// Platform WebAuthn backend on macOS via AuthenticationServices.
//
// Uses ASAuthorizationController with two providers in parallel:
//   - ASAuthorizationPlatformPublicKeyCredentialProvider (Touch ID / Face ID
//     passkeys, requires associated-domain entitlement for production).
//   - ASAuthorizationSecurityKeyPublicKeyCredentialProvider (USB / NFC FIDO2
//     security keys; works without associated domains).
//
// Requires macOS 12 (Monterey) for the platform API and macOS 13 (Ventura)
// for the security-key API. Compile with -fobjc-arc.
//
// Threading: ASAuthorizationController must be invoked on the main thread.
// The delegate also fires on the main thread. register_credential /
// assert_credential are blocking and dispatch onto the main queue, then wait
// on a dispatch_semaphore — callers MUST therefore not be on the main thread
// or the call will deadlock.

#include <fxe/webauthn.hpp>

#import <AppKit/AppKit.h>
#import <AuthenticationServices/AuthenticationServices.h>
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::webauthn {
  namespace {

    // -------------- bytes / strings helpers --------------

    NSData* to_nsdata(std::span<const uint8_t> bytes) {
      if (bytes.empty())
        return [NSData data];
      return [NSData dataWithBytes:bytes.data() length:bytes.size()];
    }

    std::vector<uint8_t> from_nsdata(NSData* data) {
      if (!data || data.length == 0u)
        return {};
      const uint8_t* p = static_cast<const uint8_t*>(data.bytes);
      return std::vector<uint8_t>(p, p + data.length);
    }

    NSString* to_nsstring(std::string_view s) {
      return [[NSString alloc] initWithBytes:s.data()
                                      length:s.size()
                                    encoding:NSUTF8StringEncoding];
    }

    NSString* uv_preference(std::string_view in) API_AVAILABLE(macos(12.0)) {
      if (in == "required")
        return ASAuthorizationPublicKeyCredentialUserVerificationPreferenceRequired;
      if (in == "discouraged")
        return ASAuthorizationPublicKeyCredentialUserVerificationPreferenceDiscouraged;
      return ASAuthorizationPublicKeyCredentialUserVerificationPreferencePreferred;
    }

    NSString* attestation_preference(std::string_view in) API_AVAILABLE(macos(12.0)) {
      if (in == "direct")
        return ASAuthorizationPublicKeyCredentialAttestationKindDirect;
      if (in == "indirect")
        return ASAuthorizationPublicKeyCredentialAttestationKindIndirect;
      if (in == "enterprise")
        return ASAuthorizationPublicKeyCredentialAttestationKindEnterprise;
      return ASAuthorizationPublicKeyCredentialAttestationKindNone;
    }

    NSString* resident_key_preference(std::string_view in) API_AVAILABLE(macos(13.0)) {
      if (in == "required")
        return ASAuthorizationPublicKeyCredentialResidentKeyPreferenceRequired;
      if (in == "discouraged")
        return ASAuthorizationPublicKeyCredentialResidentKeyPreferenceDiscouraged;
      return ASAuthorizationPublicKeyCredentialResidentKeyPreferencePreferred;
    }

  } // namespace

  // -------------- delegate (NSObject) --------------

} // namespace fxe::webauthn

API_AVAILABLE(macos(12.0))
@interface FxeWebauthnDelegate
    : NSObject <ASAuthorizationControllerDelegate, ASAuthorizationControllerPresentationContextProviding>
@property(nonatomic, strong) ASAuthorizationController* controller;
@property(nonatomic, strong) id<ASAuthorizationCredential> credential;
@property(nonatomic, strong) NSError* error;
@property(nonatomic, assign) BOOL completed;
@property(nonatomic, strong) dispatch_semaphore_t done;
- (instancetype)init;
- (void)wait;
@end

@implementation FxeWebauthnDelegate
- (instancetype)init {
  if ((self = [super init])) {
    _done = dispatch_semaphore_create(0);
    _completed = NO;
  }
  return self;
}

- (void)wait {
  dispatch_semaphore_wait(_done, DISPATCH_TIME_FOREVER);
}

- (void)signalIfNeeded {
  if (!_completed) {
    _completed = YES;
    dispatch_semaphore_signal(_done);
  }
}

- (void)authorizationController:(ASAuthorizationController*)controller
    didCompleteWithAuthorization:(ASAuthorization*)authorization {
  (void)controller;
  self.credential = (id<ASAuthorizationCredential>)authorization.credential;
  [self signalIfNeeded];
}

- (void)authorizationController:(ASAuthorizationController*)controller
           didCompleteWithError:(NSError*)error {
  (void)controller;
  self.error = error;
  [self signalIfNeeded];
}

- (ASPresentationAnchor)presentationAnchorForAuthorizationController:
    (ASAuthorizationController*)controller {
  (void)controller;
  NSWindow* window = nil;
  if ([NSThread isMainThread]) {
    NSApplication* app = [NSApplication sharedApplication];
    window = app.keyWindow ?: app.mainWindow;
    if (!window && app.windows.count > 0u)
      window = app.windows.firstObject;
  }
  return window;
}
@end

namespace fxe::webauthn {
  namespace {

    NSArray<ASAuthorizationRequest*>* build_registration_requests(
        const creation_options& opts, std::string_view client_data_json,
        std::span<const uint8_t> challenge) API_AVAILABLE(macos(12.0)) {
      NSMutableArray<ASAuthorizationRequest*>* requests = [NSMutableArray array];
      const bool platform_allowed =
          opts.authenticator_attachment.empty() || opts.authenticator_attachment == "platform";
      const bool cross_allowed = opts.authenticator_attachment.empty() ||
                                 opts.authenticator_attachment == "cross-platform";

      NSData* challenge_data = to_nsdata(challenge);
      NSData* user_id = to_nsdata(opts.user.id);
      NSString* user_name = to_nsstring(opts.user.name);
      NSString* rp_id = to_nsstring(opts.rp_id);

      if (platform_allowed) {
        ASAuthorizationPlatformPublicKeyCredentialProvider* provider =
            [[ASAuthorizationPlatformPublicKeyCredentialProvider alloc]
                initWithRelyingPartyIdentifier:rp_id];
        ASAuthorizationPlatformPublicKeyCredentialRegistrationRequest* req =
            [provider createCredentialRegistrationRequestWithChallenge:challenge_data
                                                                  name:user_name
                                                                userID:user_id];
        req.userVerificationPreference = uv_preference(opts.user_verification);
        req.attestationPreference = attestation_preference(opts.attestation);
        if (!opts.user.display_name.empty())
          req.displayName = to_nsstring(opts.user.display_name);
        if (!opts.exclude_credentials.empty()) {
          NSMutableArray<ASAuthorizationPlatformPublicKeyCredentialDescriptor*>* excl =
              [NSMutableArray array];
          for (const auto& id : opts.exclude_credentials) {
            [excl addObject:[[ASAuthorizationPlatformPublicKeyCredentialDescriptor alloc]
                                initWithCredentialID:to_nsdata(id)]];
          }
          if (@available(macOS 14.0, *)) {
            req.excludedCredentials = excl;
          }
        }
        (void)client_data_json;
        [requests addObject:req];
      }

      if (cross_allowed) {
        if (@available(macOS 13.0, *)) {
          ASAuthorizationSecurityKeyPublicKeyCredentialProvider* provider =
              [[ASAuthorizationSecurityKeyPublicKeyCredentialProvider alloc]
                  initWithRelyingPartyIdentifier:rp_id];
          ASAuthorizationSecurityKeyPublicKeyCredentialRegistrationRequest* req =
              [provider createCredentialRegistrationRequestWithChallenge:challenge_data
                                                             displayName:to_nsstring(
                                                                             opts.user.display_name)
                                                                    name:user_name
                                                                  userID:user_id];
          NSMutableArray<ASAuthorizationPublicKeyCredentialParameters*>* params =
              [NSMutableArray array];
          for (cose_algorithm alg : opts.pub_key_params) {
            [params addObject:[[ASAuthorizationPublicKeyCredentialParameters alloc]
                                  initWithAlgorithm:static_cast<ASCOSEAlgorithmIdentifier>(alg)]];
          }
          req.credentialParameters = params;
          req.userVerificationPreference = uv_preference(opts.user_verification);
          req.attestationPreference = attestation_preference(opts.attestation);
          req.residentKeyPreference = resident_key_preference(opts.resident_key);
          if (!opts.exclude_credentials.empty()) {
            NSMutableArray<ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor*>* excl =
                [NSMutableArray array];
            NSArray<ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransport>* transports =
                @[
                  ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportUSB,
                  ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportNFC,
                  ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportBluetooth,
                ];
            for (const auto& id : opts.exclude_credentials) {
              [excl addObject:[[ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor alloc]
                                  initWithCredentialID:to_nsdata(id)
                                            transports:transports]];
            }
            req.excludedCredentials = excl;
          }
          [requests addObject:req];
        }
      }

      return requests;
    }

    NSArray<ASAuthorizationRequest*>* build_assertion_requests(
        const request_options& opts, std::span<const uint8_t> challenge)
        API_AVAILABLE(macos(12.0)) {
      NSMutableArray<ASAuthorizationRequest*>* requests = [NSMutableArray array];
      NSData* challenge_data = to_nsdata(challenge);
      NSString* rp_id = to_nsstring(opts.rp_id);
      NSString* uv = uv_preference(opts.user_verification);

      // Platform passkey assertion.
      {
        ASAuthorizationPlatformPublicKeyCredentialProvider* provider =
            [[ASAuthorizationPlatformPublicKeyCredentialProvider alloc]
                initWithRelyingPartyIdentifier:rp_id];
        ASAuthorizationPlatformPublicKeyCredentialAssertionRequest* req =
            [provider createCredentialAssertionRequestWithChallenge:challenge_data];
        req.userVerificationPreference = uv;
        if (!opts.allow_credentials.empty()) {
          NSMutableArray<ASAuthorizationPlatformPublicKeyCredentialDescriptor*>* allow =
              [NSMutableArray array];
          for (const auto& id : opts.allow_credentials) {
            [allow addObject:[[ASAuthorizationPlatformPublicKeyCredentialDescriptor alloc]
                                 initWithCredentialID:to_nsdata(id)]];
          }
          req.allowedCredentials = allow;
        }
        [requests addObject:req];
      }

      // Security-key assertion (macOS 13+).
      if (@available(macOS 13.0, *)) {
        ASAuthorizationSecurityKeyPublicKeyCredentialProvider* provider =
            [[ASAuthorizationSecurityKeyPublicKeyCredentialProvider alloc]
                initWithRelyingPartyIdentifier:rp_id];
        ASAuthorizationSecurityKeyPublicKeyCredentialAssertionRequest* req =
            [provider createCredentialAssertionRequestWithChallenge:challenge_data];
        req.userVerificationPreference = uv;
        if (!opts.allow_credentials.empty()) {
          NSMutableArray<ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor*>* allow =
              [NSMutableArray array];
          NSArray<ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransport>* transports =
              @[
                ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportUSB,
                ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportNFC,
                ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportBluetooth,
              ];
          for (const auto& id : opts.allow_credentials) {
            [allow addObject:[[ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor alloc]
                                 initWithCredentialID:to_nsdata(id)
                                           transports:transports]];
          }
          req.allowedCredentials = allow;
        }
        [requests addObject:req];
      }

      return requests;
    }

    std::string format_error(NSError* error) {
      if (!error)
        return "AuthenticationServices error: unknown";
      const NSInteger code = error.code;
      const char* tag = "Unknown";
      if ([error.domain isEqualToString:ASAuthorizationErrorDomain]) {
        switch (code) {
        case ASAuthorizationErrorCanceled:
          tag = "Canceled";
          break;
        case ASAuthorizationErrorFailed:
          tag = "Failed";
          break;
        case ASAuthorizationErrorInvalidResponse:
          tag = "InvalidResponse";
          break;
        case ASAuthorizationErrorNotHandled:
          tag = "NotHandled";
          break;
        case ASAuthorizationErrorNotInteractive:
          tag = "NotInteractive";
          break;
        case ASAuthorizationErrorUnknown:
        default:
          tag = "Unknown";
          break;
        }
      }
      const char* desc = error.localizedDescription.UTF8String ?: "";
      char buf[512];
      snprintf(buf, sizeof(buf), "AuthenticationServices/%s (code %ld): %s", tag,
               static_cast<long>(code), desc);
      return std::string(buf);
    }

    // -------------- backend implementation --------------

    class macos_authenticator final : public platform_authenticator {
    public:
      macos_authenticator() = default;
      ~macos_authenticator() override { cancel(); }

      std::string_view backend_name() const override {
        return "macos.AuthenticationServices";
      }

      std::string register_credential(const creation_options& opts, std::string_view origin,
                                      register_response& out) override {
        if (@available(macOS 12.0, *)) {
          if (std::string err = validate_creation_options(opts); !err.empty())
            return err;
          if ([NSThread isMainThread])
            return "platform.macos: register_credential must be called off the main thread";
          const auto cd = build_client_data(client_data_type::create, opts.challenge, origin, false);

          NSArray<ASAuthorizationRequest*>* requests =
              build_registration_requests(opts, cd.json, opts.challenge);
          if (requests.count == 0u)
            return "platform.macos: no compatible authenticator providers";

          FxeWebauthnDelegate* delegate = [[FxeWebauthnDelegate alloc] init];
          ASAuthorizationController* __block controller = nil;
          dispatch_async(dispatch_get_main_queue(), ^{
            controller = [[ASAuthorizationController alloc]
                initWithAuthorizationRequests:requests];
            controller.delegate = delegate;
            controller.presentationContextProvider = delegate;
            delegate.controller = controller;
            {
              std::lock_guard<std::mutex> lock(mu_);
              active_ = controller;
            }
            [controller performRequests];
          });
          [delegate wait];
          {
            std::lock_guard<std::mutex> lock(mu_);
            active_ = nil;
          }
          if (delegate.error)
            return format_error(delegate.error);
          if (!delegate.credential)
            return "platform.macos: empty credential";

          // Both Platform and SecurityKey registrations expose the same shape:
          //   rawAttestationObject, rawClientDataJSON, credentialID
          NSData* attestation = nil;
          NSData* client_data_json = nil;
          NSData* credential_id = nil;
          if ([delegate.credential
                  isKindOfClass:[ASAuthorizationPlatformPublicKeyCredentialRegistration class]]) {
            auto* reg = (ASAuthorizationPlatformPublicKeyCredentialRegistration*)
                delegate.credential;
            attestation = reg.rawAttestationObject;
            client_data_json = reg.rawClientDataJSON;
            credential_id = reg.credentialID;
          } else if (@available(macOS 13.0, *)) {
            if ([delegate.credential
                    isKindOfClass:
                        [ASAuthorizationSecurityKeyPublicKeyCredentialRegistration class]]) {
              auto* reg = (ASAuthorizationSecurityKeyPublicKeyCredentialRegistration*)
                  delegate.credential;
              attestation = reg.rawAttestationObject;
              client_data_json = reg.rawClientDataJSON;
              credential_id = reg.credentialID;
            }
          }
          if (!attestation || !credential_id)
            return "platform.macos: registration response missing fields";

          out.credential_id = from_nsdata(credential_id);
          out.attestation_object = from_nsdata(attestation);
          // AS framework builds its own clientDataJSON; prefer it but fall
          // back to ours when the framework returns nil (older betas).
          out.client_data_json =
              client_data_json ? from_nsdata(client_data_json)
                               : std::vector<uint8_t>(cd.json.begin(), cd.json.end());

          // Extract the public-key COSE blob from authData so callers can
          // verify subsequent assertions without re-parsing the attestation
          // each time.
          if (auto att = decode_attestation_object(out.attestation_object)) {
            if (auto ad = parse_authenticator_data(att->auth_data); ad && ad->attested) {
              out.public_key = ad->attested->cose_public_key;
            }
          }
          out.algorithm = cose_algorithm::es256;
          return {};
        }
        return "platform.macos: AuthenticationServices requires macOS 12 or later";
      }

      std::string assert_credential(const request_options& opts, std::string_view origin,
                                    assert_response& out) override {
        if (@available(macOS 12.0, *)) {
          if (std::string err = validate_request_options(opts); !err.empty())
            return err;
          if ([NSThread isMainThread])
            return "platform.macos: assert_credential must be called off the main thread";
          const auto cd = build_client_data(client_data_type::get, opts.challenge, origin, false);

          NSArray<ASAuthorizationRequest*>* requests = build_assertion_requests(opts, opts.challenge);
          if (requests.count == 0u)
            return "platform.macos: no compatible authenticator providers";

          FxeWebauthnDelegate* delegate = [[FxeWebauthnDelegate alloc] init];
          ASAuthorizationController* __block controller = nil;
          dispatch_async(dispatch_get_main_queue(), ^{
            controller = [[ASAuthorizationController alloc]
                initWithAuthorizationRequests:requests];
            controller.delegate = delegate;
            controller.presentationContextProvider = delegate;
            delegate.controller = controller;
            {
              std::lock_guard<std::mutex> lock(mu_);
              active_ = controller;
            }
            [controller performRequests];
          });
          [delegate wait];
          {
            std::lock_guard<std::mutex> lock(mu_);
            active_ = nil;
          }
          if (delegate.error)
            return format_error(delegate.error);
          if (!delegate.credential)
            return "platform.macos: empty credential";

          NSData* auth_data = nil;
          NSData* signature = nil;
          NSData* user_handle = nil;
          NSData* credential_id = nil;
          NSData* client_data_json = nil;
          if ([delegate.credential
                  isKindOfClass:[ASAuthorizationPlatformPublicKeyCredentialAssertion class]]) {
            auto* a = (ASAuthorizationPlatformPublicKeyCredentialAssertion*)delegate.credential;
            auth_data = a.rawAuthenticatorData;
            signature = a.signature;
            user_handle = a.userID;
            credential_id = a.credentialID;
            client_data_json = a.rawClientDataJSON;
          } else if (@available(macOS 13.0, *)) {
            if ([delegate.credential
                    isKindOfClass:
                        [ASAuthorizationSecurityKeyPublicKeyCredentialAssertion class]]) {
              auto* a = (ASAuthorizationSecurityKeyPublicKeyCredentialAssertion*)delegate.credential;
              auth_data = a.rawAuthenticatorData;
              signature = a.signature;
              user_handle = a.userID;
              credential_id = a.credentialID;
              client_data_json = a.rawClientDataJSON;
            }
          }
          if (!auth_data || !signature || !credential_id)
            return "platform.macos: assertion response missing fields";

          out.credential_id = from_nsdata(credential_id);
          out.authenticator_data = from_nsdata(auth_data);
          out.signature = from_nsdata(signature);
          out.user_handle = from_nsdata(user_handle);
          out.client_data_json = client_data_json
                                     ? from_nsdata(client_data_json)
                                     : std::vector<uint8_t>(cd.json.begin(), cd.json.end());
          return {};
        }
        return "platform.macos: AuthenticationServices requires macOS 12 or later";
      }

      void cancel() override {
        if (@available(macOS 12.0, *)) {
          ASAuthorizationController* c = nil;
          {
            std::lock_guard<std::mutex> lock(mu_);
            c = active_;
          }
          if (c) {
            // [cancel] must run on the main thread.
            dispatch_async(dispatch_get_main_queue(), ^{
              [c cancel];
            });
          }
        }
      }

    private:
      std::mutex mu_;
      ASAuthorizationController* __strong active_ API_AVAILABLE(macos(12.0)) = nil;
    };

  } // namespace

  bool platform_authenticator::is_available() {
    if (@available(macOS 12.0, *))
      return true;
    return false;
  }

  bool platform_authenticator::is_user_verifying_platform_available() {
    if (@available(macOS 10.13.2, *)) {
      LAContext* ctx = [[LAContext alloc] init];
      NSError* err = nil;
      const BOOL ok = [ctx canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
                                       error:&err];
      return ok ? true : false;
    }
    return false;
  }

  std::unique_ptr<platform_authenticator> platform_authenticator::create() {
    if (!is_available())
      return nullptr;
    return std::unique_ptr<platform_authenticator>(new macos_authenticator());
  }

} // namespace fxe::webauthn
