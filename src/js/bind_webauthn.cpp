#include "bind_webauthn.hpp"

#include "../runtime/capabilities.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_strings.hpp>
#include <fxe/webauthn.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if !FXE_HAS_WEBAUTHN
namespace fxe::js {
  void install_webauthn_globals(v8::Isolate*, v8::Local<v8::ObjectTemplate>) {}
} // namespace fxe::js
#else
namespace fxe::js {
  namespace {
    using namespace v8;
    namespace webauthn = fxe::webauthn;

    using TplGlobal = Global<FunctionTemplate>;

    std::unordered_map<Isolate*, TplGlobal>& public_key_credential_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> table;
      return table;
    }

    void webauthn_reset_for_isolate(Isolate* iso) {
      auto& table = public_key_credential_tpl_table();
      auto it = table.find(iso);
      if (it != table.end()) {
        it->second.Reset();
        table.erase(it);
      }
    }

    struct webauthn_resetter_register {
      webauthn_resetter_register() {
        register_template_resetter(&webauthn_reset_for_isolate);
      }
    };
    static webauthn_resetter_register s_webauthn_resetter_register;

    void* public_key_credential_ctor_token() {
      static int token = 0;
      return &token;
    }

    webauthn::virtual_authenticator& authenticator() {
      static webauthn::virtual_authenticator instance;
      return instance;
    }

    Local<String> s8(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string to_str(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
    }

    Local<Value> make_named_error(Isolate* iso, std::string_view name, std::string_view message) {
      auto ctx = iso->GetCurrentContext();
      std::string rendered(name);
      if (!message.empty()) {
        rendered += ": ";
        rendered += message;
      }
      auto err = Exception::Error(s8(iso, rendered)).As<Object>();
      (void)err->Set(ctx, "name"_v8(iso), s8(iso, name));
      return err;
    }

    Local<Value> make_abort_error(Isolate* iso, std::string_view message) {
      const std::string rendered =
          message.empty() ? std::string("The operation was aborted") : std::string(message);
      return make_named_error(iso, "AbortError", rendered);
    }

    Local<ArrayBuffer> make_array_buffer(Isolate* iso, std::span<const uint8_t> bytes) {
      auto ab = ArrayBuffer::New(iso, bytes.size());
      if (!bytes.empty())
        std::memcpy(ab->GetBackingStore()->Data(), bytes.data(), bytes.size());
      return ab;
    }

    std::string base64url_encode(std::span<const uint8_t> bytes) {
      static constexpr char alphabet[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      std::string out;
      out.reserve(((bytes.size() + 2u) / 3u) * 4u);
      usize i = 0;
      while (i + 3u <= bytes.size()) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16u) |
                               (static_cast<uint32_t>(bytes[i + 1u]) << 8u) |
                               static_cast<uint32_t>(bytes[i + 2u]);
        out.push_back(alphabet[(chunk >> 18u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 12u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 6u) & 0x3Fu]);
        out.push_back(alphabet[chunk & 0x3Fu]);
        i += 3u;
      }
      const usize rem = bytes.size() - i;
      if (rem == 1u) {
        const uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16u;
        out.push_back(alphabet[(chunk >> 18u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 12u) & 0x3Fu]);
      } else if (rem == 2u) {
        const uint32_t chunk =
            (static_cast<uint32_t>(bytes[i]) << 16u) | (static_cast<uint32_t>(bytes[i + 1u]) << 8u);
        out.push_back(alphabet[(chunk >> 18u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 12u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 6u) & 0x3Fu]);
      }
      return out;
    }

    int attestation_rank(std::string_view value) {
      if (value == "direct")
        return 2;
      if (value == "indirect")
        return 1;
      return 0;
    }

    std::string_view attestation_name(int rank) {
      switch (rank) {
      case 2:
        return "direct";
      case 1:
        return "indirect";
      default:
        return "none";
      }
    }

    int user_verification_rank(std::string_view value) {
      if (value == "required")
        return 2;
      if (value == "preferred")
        return 1;
      return 0;
    }

