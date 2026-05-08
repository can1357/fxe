#pragma once

#include <cstdint>
#include <fxe/types.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::runtime {

  enum class update_channel { stable, beta, alpha };

  struct update_descriptor {
    std::string version;
    std::string url;
    std::string sha256;
    std::string signature;
    std::string signature_algorithm;
    std::string canonical_manifest;
    std::string expected_public_key;
    std::string user_data_dir;
    std::string expected_signing_authority;
    std::string expected_subject;
    update_channel channel = update_channel::stable;
    std::vector<u8> artifact;
  };

  bool ed25519_verify(std::span<const u8> sig, std::span<const u8> message,
                      std::span<const u8> public_key);

  bool verify_manifest_signature(std::string_view signature_b64,
                                 std::string_view canonical_manifest,
                                 std::string_view expected_public_key_b64, std::string& error_out);

  class updater {
  public:
    // Stage: write bytes to tmpdir, verify sha256 + signature, atomic-rename into staging dir.
    static std::optional<std::string> stage(const update_descriptor& d, std::string& error_out);

    // v1 apply records the pending marker as consumed; platform relauncher swap is intentionally
    // later.
    static bool apply_pending(std::string& error_out);

    static bool rollback(std::string& error_out);
    static std::vector<std::string> history(std::string& error_out);

    static bool set_channel(update_channel channel, std::string& error_out);
    static update_channel channel();
    static const char* channel_name(update_channel channel);
    static std::optional<update_channel> parse_channel(std::string_view channel);
    static std::string substitute_channel(std::string_view feed_url);
    static std::string device_id(std::string& error_out);
    static bool rollout_eligible(int rollout_percent, std::string_view device_id);
  };

} // namespace fxe::runtime
