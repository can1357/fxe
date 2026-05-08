#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fxe::webauthn {
  struct virtual_credential;
}

namespace fxe::webauthn::detail {

  class credential_jar {
  public:
    static std::unique_ptr<credential_jar> open(const std::filesystem::path& path);
    ~credential_jar();
    credential_jar(const credential_jar&) = delete;
    credential_jar& operator=(const credential_jar&) = delete;

    std::vector<virtual_credential> load_all();
    bool upsert(const virtual_credential& cred);
    bool bump_sign_count(const std::vector<uint8_t>& credential_id, uint32_t new_value);
    bool remove(const std::vector<uint8_t>& credential_id);
    bool clear();

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
    explicit credential_jar(std::unique_ptr<impl> i);
  };

} // namespace fxe::webauthn::detail
