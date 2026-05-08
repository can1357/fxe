// Fallback platform_authenticator implementation for builds where no real
// backend is available (e.g. Linux without libfido2). Each entry-point
// returns a clear error / nullptr so callers can fall back to the virtual
// authenticator in dev or surface "not supported" in release.

#include <fxe/webauthn.hpp>

#include <memory>

namespace fxe::webauthn {

  bool platform_authenticator::is_available() { return false; }
  bool platform_authenticator::is_user_verifying_platform_available() { return false; }
  std::unique_ptr<platform_authenticator> platform_authenticator::create() { return nullptr; }

} // namespace fxe::webauthn
