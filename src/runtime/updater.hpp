#pragma once

#include <cstdint>
#include <filesystem>
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

  struct update_manifest_v2 {
    std::string version;
    update_channel channel = update_channel::stable;
    uint32_t rollout_percent = 100;
    std::string platform;
    std::string arch;
    struct artifact {
      std::string kind;
      std::string url;
      std::string sha256;
      int64_t size = 0;
      std::string from_version;
      std::string target_sha256;
      std::string code_signature;
    };
    std::vector<artifact> artifacts;
    std::vector<uint8_t> signature;
    std::vector<uint8_t> canonical_bytes;
  };

  bool ed25519_verify(std::span<const u8> sig, std::span<const u8> message,
                      std::span<const u8> public_key);

  bool verify_manifest_signature(std::string_view signature_b64,
                                 std::string_view canonical_manifest,
                                 std::string_view expected_public_key_b64, std::string& error_out);

  // Parse a Sparkle appcast RSS/XML document into update_manifest_v2 metadata. The parser
  // selects the first host-matching item when `sparkle:os` is present, otherwise the first
  // item, emits exactly one `full` artifact, and leaves `arch`, `signature`, and
  // `canonical_bytes` empty. Sparkle ed signatures are not verified here; callers must
  // perform any trust checks separately.
  std::optional<update_manifest_v2> parse_appcast_xml(std::string_view xml, std::string& error_out);

  // Parse a Squirrel RELEASES feed into update_manifest_v2 metadata. Artifact URLs are the
  // raw filenames from the feed and may need caller-side resolution against the feed URL;
  // `platform`, `arch`, `signature`, and `canonical_bytes` stay empty, and delta entries
  // are lossy (`from_version`/`target_sha256` remain empty). The parser normalizes metadata
  // only and does not verify Squirrel sha1 values.
  std::optional<update_manifest_v2> parse_squirrel_releases(std::string_view text,
                                                            std::string& error_out);
  std::optional<update_manifest_v2> parse_manifest_v2_cbor(const std::vector<uint8_t>& bytes,
                                                           std::string& error_out);

  bool apply_bsdiff(const std::vector<uint8_t>& old_bytes, const std::vector<uint8_t>& patch,
                    std::vector<uint8_t>& out, std::string& err);

  bool apply_bsdiff_delta(const std::filesystem::path& patch_path,
                          const std::filesystem::path& current_path,
                          const std::filesystem::path& staged_path,
                          std::string_view expected_target_sha256, std::string& error_out);

  class updater {
  public:
    // Stage: write bytes to tmpdir, verify sha256 + signature, atomic-rename into staging dir.
    static std::optional<std::string> stage(const update_descriptor& d, std::string& error_out);

    // v1 apply records the pending marker as consumed; platform relauncher swap is intentionally
    // later.
    static bool apply_pending(std::string& error_out);

    // After apply_pending(), the next launch must call mark_ready() within a
    // configurable health window or the launcher rolls back. Returns true if a
    // pending-first-launch flag existed and was cleared.
    static bool mark_ready();

    // Inspect: pending-first-launch flag set?
    static bool has_pending_first_launch();

    // Auto-rollback if the previous launch didn't mark ready. Call this once at
    // startup. Returns the version that was rolled back FROM, or empty string if
    // no rollback occurred. Caller should record this in history / alert the user.
    static std::string auto_rollback_if_unready();

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