    std::string_view user_verification_name(int rank) {
      switch (rank) {
      case 2:
        return "required";
      case 1:
        return "preferred";
      default:
        return "discouraged";
      }
    }

    bool get_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                  Local<Value>& out) {
      Local<Value> value;
      if (!obj->Get(ctx, key[0] == '\0' ? String::Empty(iso) : s8(iso, key)).ToLocal(&value) ||
          value->IsUndefined())
        return false;
      out = value;
      return true;
    }

    bool require_object(Isolate* iso, Local<Context> ctx, Local<Object> parent, const char* key,
                        Local<Object>& out, const char* label) {
      Local<Value> value;
      if (!get_prop(iso, ctx, parent, key, value) || !value->IsObject()) {
        iso->ThrowException(
            Exception::TypeError(s8(iso, std::string(label) + " must be an object")));
        return false;
      }
      out = value.As<Object>();
      return true;
    }

    bool require_string(Isolate* iso, Local<Context> ctx, Local<Object> parent, const char* key,
                        std::string& out, const char* label) {
      Local<Value> value;
      if (!get_prop(iso, ctx, parent, key, value) || !value->IsString()) {
        iso->ThrowException(
            Exception::TypeError(s8(iso, std::string(label) + " must be a string")));
        return false;
      }
      out = to_str(iso, value);
      return true;
    }

    bool read_buffer_source(Isolate* iso, Local<Context>, Local<Value> value,
                            std::vector<uint8_t>& out) {
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto store = view->Buffer()->GetBackingStore();
        const auto* data = static_cast<const uint8_t*>(store->Data()) + view->ByteOffset();
        out.assign(data, data + view->ByteLength());
        return true;
      }
      if (value->IsArrayBuffer()) {
        auto store = value.As<ArrayBuffer>()->GetBackingStore();
        const auto* data = static_cast<const uint8_t*>(store->Data());
        out.assign(data, data + store->ByteLength());
        return true;
      }
      iso->ThrowException(Exception::TypeError("expected BufferSource"_v8(iso)));
      return false;
    }

    bool read_string_vector(Isolate* iso, Local<Context> ctx, Local<Value> value,
                            std::vector<std::string>& out, const char* label) {
      if (value.IsEmpty() || value->IsUndefined())
        return true;
      if (!value->IsArray()) {
        iso->ThrowException(
            Exception::TypeError(s8(iso, std::string(label) + " must be an array")));
        return false;
      }
      auto arr = value.As<Array>();
      out.clear();
      out.reserve(arr->Length());
      for (u32 i = 0; i < arr->Length(); ++i) {
        Local<Value> item;
        if (!arr->Get(ctx, i).ToLocal(&item) || !item->IsString()) {
          iso->ThrowException(
              Exception::TypeError(s8(iso, std::string(label) + " entries must be strings")));
          return false;
        }
        out.push_back(to_str(iso, item));
      }
      return true;
    }

    Local<Value> private_get_or_null(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                     const char* key) {
      Local<Value> value;
      if (obj->Get(ctx, s8(iso, key)).ToLocal(&value) && !value->IsUndefined())
        return value;
      return Null(iso);
    }

    void attestation_get_authenticator_data(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      info.GetReturnValue().Set(private_get_or_null(iso, ctx, info.This(), "__fxeAuthData"));
    }

    void attestation_get_public_key(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      info.GetReturnValue().Set(private_get_or_null(iso, ctx, info.This(), "__fxePublicKey"));
    }

    void attestation_get_public_key_algorithm(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Integer::New(info.GetIsolate(), -7));
    }

    void attestation_get_transports(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto transports = Array::New(iso, 1);
      (void)transports->Set(ctx, 0, "internal"_v8(iso));
      info.GetReturnValue().Set(transports);
    }

    void public_key_credential_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.IsConstructCall() && info.Length() == 1 && info[0]->IsExternal() &&
          info[0].As<External>()->Value() == public_key_credential_ctor_token()) {
        info.This()->SetAlignedPointerInInternalField(0, nullptr);
        return;
      }
      iso->ThrowException(
          Exception::TypeError("PublicKeyCredential cannot be constructed directly"_v8(iso)));
    }

    void
    public_key_credential_get_client_extension_results(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Object::New(info.GetIsolate()));
    }

    Local<Object> wrap_public_key_credential(Isolate* iso, Local<Context> ctx, std::string_view id,
                                             std::span<const uint8_t> raw_id,
                                             Local<Object> response) {
      auto tpl = public_key_credential_tpl_table()[iso].Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      Local<Value> argv[] = {External::New(iso, public_key_credential_ctor_token())};
      auto obj = fn->NewInstance(ctx, 1, argv).ToLocalChecked();
      (void)obj->Set(ctx, "id"_v8(iso), s8(iso, id));
      (void)obj->Set(ctx, "rawId"_v8(iso), make_array_buffer(iso, raw_id));
      (void)obj->Set(ctx, "type"_v8(iso), "public-key"_v8(iso));
      (void)obj->Set(ctx, "authenticatorAttachment"_v8(iso), "cross-platform"_v8(iso));
      (void)obj->Set(ctx, "response"_v8(iso), response);
      return obj;
    }

    Local<Object> make_attestation_response(Isolate* iso, Local<Context> ctx,
                                            const webauthn::register_response& response) {
      auto obj = Object::New(iso);
      const auto attestation = webauthn::decode_attestation_object(response.attestation_object);
      std::vector<uint8_t> authenticator_data;
      if (attestation)
        authenticator_data = attestation->auth_data;
      (void)obj->Set(ctx, "clientDataJSON"_v8(iso),
                     make_array_buffer(iso, response.client_data_json));
      (void)obj->Set(ctx, "attestationObject"_v8(iso),
                     make_array_buffer(iso, response.attestation_object));
      (void)obj->Set(ctx, "__fxeAuthData"_v8(iso), make_array_buffer(iso, authenticator_data));
      (void)obj->Set(ctx, "__fxePublicKey"_v8(iso), make_array_buffer(iso, response.public_key));
      (void)obj->Set(ctx, "getAuthenticatorData"_v8(iso),
                     Function::New(ctx, attestation_get_authenticator_data).ToLocalChecked());
      (void)obj->Set(ctx, "getPublicKey"_v8(iso),
                     Function::New(ctx, attestation_get_public_key).ToLocalChecked());
      (void)obj->Set(ctx, "getPublicKeyAlgorithm"_v8(iso),
                     Function::New(ctx, attestation_get_public_key_algorithm).ToLocalChecked());
      (void)obj->Set(ctx, "getTransports"_v8(iso),
                     Function::New(ctx, attestation_get_transports).ToLocalChecked());
      return obj;
    }

    Local<Object> make_assertion_response(Isolate* iso, Local<Context> ctx,
                                          const webauthn::assert_response& response) {
      auto obj = Object::New(iso);
      (void)obj->Set(ctx, "clientDataJSON"_v8(iso),
                     make_array_buffer(iso, response.client_data_json));
      (void)obj->Set(ctx, "authenticatorData"_v8(iso),
                     make_array_buffer(iso, response.authenticator_data));
      (void)obj->Set(ctx, "signature"_v8(iso), make_array_buffer(iso, response.signature));
      if (response.user_handle.empty())
        (void)obj->Set(ctx, "userHandle"_v8(iso), Null(iso));
      else
        (void)obj->Set(ctx, "userHandle"_v8(iso), make_array_buffer(iso, response.user_handle));
      return obj;
    }

    bool parse_public_key_cred_params(Isolate* iso, Local<Context> ctx, Local<Object> public_key,
                                      webauthn::creation_options& out) {
      Local<Value> value;
      if (!get_prop(iso, ctx, public_key, "pubKeyCredParams", value) || !value->IsArray()) {
        iso->ThrowException(
            Exception::TypeError("publicKey.pubKeyCredParams must be an array"_v8(iso)));
        return false;
      }
      auto arr = value.As<Array>();
      out.pub_key_params.clear();
      for (u32 i = 0; i < arr->Length(); ++i) {
        Local<Value> item;
        if (!arr->Get(ctx, i).ToLocal(&item) || !item->IsObject()) {
          iso->ThrowException(
              Exception::TypeError("publicKey.pubKeyCredParams entries must be objects"_v8(iso)));
          return false;
        }
        auto obj = item.As<Object>();
        std::string type;
        if (!require_string(iso, ctx, obj, "type", type, "publicKey.pubKeyCredParams[].type"))
          return false;
        Local<Value> alg_value;
        if (!get_prop(iso, ctx, obj, "alg", alg_value) || !alg_value->IsNumber()) {
          iso->ThrowException(
              Exception::TypeError("publicKey.pubKeyCredParams[].alg must be a number"_v8(iso)));
          return false;
        }
        if (type == "public-key" && alg_value.As<Number>()->Value() == -7)
          out.pub_key_params.push_back(webauthn::cose_algorithm::es256);
      }
      return true;
    }

    bool parse_credential_descriptors(Isolate* iso, Local<Context> ctx, Local<Object> public_key,
                                      const char* property, std::vector<std::vector<uint8_t>>& out,
                                      const char* label) {
      Local<Value> value;
      if (!get_prop(iso, ctx, public_key, property, value))
        return true;
      if (!value->IsArray()) {
        iso->ThrowException(
            Exception::TypeError(s8(iso, std::string(label) + " must be an array")));
        return false;
      }
      auto arr = value.As<Array>();
      out.clear();
      out.reserve(arr->Length());
      for (u32 i = 0; i < arr->Length(); ++i) {
        Local<Value> item;
        if (!arr->Get(ctx, i).ToLocal(&item) || !item->IsObject()) {
          iso->ThrowException(
              Exception::TypeError(s8(iso, std::string(label) + " entries must be objects")));
          return false;
        }
        auto obj = item.As<Object>();
        std::string type;
        if (!require_string(iso, ctx, obj, "type", type, (std::string(label) + "[].type").c_str()))
          return false;
        if (type != "public-key") {
          iso->ThrowException(
              Exception::TypeError(s8(iso, std::string(label) + "[].type must be 'public-key'")));
          return false;
        }
        Local<Value> id_value;
        if (!get_prop(iso, ctx, obj, "id", id_value)) {
          iso->ThrowException(
              Exception::TypeError(s8(iso, std::string(label) + "[].id must be a BufferSource")));
          return false;
        }
        std::vector<uint8_t> id;
        if (!read_buffer_source(iso, ctx, id_value, id))
          return false;
        out.push_back(std::move(id));
      }
      return true;
    }

    bool parse_creation_options(Isolate* iso, Local<Context> ctx, Local<Object> options,
                                webauthn::creation_options& out, std::string& origin) {
      Local<Object> public_key;
      if (!require_object(iso, ctx, options, "publicKey", public_key, "publicKey"))
        return false;
      Local<Object> rp;
      if (!require_object(iso, ctx, public_key, "rp", rp, "publicKey.rp"))
        return false;
      if (!require_string(iso, ctx, rp, "id", out.rp_id, "publicKey.rp.id"))
        return false;
      if (!require_string(iso, ctx, rp, "name", out.rp_name, "publicKey.rp.name"))
        return false;

      Local<Object> user;
      if (!require_object(iso, ctx, public_key, "user", user, "publicKey.user"))
        return false;
      Local<Value> user_id;
      if (!get_prop(iso, ctx, user, "id", user_id) ||
          !read_buffer_source(iso, ctx, user_id, out.user.id))
        return false;
      if (!require_string(iso, ctx, user, "name", out.user.name, "publicKey.user.name"))
        return false;
      if (!require_string(iso, ctx, user, "displayName", out.user.display_name,
                          "publicKey.user.displayName"))
        return false;

      Local<Value> challenge;
      if (!get_prop(iso, ctx, public_key, "challenge", challenge) ||
          !read_buffer_source(iso, ctx, challenge, out.challenge))
        return false;

      if (!parse_public_key_cred_params(iso, ctx, public_key, out))
        return false;
      if (!parse_credential_descriptors(iso, ctx, public_key, "excludeCredentials",
                                        out.exclude_credentials, "publicKey.excludeCredentials"))
        return false;

      Local<Value> selection_value;
      if (get_prop(iso, ctx, public_key, "authenticatorSelection", selection_value)) {
        if (!selection_value->IsObject()) {
          iso->ThrowException(
              Exception::TypeError("publicKey.authenticatorSelection must be an object"_v8(iso)));
          return false;
        }
        auto selection = selection_value.As<Object>();
        Local<Value> value;
        if (get_prop(iso, ctx, selection, "authenticatorAttachment", value)) {
          if (!value->IsString()) {
            iso->ThrowException(Exception::TypeError(
                "publicKey.authenticatorSelection.authenticatorAttachment must be a string"_v8(
                    iso)));
            return false;
          }
          out.authenticator_attachment = to_str(iso, value);
        }
        if (get_prop(iso, ctx, selection, "userVerification", value)) {
          if (!value->IsString()) {
            iso->ThrowException(Exception::TypeError(
                "publicKey.authenticatorSelection.userVerification must be a string"_v8(iso)));
            return false;
          }
          out.user_verification = to_str(iso, value);
        }
        if (get_prop(iso, ctx, selection, "residentKey", value)) {
          if (!value->IsString()) {
            iso->ThrowException(Exception::TypeError(
                "publicKey.authenticatorSelection.residentKey must be a string"_v8(iso)));
            return false;
          }
          out.resident_key = to_str(iso, value);
        }
      }

      Local<Value> value;
      if (get_prop(iso, ctx, public_key, "attestation", value)) {
        if (!value->IsString()) {
          iso->ThrowException(
              Exception::TypeError("publicKey.attestation must be a string"_v8(iso)));
          return false;
        }
        out.attestation = to_str(iso, value);
      }
      if (get_prop(iso, ctx, public_key, "timeout", value)) {
        if (!value->IsNumber()) {
          iso->ThrowException(Exception::TypeError("publicKey.timeout must be a number"_v8(iso)));
          return false;
        }
        const double timeout = value.As<Number>()->Value();
        if (timeout < 0.0) {
          iso->ThrowException(
              Exception::TypeError("publicKey.timeout must be non-negative"_v8(iso)));
          return false;
        }
        out.timeout_ms = static_cast<uint64_t>(timeout);
      }

      origin = std::string("https://") + out.rp_id;
      return true;
    }

    bool parse_request_options(Isolate* iso, Local<Context> ctx, Local<Object> options,
                               webauthn::request_options& out, std::string& origin) {
      Local<Object> public_key;
      if (!require_object(iso, ctx, options, "publicKey", public_key, "publicKey"))
        return false;
      if (!require_string(iso, ctx, public_key, "rpId", out.rp_id, "publicKey.rpId"))
        return false;
      Local<Value> challenge;
      if (!get_prop(iso, ctx, public_key, "challenge", challenge) ||
          !read_buffer_source(iso, ctx, challenge, out.challenge))
        return false;
      if (!parse_credential_descriptors(iso, ctx, public_key, "allowCredentials",
                                        out.allow_credentials, "publicKey.allowCredentials"))
        return false;

      Local<Value> value;
      if (get_prop(iso, ctx, public_key, "userVerification", value)) {
        if (!value->IsString()) {
          iso->ThrowException(
              Exception::TypeError("publicKey.userVerification must be a string"_v8(iso)));
          return false;
        }
        out.user_verification = to_str(iso, value);
      }
      if (get_prop(iso, ctx, public_key, "timeout", value)) {
        if (!value->IsNumber()) {
          iso->ThrowException(Exception::TypeError("publicKey.timeout must be a number"_v8(iso)));
          return false;
        }
        const double timeout = value.As<Number>()->Value();
        if (timeout < 0.0) {
          iso->ThrowException(
              Exception::TypeError("publicKey.timeout must be non-negative"_v8(iso)));
          return false;
        }
        out.timeout_ms = static_cast<uint64_t>(timeout);
      }

      origin = std::string("https://") + out.rp_id;
      return true;
    }

    bool signal_aborted(Isolate* iso, Local<Context> ctx, Local<Object> options,
                        std::string& reason) {
      Local<Value> signal_value;
      if (!get_prop(iso, ctx, options, "signal", signal_value) || !signal_value->IsObject())
        return false;
      auto signal = signal_value.As<Object>();
      Local<Value> aborted_value;
      if (!signal->Get(ctx, "aborted"_v8(iso)).ToLocal(&aborted_value) ||
          !aborted_value->BooleanValue(iso))
        return false;
      Local<Value> reason_value;
      if (signal->Get(ctx, "reason"_v8(iso)).ToLocal(&reason_value) && !reason_value->IsUndefined())
        reason = to_str(iso, reason_value);
      return true;
    }

    void reject_caught(Isolate* iso, Local<Context> ctx, TryCatch& tc,
                       Local<Promise::Resolver> resolver, std::string_view fallback) {
      Local<Value> err = tc.HasCaught() ? tc.Exception() : Local<Value>();
      if (err.IsEmpty() || !err->IsNativeError())
        err = Exception::TypeError(s8(iso, fallback));
      resolver->Reject(ctx, err).Check();
    }

    void credentials_create(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());

      if (info.Length() < 1 || !info[0]->IsObject()) {
        resolver->Reject(ctx, Exception::TypeError("navigator.credentials.create(options)"_v8(iso)))
            .Check();
        return;
      }

      TryCatch tc(iso);
      auto options = info[0].As<Object>();
      Local<Value> public_key_value;
      if (!get_prop(iso, ctx, options, "publicKey", public_key_value) ||
          !public_key_value->IsObject()) {
        resolver->Reject(ctx, Exception::TypeError("publicKey is required"_v8(iso))).Check();
        return;
      }

      std::string abort_reason;
      if (signal_aborted(iso, ctx, options, abort_reason)) {
        resolver->Reject(ctx, make_abort_error(iso, abort_reason)).Check();
        return;
      }

      webauthn::creation_options creation;
      std::string origin;
      if (!parse_creation_options(iso, ctx, options, creation, origin)) {
        reject_caught(iso, ctx, tc, resolver, "WebAuthn create options are invalid");
        return;
      }
      if (creation.pub_key_params.empty()) {
        resolver
            ->Reject(ctx, make_named_error(iso, "NotSupportedError",
                                           "No supported publicKey.pubKeyCredParams were provided"))
            .Check();
        return;
      }
      auto policy = fxe::runtime::webauthn_allowed(creation.rp_id);
      if (!policy) {
        resolver
            ->Reject(ctx, make_named_error(iso, "NotAllowedError",
                                           "WebAuthn is not allowed for this RP ID"))
            .Check();
        return;
      }
      if (!creation.attestation.empty() &&
          attestation_rank(creation.attestation) > attestation_rank(policy->attestation)) {
        creation.attestation = std::string(attestation_name(attestation_rank(policy->attestation)));
      }
      if (!creation.user_verification.empty() &&
          user_verification_rank(creation.user_verification) >
              user_verification_rank(policy->user_verification)) {
        creation.user_verification =
            std::string(user_verification_name(user_verification_rank(policy->user_verification)));
      }
      if (std::string error = webauthn::validate_creation_options(creation); !error.empty()) {
        resolver->Reject(ctx, Exception::TypeError(s8(iso, error))).Check();
        return;
      }

      webauthn::register_response response;
      if (std::string error = authenticator().register_credential(creation, origin, response);
          !error.empty()) {
        resolver->Reject(ctx, make_named_error(iso, "NotAllowedError", error)).Check();
        return;
      }

      auto response_obj = make_attestation_response(iso, ctx, response);
      auto credential = wrap_public_key_credential(
          iso, ctx, base64url_encode(response.credential_id), response.credential_id, response_obj);
      resolver->Resolve(ctx, credential).Check();
    }

    void credentials_get(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());

      if (info.Length() < 1 || !info[0]->IsObject()) {
        resolver->Reject(ctx, Exception::TypeError("navigator.credentials.get(options)"_v8(iso)))
            .Check();
        return;
      }

      TryCatch tc(iso);
      auto options = info[0].As<Object>();
      Local<Value> public_key_value;
      if (!get_prop(iso, ctx, options, "publicKey", public_key_value) ||
          !public_key_value->IsObject()) {
        resolver->Reject(ctx, Exception::TypeError("publicKey is required"_v8(iso))).Check();
        return;
      }

      std::string abort_reason;
      if (signal_aborted(iso, ctx, options, abort_reason)) {
        resolver->Reject(ctx, make_abort_error(iso, abort_reason)).Check();
        return;
      }

      webauthn::request_options request;
      std::string origin;
      if (!parse_request_options(iso, ctx, options, request, origin)) {
        reject_caught(iso, ctx, tc, resolver, "WebAuthn get options are invalid");
        return;
      }

      auto policy = fxe::runtime::webauthn_allowed(request.rp_id);
      if (!policy) {
        resolver
            ->Reject(ctx, make_named_error(iso, "NotAllowedError",
                                           "WebAuthn is not allowed for this RP ID"))
            .Check();
        return;
      }
      if (!request.user_verification.empty() &&
          user_verification_rank(request.user_verification) >
              user_verification_rank(policy->user_verification)) {
        request.user_verification =
            std::string(user_verification_name(user_verification_rank(policy->user_verification)));
      }
      if (std::string error = webauthn::validate_request_options(request); !error.empty()) {
        resolver->Reject(ctx, Exception::TypeError(s8(iso, error))).Check();
        return;
      }

      webauthn::assert_response response;
      if (std::string error = authenticator().assert_credential(request, origin, response);
          !error.empty()) {
        resolver->Reject(ctx, make_named_error(iso, "NotAllowedError", error)).Check();
        return;
      }

      auto response_obj = make_assertion_response(iso, ctx, response);
      auto credential = wrap_public_key_credential(
          iso, ctx, base64url_encode(response.credential_id), response.credential_id, response_obj);
      resolver->Resolve(ctx, credential).Check();
    }

    void static_false_promise(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      resolver->Resolve(ctx, False(iso)).Check();
      info.GetReturnValue().Set(resolver->GetPromise());
    }

  } // namespace

  void install_webauthn_globals(Isolate* iso, Local<ObjectTemplate> global) {
    auto navigator_tpl = ObjectTemplate::New(iso);
    auto credentials_tpl = ObjectTemplate::New(iso);
    credentials_tpl->Set(iso, "create"_v8(iso), FunctionTemplate::New(iso, credentials_create));
    credentials_tpl->Set(iso, "get"_v8(iso), FunctionTemplate::New(iso, credentials_get));
    navigator_tpl->Set(iso, "credentials"_v8(iso), credentials_tpl);
    global->Set(iso, "navigator"_v8(iso), navigator_tpl);

    auto tpl = FunctionTemplate::New(iso, public_key_credential_ctor);
    tpl->SetClassName("PublicKeyCredential"_v8(iso));
    tpl->Set(iso, "isUserVerifyingPlatformAuthenticatorAvailable"_v8(iso),
             FunctionTemplate::New(iso, static_false_promise));
    tpl->Set(iso, "isConditionalMediationAvailable"_v8(iso),
             FunctionTemplate::New(iso, static_false_promise));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    tpl->PrototypeTemplate()->Set(
        iso, "getClientExtensionResults"_v8(iso),
        FunctionTemplate::New(iso, public_key_credential_get_client_extension_results));
    public_key_credential_tpl_table()[iso].Reset(iso, tpl);
    global->Set(iso, "PublicKeyCredential"_v8(iso), tpl);
  }
} // namespace fxe::js
#endif
