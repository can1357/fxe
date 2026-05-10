#pragma once

#include <mbedtls/error.h>

#include <array>
#include <string>

namespace fxe::webauthn {

  // Format an mbedTLS error code into a human-readable string. Buffer is
  // 256 bytes — large enough for every message in mbedtls/error.c.
  inline std::string mbedtls_err_str(int rc) {
    std::array<char, 256> buf{};
    mbedtls_strerror(rc, buf.data(), buf.size());
    return buf.data();
  }

} // namespace fxe::webauthn
