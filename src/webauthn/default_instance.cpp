#include <fxe/webauthn.hpp>

namespace fxe::webauthn {
  virtual_authenticator& default_virtual_authenticator() {
    static virtual_authenticator instance;
    return instance;
  }
} // namespace fxe::webauthn
