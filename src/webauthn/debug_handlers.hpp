#pragma once

#include "../debug/dispatch.hpp"

#include <fxe/types.hpp>
#include <span>
#include <string_view>

namespace fxe::webauthn {
  struct virtual_credential;
  struct register_response;
  struct assert_response;
} // namespace fxe::webauthn

namespace fxe::webauthn::debug {
  using dispatch_fn = nlohmann::ordered_json (*)(fxe::debug::dispatch_context&,
                                                 const nlohmann::ordered_json&);

  struct registered_method {
    const char* name;
    dispatch_fn fn;
  };

  std::span<const registered_method> handler_table();
  std::span<const std::string_view> schema_capabilities();

  // Called by the JS surface after a successful navigator.credentials.create().
  void on_credential_registered(std::string_view rp_id, const register_response& r,
                                const virtual_credential& cred);
  // Called by the JS surface after a successful navigator.credentials.get().
  void on_credential_asserted(std::string_view rp_id, const assert_response& r,
                              const virtual_credential& cred);

  void register_webauthn_dispatch_handlers();
} // namespace fxe::webauthn::debug
