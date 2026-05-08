#include "bind_webauthn.hpp"

#include "../runtime/capabilities.hpp"
#include "../runtime/uv_loop.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_host.hpp>
#include <fxe/v8_strings.hpp>
#include <fxe/webauthn.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

    struct selected_webauthn_backend {
      bool is_platform = false;
      std::string attachment = "cross-platform";
      std::vector<std::string> transports;
      std::shared_ptr<webauthn::platform_authenticator> platform;
    };

    struct webauthn_op_state;

    struct pending_abort_ctx {
      std::shared_ptr<webauthn_op_state> state;
    };

    struct webauthn_op_state {
      Isolate* iso = nullptr;
      Global<Context> ctx;
      Global<Promise::Resolver> resolver;
      Global<Object> signal_obj;
      Global<Function> abort_listener;
      pending_abort_ctx* abort_ctx = nullptr;
      std::shared_ptr<webauthn::platform_authenticator> authenticator;
      std::atomic_bool done{false};
      std::atomic_bool aborted{false};
      std::string abort_reason;
      std::string attachment = "platform";
      std::vector<std::string> transports;
      webauthn::register_response register_result;
      webauthn::assert_response assert_result;
      bool has_register_result = false;
      bool has_assert_result = false;
      std::string error;
    };

    std::vector<std::string> transports_for_backend(std::string_view backend_name) {
      (void)backend_name;
      return {"internal"};
    }

    std::optional<selected_webauthn_backend>
    select_webauthn_backend(const fxe::runtime::capability_set::webauthn_policy& policy) {
      if (policy.allow_virtual_authenticator) {
        selected_webauthn_backend backend;
        backend.attachment = "cross-platform";
        backend.transports = transports_for_backend("virtual");
        return backend;
      }
      if (!webauthn::platform_authenticator::is_available())
        return std::nullopt;
      auto platform = webauthn::platform_authenticator::create();
      if (!platform)
        return std::nullopt;
      selected_webauthn_backend backend;
      backend.is_platform = true;
      backend.attachment = "platform";
      backend.transports = transports_for_backend(platform->backend_name());
      backend.platform = std::shared_ptr<webauthn::platform_authenticator>(std::move(platform));
      return backend;
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
      auto transports = private_get_or_null(iso, ctx, info.This(), "__fxeTransports");
      if (!transports->IsArray()) {
        auto fallback = Array::New(iso, 1);
        (void)fallback->Set(ctx, 0, "internal"_v8(iso));
        info.GetReturnValue().Set(fallback);
        return;
      }
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
                                             std::string_view attachment, Local<Object> response) {
      auto tpl = public_key_credential_tpl_table()[iso].Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      Local<Value> argv[] = {External::New(iso, public_key_credential_ctor_token())};
      auto obj = fn->NewInstance(ctx, 1, argv).ToLocalChecked();
      (void)obj->Set(ctx, "id"_v8(iso), s8(iso, id));
      (void)obj->Set(ctx, "rawId"_v8(iso), make_array_buffer(iso, raw_id));
      (void)obj->Set(ctx, "type"_v8(iso), "public-key"_v8(iso));
      (void)obj->Set(ctx, "authenticatorAttachment"_v8(iso), s8(iso, attachment));
      (void)obj->Set(ctx, "response"_v8(iso), response);
      return obj;
    }

    Local<Object> make_attestation_response(Isolate* iso, Local<Context> ctx,
                                            const webauthn::register_response& response,
                                            std::span<const std::string> transports) {
      auto obj = Object::New(iso);
      const auto attestation = webauthn::decode_attestation_object(response.attestation_object);
      std::vector<uint8_t> authenticator_data;
      if (attestation)
        authenticator_data = attestation->auth_data;
      auto transport_values = Array::New(iso, static_cast<int>(transports.size()));
      for (u32 i = 0; i < transports.size(); ++i)
        (void)transport_values->Set(ctx, i, s8(iso, transports[i]));
      (void)obj->Set(ctx, "clientDataJSON"_v8(iso),
                     make_array_buffer(iso, response.client_data_json));
      (void)obj->Set(ctx, "attestationObject"_v8(iso),
                     make_array_buffer(iso, response.attestation_object));
      (void)obj->Set(ctx, "__fxeAuthData"_v8(iso), make_array_buffer(iso, authenticator_data));
      (void)obj->Set(ctx, "__fxePublicKey"_v8(iso), make_array_buffer(iso, response.public_key));
      (void)obj->Set(ctx, "__fxeTransports"_v8(iso), transport_values);
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

    bool get_signal_object(Isolate* iso, Local<Context> ctx, Local<Object> options,
                           Local<Object>& signal) {
      Local<Value> signal_value;
      if (!get_prop(iso, ctx, options, "signal", signal_value) || !signal_value->IsObject())
        return false;
      signal = signal_value.As<Object>();
      return true;
    }

    bool signal_aborted(Isolate* iso, Local<Context> ctx, Local<Object> options,
                        std::string& reason) {
      Local<Object> signal;
      if (!get_signal_object(iso, ctx, options, signal))
        return false;
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

    void clear_webauthn_abort_listener(Isolate* iso, webauthn_op_state& state) {
      auto ctx = state.ctx.IsEmpty() ? Local<Context>() : state.ctx.Get(iso);
      if (!state.abort_listener.IsEmpty() && !state.signal_obj.IsEmpty() && !ctx.IsEmpty()) {
        TryCatch tc(iso);
        auto signal = state.signal_obj.Get(iso);
        Local<Value> remove_value;
        if (signal->Get(ctx, "removeEventListener"_v8(iso)).ToLocal(&remove_value) &&
            remove_value->IsFunction()) {
          Local<Value> argv[2] = {"abort"_v8(iso), state.abort_listener.Get(iso)};
          Local<Value> ignored;
          (void)remove_value.As<Function>()->Call(ctx, signal, 2, argv).ToLocal(&ignored);
        }
        tc.Reset();
      }
      state.abort_listener.Reset();
      delete state.abort_ctx;
      state.abort_ctx = nullptr;
      state.signal_obj.Reset();
    }

    void reject_webauthn_state(Isolate* iso, webauthn_op_state& state, Local<Value> error) {
      auto ctx = state.ctx.Get(iso);
      auto resolver = state.resolver.Get(iso);
      clear_webauthn_abort_listener(iso, state);
      resolver->Reject(ctx, error).Check();
      state.resolver.Reset();
      state.ctx.Reset();
      state.authenticator.reset();
    }

    void resolve_webauthn_create_state(const std::shared_ptr<webauthn_op_state>& state) {
      auto* iso = state->iso;
      if (host_for_isolate(iso) == nullptr) {
        delete state->abort_ctx;
        state->abort_ctx = nullptr;
        return;
      }
      v8::Locker locker(iso);
      Isolate::Scope iscope(iso);
      HandleScope hs(iso);
      auto ctx = state->ctx.Get(iso);
      if (ctx.IsEmpty()) {
        delete state->abort_ctx;
        state->abort_ctx = nullptr;
        return;
      }
      Context::Scope cs(ctx);
      if (state->aborted.load(std::memory_order_acquire)) {
        reject_webauthn_state(iso, *state, make_abort_error(iso, state->abort_reason));
        return;
      }
      if (!state->error.empty() || !state->has_register_result) {
        const auto message = state->error.empty() ? std::string("WebAuthn registration failed")
                                                  : state->error;
        reject_webauthn_state(iso, *state, make_named_error(iso, "NotAllowedError", message));
        return;
      }
      auto resolver = state->resolver.Get(iso);
      auto response_obj =
          make_attestation_response(iso, ctx, state->register_result, state->transports);
      auto credential =
          wrap_public_key_credential(iso, ctx, base64url_encode(state->register_result.credential_id),
                                     state->register_result.credential_id, state->attachment,
                                     response_obj);
      clear_webauthn_abort_listener(iso, *state);
      resolver->Resolve(ctx, credential).Check();
      state->resolver.Reset();
      state->ctx.Reset();
      state->authenticator.reset();
    }

    void resolve_webauthn_get_state(const std::shared_ptr<webauthn_op_state>& state) {
      auto* iso = state->iso;
      if (host_for_isolate(iso) == nullptr) {
        delete state->abort_ctx;
        state->abort_ctx = nullptr;
        return;
      }
      v8::Locker locker(iso);
      Isolate::Scope iscope(iso);
      HandleScope hs(iso);
      auto ctx = state->ctx.Get(iso);
      if (ctx.IsEmpty()) {
        delete state->abort_ctx;
        state->abort_ctx = nullptr;
        return;
      }
      Context::Scope cs(ctx);
      if (state->aborted.load(std::memory_order_acquire)) {
        reject_webauthn_state(iso, *state, make_abort_error(iso, state->abort_reason));
        return;
      }
      if (!state->error.empty() || !state->has_assert_result) {
        const auto message =
            state->error.empty() ? std::string("WebAuthn assertion failed") : state->error;
        reject_webauthn_state(iso, *state, make_named_error(iso, "NotAllowedError", message));
        return;
      }
      auto resolver = state->resolver.Get(iso);
      auto response_obj = make_assertion_response(iso, ctx, state->assert_result);
      auto credential =
          wrap_public_key_credential(iso, ctx, base64url_encode(state->assert_result.credential_id),
                                     state->assert_result.credential_id, state->attachment,
                                     response_obj);
      clear_webauthn_abort_listener(iso, *state);
      resolver->Resolve(ctx, credential).Check();
      state->resolver.Reset();
      state->ctx.Reset();
      state->authenticator.reset();
    }

    void webauthn_abort_listener(const FunctionCallbackInfo<Value>& info) {
      auto* pending = external_ptr<pending_abort_ctx>(info.Data());
      if (!pending || !pending->state)
        return;
      auto state = pending->state;
      if (state->done.load(std::memory_order_acquire))
        return;
      state->aborted.store(true, std::memory_order_release);
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (!state->signal_obj.IsEmpty() && !state->ctx.IsEmpty()) {
        auto ctx = state->ctx.Get(iso);
        if (!ctx.IsEmpty()) {
          Context::Scope cs(ctx);
          auto signal = state->signal_obj.Get(iso);
          Local<Value> reason_value;
          if (signal->Get(ctx, "reason"_v8(iso)).ToLocal(&reason_value) &&
              !reason_value->IsUndefined()) {
            state->abort_reason = to_str(iso, reason_value);
          }
        }
      }
      if (state->authenticator)
        state->authenticator->cancel();
    }

    bool install_webauthn_abort_listener(Isolate* iso, Local<Context> ctx, Local<Object> signal,
                                         const std::shared_ptr<webauthn_op_state>& state) {
      state->signal_obj.Reset(iso, signal);
      Local<Value> add_value;
      if (!signal->Get(ctx, "addEventListener"_v8(iso)).ToLocal(&add_value))
        return false;
      if (!add_value->IsFunction())
        return true;
      auto* abort_ctx = new pending_abort_ctx{state};
      state->abort_ctx = abort_ctx;
      auto listener_maybe =
          Function::New(ctx, webauthn_abort_listener, make_external(iso, abort_ctx));
      if (listener_maybe.IsEmpty()) {
        delete abort_ctx;
        state->abort_ctx = nullptr;
        return false;
      }
      auto listener = listener_maybe.ToLocalChecked();
      state->abort_listener.Reset(iso, listener);
      Local<Value> argv[2] = {"abort"_v8(iso), listener};
      Local<Value> ignored;
      if (!add_value.As<Function>()->Call(ctx, signal, 2, argv).ToLocal(&ignored)) {
        state->abort_listener.Reset();
        delete state->abort_ctx;
        state->abort_ctx = nullptr;
        return false;
      }
      return true;
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

      auto backend = select_webauthn_backend(*policy);
      if (!backend) {
        resolver->Reject(ctx, make_named_error(iso, "NotAllowedError",
                                               "No WebAuthn backend available"))
            .Check();
        return;
      }
      if (!backend->is_platform) {
        webauthn::register_response response;
        if (std::string error = authenticator().register_credential(creation, origin, response);
            !error.empty()) {
          resolver->Reject(ctx, make_named_error(iso, "NotAllowedError", error)).Check();
          return;
        }
        auto response_obj = make_attestation_response(iso, ctx, response, backend->transports);
        auto credential =
            wrap_public_key_credential(iso, ctx, base64url_encode(response.credential_id),
                                       response.credential_id, backend->attachment, response_obj);
        resolver->Resolve(ctx, credential).Check();
        return;
      }

      Local<Object> signal;
      const bool have_signal = get_signal_object(iso, ctx, options, signal);
      auto state = std::make_shared<webauthn_op_state>();
      state->iso = iso;
      state->ctx.Reset(iso, ctx);
      state->resolver.Reset(iso, resolver);
      state->authenticator = backend->platform;
      state->attachment = backend->attachment;
      state->transports = backend->transports;
      if (have_signal && !install_webauthn_abort_listener(iso, ctx, signal, state)) {
        clear_webauthn_abort_listener(iso, *state);
        state->resolver.Reset();
        state->ctx.Reset();
        state->authenticator.reset();
        reject_caught(iso, ctx, tc, resolver, "WebAuthn create options are invalid");
        return;
      }

      std::thread([state, creation = std::move(creation), origin = std::move(origin)]() mutable {
        webauthn::register_response response;
        if (state->authenticator) {
          state->error = state->authenticator->register_credential(creation, origin, response);
        } else {
          state->error = "No WebAuthn backend available";
        }
        if (state->error.empty()) {
          state->register_result = std::move(response);
          state->has_register_result = true;
        }
        runtime::uv_loop_runtime::instance().post([state] { resolve_webauthn_create_state(state); });
        state->done.store(true, std::memory_order_release);
      }).detach();
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

      auto backend = select_webauthn_backend(*policy);
      if (!backend) {
        resolver->Reject(ctx, make_named_error(iso, "NotAllowedError",
                                               "No WebAuthn backend available"))
            .Check();
        return;
      }
      if (!backend->is_platform) {
        webauthn::assert_response response;
        if (std::string error = authenticator().assert_credential(request, origin, response);
            !error.empty()) {
          resolver->Reject(ctx, make_named_error(iso, "NotAllowedError", error)).Check();
          return;
        }
        auto response_obj = make_assertion_response(iso, ctx, response);
        auto credential =
            wrap_public_key_credential(iso, ctx, base64url_encode(response.credential_id),
                                       response.credential_id, backend->attachment, response_obj);
        resolver->Resolve(ctx, credential).Check();
        return;
      }

      Local<Object> signal;
      const bool have_signal = get_signal_object(iso, ctx, options, signal);
      auto state = std::make_shared<webauthn_op_state>();
      state->iso = iso;
      state->ctx.Reset(iso, ctx);
      state->resolver.Reset(iso, resolver);
      state->authenticator = backend->platform;
      state->attachment = backend->attachment;
      state->transports = backend->transports;
      if (have_signal && !install_webauthn_abort_listener(iso, ctx, signal, state)) {
        clear_webauthn_abort_listener(iso, *state);
        state->resolver.Reset();
        state->ctx.Reset();
        state->authenticator.reset();
        reject_caught(iso, ctx, tc, resolver, "WebAuthn get options are invalid");
        return;
      }

      std::thread([state, request = std::move(request), origin = std::move(origin)]() mutable {
        webauthn::assert_response response;
        if (state->authenticator) {
          state->error = state->authenticator->assert_credential(request, origin, response);
        } else {
          state->error = "No WebAuthn backend available";
        }
        if (state->error.empty()) {
          state->assert_result = std::move(response);
          state->has_assert_result = true;
        }
        runtime::uv_loop_runtime::instance().post([state] { resolve_webauthn_get_state(state); });
        state->done.store(true, std::memory_order_release);
      }).detach();
    }

    void is_user_verifying_platform_available(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      const bool available =
          webauthn::platform_authenticator::is_user_verifying_platform_available();
      resolver->Resolve(ctx, Boolean::New(iso, available)).Check();
      info.GetReturnValue().Set(resolver->GetPromise());
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
             FunctionTemplate::New(iso, is_user_verifying_platform_available));
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
