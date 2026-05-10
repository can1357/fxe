#pragma once

#include <fxe/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fxe::runtime::fxa_archive {

  struct Entry {
    u64 offset = 0;
    u64 size = 0;
  };

  struct ManifestMetadata {
    std::string app_name;
    std::string version;
    std::string entry;
    std::string created_at;
    std::string compression;
    std::string update_url;
    std::string public_key;
    std::string channel;
    std::string signer_secret_key_b64;
    std::string signer_public_key_b64;
  };

  struct PackOptions {
    bool sign = false;
    std::string secret_key_b64;
    std::string public_key_b64;
    bool compress = true;
  };

  bool pack_files(const std::string& binary_path,
                  const std::vector<std::pair<std::string, std::string>>& files,
                  const ManifestMetadata& meta, const PackOptions& opts,
                  std::string* error = nullptr);

  class Bundle {
  public:
    explicit Bundle(std::string path);

    bool valid() const noexcept {
      return valid_;
    }

    bool signed_archive() const noexcept {
      return signed_archive_;
    }

    bool signature_verified() const noexcept {
      return signature_verified_;
    }

    const std::vector<u8>& signer_pubkey() const noexcept {
      return signer_pubkey_;
    }

    std::optional<std::string> read(std::string_view name) const;
    std::vector<std::string> list() const;

    const std::string& payload_sha256() const noexcept {
      return payload_sha256_;
    }

  private:
    std::string path_;
    bool valid_ = false;
    bool signed_archive_ = false;
    bool signature_verified_ = false;
    u64 payload_offset_ = 0;
    std::string compression_;
    std::string payload_sha256_;
    std::vector<u8> signer_pubkey_;
    std::vector<u8> decompressed_payload_;
    std::unordered_map<std::string, Entry> index_;
    std::vector<std::string> names_;
  };

} // namespace fxe::runtime::fxa_archive
