#include "updater.hpp"

#include "../os/os.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sodium.h>
#include <sstream>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <softpub.h>
#include <wincrypt.h>
#include <windows.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#endif

namespace fxe::runtime {
  namespace {

    void ensure_sodium_initialized() {
      static std::once_flag flag;
      std::call_once(flag, []() {
        if (sodium_init() < 0) {
          // sodium_init returns -1 on failure, 0 on success, 1 if already initialized.
          // A failure here means the platform RNG is unusable; abort hard so we never
          // run signature verification on an uninitialized libsodium.
          std::abort();
        }
      });
    }

    std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view input) {
      ensure_sodium_initialized();
      std::vector<std::uint8_t> out;
      // Worst-case 3 raw bytes per 4 b64 chars; over-allocate then shrink.
      out.resize(input.size());
      std::size_t out_len = 0;
      const char* end = nullptr;
      if (sodium_base642bin(out.data(), out.size(), input.data(), input.size(),
                            " \t\n\r", // ignore whitespace
                            &out_len, &end, sodium_base64_VARIANT_ORIGINAL) != 0) {
        return std::nullopt;
      }
      // Reject any trailing non-whitespace after the encoded payload.
      const char* input_end = input.data() + input.size();
      while (end != input_end) {
        const char c = *end++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
          return std::nullopt;
      }
      out.resize(out_len);
      return out;
    }

    std::string ascii_lower(std::string s) {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    std::string filename_from_url(std::string_view url) {
      const std::size_t q = url.find_first_of("?#");
      const std::string_view clean = q == std::string_view::npos ? url : url.substr(0, q);
      const std::size_t slash = clean.find_last_of("/\\");
      std::string name =
          std::string(slash == std::string_view::npos ? clean : clean.substr(slash + 1));
      if (name.empty())
        name = "update.bin";
      for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0')
          c = '_';
      }
      return name;
    }

    bool write_all(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                   std::string& error_out) {
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (!f) {
        error_out = "cannot open update staging file for write: " + path.string();
        return false;
      }
      f.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!f) {
        error_out = "failed to write update staging file: " + path.string();
        return false;
      }
      return true;
    }

    bool rename_or_copy(const std::filesystem::path& from, const std::filesystem::path& to,
                        std::string& error_out) {
      std::error_code ec;
      std::filesystem::rename(from, to, ec);
      if (!ec)
        return true;
#ifdef EXDEV
      if (ec.value() != EXDEV) {
        error_out = "failed to move staged update into place: " + ec.message();
        return false;
      }
#else
      if (ec) {
        error_out = "failed to move staged update into place: " + ec.message();
        return false;
      }
#endif
      const auto tmp = to;
      std::filesystem::copy_file(from, tmp, std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        error_out = "failed to copy staged update across filesystems: " + ec.message();
        return false;
      }
      std::filesystem::remove(from, ec);
      return true;
    }

    std::filesystem::path user_data_root(std::string_view override_dir = {}) {
      std::filesystem::path user_data;
      if (override_dir.empty())
        user_data = fxe::os::get_path("userData");
      else
        user_data = std::filesystem::path(override_dir);
      if (user_data.empty())
        user_data = std::filesystem::temp_directory_path() / "fxe";
      return user_data;
    }

    std::filesystem::path updates_root(std::string_view override_dir = {}) {
      return user_data_root(override_dir) / "updates";
    }

    std::string read_first_line(const std::filesystem::path& path) {
      std::ifstream f(path, std::ios::binary);
      std::string line;
      std::getline(f, line);
      return line;
    }

    std::vector<std::string> read_history_file(const std::filesystem::path& root) {
      std::vector<std::string> out;
      std::unordered_set<std::string> seen;
      std::ifstream f(root / "history.txt", std::ios::binary);
      std::string line;
      while (std::getline(f, line)) {
        if (line.empty() || !seen.insert(line).second)
          continue;
        out.push_back(line);
      }
      return out;
    }

    bool write_history_file(const std::filesystem::path& root,
                            const std::vector<std::string>& history, std::string& error_out) {
      std::error_code ec;
      std::filesystem::create_directories(root, ec);
      if (ec) {
        error_out = "cannot create update history directory: " + ec.message();
        return false;
      }
      std::ofstream f(root / "history.txt", std::ios::binary | std::ios::trunc);
      if (!f) {
        error_out = "cannot write update history";
        return false;
      }
      for (const auto& version : history)
        f << version << "\n";
      return static_cast<bool>(f);
    }

    void prune_old_updates(const std::filesystem::path& root,
                           const std::vector<std::string>& keep) {
      std::unordered_set<std::string> keep_set(keep.begin(), keep.end());
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec)
          break;
        if (!entry.is_directory(ec))
          continue;
        const auto name = entry.path().filename().string();
        if (!keep_set.contains(name))
          std::filesystem::remove_all(entry.path(), ec);
      }
    }

    bool record_installed_version(const std::filesystem::path& root, const std::string& version,
                                  std::string& error_out) {
      auto history = read_history_file(root);
      std::vector<std::string> next;
      next.reserve(3);
      next.push_back(version);
      for (const auto& item : history) {
        if (item != version && next.size() < 3)
          next.push_back(item);
      }
      if (!write_history_file(root, next, error_out))
        return false;
      prune_old_updates(root, next);
      return true;
    }

    std::optional<std::filesystem::path> update_payload_path(const std::filesystem::path& dir) {
      std::error_code ec;
      for (const auto marker : {".pending", ".staged", ".rolledback"}) {
        const auto path = read_first_line(dir / marker);
        if (!path.empty() && std::filesystem::exists(path, ec))
          return std::filesystem::path(path);
      }
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
          break;
        const auto name = entry.path().filename().string();
        if (!name.empty() && name.front() != '.' && entry.is_regular_file(ec))
          return entry.path();
      }
      return std::nullopt;
    }

    bool write_marker(const std::filesystem::path& marker, const std::filesystem::path& payload,
                      std::string& error_out) {
      std::ofstream f(marker, std::ios::binary | std::ios::trunc);
      if (!f) {
        error_out = "cannot write update marker: " + marker.string();
        return false;
      }
      f << payload.string() << "\n";
      return static_cast<bool>(f);
    }

    std::mutex& channel_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    update_channel& selected_channel() {
      static update_channel channel = update_channel::stable;
      return channel;
    }

    std::uint64_t fnv1a64(std::string_view value) {
      std::uint64_t hash = 1469598103934665603ull;
      for (char ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
      }
      return hash;
    }

    std::string make_uuid_v4() {
      ensure_sodium_initialized();
      std::array<unsigned char, 16> bytes{};
      randombytes_buf(bytes.data(), bytes.size());
      bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
      bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
      static constexpr char k_hex[] = "0123456789abcdef";
      std::string out;
      out.reserve(36);
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
          out.push_back('-');
        out.push_back(k_hex[bytes[i] >> 4]);
        out.push_back(k_hex[bytes[i] & 0x0f]);
      }
      return out;
    }

    std::string shell_quote(const std::filesystem::path& path) {
      std::string s = path.string();
      std::string out = "'";
      for (char c : s) {
        if (c == '\'')
          out += "'\\''";
        else
          out.push_back(c);
      }
      out.push_back('\'');
      return out;
    }

#ifdef __APPLE__
    std::string run_command_capture(const std::string& command) {
      std::array<char, 256> buffer{};
      std::string out;
      FILE* pipe = popen(command.c_str(), "r");
      if (!pipe)
        return out;
      while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        out += buffer.data();
      (void)pclose(pipe);
      return out;
    }
#endif

    bool verify_platform_code_signature(const std::filesystem::path& artifact,
                                        std::string_view expected_authority,
                                        std::string_view expected_subject, std::string& error_out) {
      if (expected_authority.empty() && expected_subject.empty())
        return true;
#ifdef __APPLE__
      const std::string quoted = shell_quote(artifact);
      if (std::system(("/usr/bin/codesign --verify --deep --strict " + quoted).c_str()) != 0) {
        error_out = "update artifact code-signature verification failed";
        return false;
      }
      const std::string details =
          run_command_capture("/usr/bin/codesign -dvvv --verbose=4 " + quoted + " 2>&1");
      if (!expected_authority.empty() &&
          details.find("Authority=" + std::string(expected_authority)) == std::string::npos) {
        error_out = "update artifact signing authority mismatch";
        return false;
      }
      if (!expected_subject.empty() &&
          details.find(std::string(expected_subject)) == std::string::npos) {
        error_out = "update artifact signing subject mismatch";
        return false;
      }
      return true;
#elif defined(_WIN32)
      std::wstring wide = artifact.wstring();
      WINTRUST_FILE_INFO file_info{};
      file_info.cbStruct = sizeof(file_info);
      file_info.pcwszFilePath = wide.c_str();
      GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
      WINTRUST_DATA trust_data{};
      trust_data.cbStruct = sizeof(trust_data);
      trust_data.dwUIChoice = WTD_UI_NONE;
      trust_data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
      trust_data.dwUnionChoice = WTD_CHOICE_FILE;
      trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
      trust_data.pFile = &file_info;
      LONG status = WinVerifyTrust(nullptr, &policy, &trust_data);
      trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
      (void)WinVerifyTrust(nullptr, &policy, &trust_data);
      if (status != ERROR_SUCCESS) {
        error_out = "update artifact WinTrust verification failed";
        return false;
      }
      (void)expected_authority;
      (void)expected_subject;
      return true;
#else
      error_out = "update artifact code-signature verification is not supported on this platform";
      return false;
#endif
    }

    std::vector<std::uint8_t> hex_bytes(std::string_view hex) {
      auto hex_value = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9')
          return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
          return static_cast<std::uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
          return static_cast<std::uint8_t>(c - 'A' + 10);
        return 0;
      };
      std::vector<std::uint8_t> out;
      out.reserve(hex.size() / 2);
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((hex_value(hex[i]) << 4) | hex_value(hex[i + 1])));
      return out;
    }

    struct ed25519_self_test {
      ed25519_self_test() {
        const auto pk =
            hex_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        const auto sig =
            hex_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        const std::array<std::uint8_t, 0> msg{};
        if (!ed25519_verify(sig, msg, pk))
          std::abort();
      }
    };

    static const ed25519_self_test k_ed25519_self_test;

  } // namespace

  bool ed25519_verify(std::span<const std::uint8_t> sig, std::span<const std::uint8_t> message,
                      std::span<const std::uint8_t> public_key) {
    if (sig.size() != 64 || public_key.size() != 32)
      return false;
    ensure_sodium_initialized();
    return crypto_sign_verify_detached(sig.data(), message.data(), message.size(),
                                       public_key.data()) == 0;
  }

  bool verify_manifest_signature(std::string_view signature_b64,
                                 std::string_view canonical_manifest,
                                 std::string_view expected_public_key_b64, std::string& error_out) {
    auto sig = base64_decode(signature_b64);
    if (!sig || sig->size() != 64) {
      error_out = "signature verification failed";
      return false;
    }
    auto pk = base64_decode(expected_public_key_b64);
    if (!pk || pk->size() != 32) {
      error_out = "signature verification failed";
      return false;
    }
    const auto* msg = reinterpret_cast<const std::uint8_t*>(canonical_manifest.data());
    if (!ed25519_verify(*sig, std::span<const std::uint8_t>(msg, canonical_manifest.size()), *pk)) {
      error_out = "signature verification failed";
      return false;
    }
    return true;
  }

  std::optional<std::string> updater::stage(const update_descriptor& d, std::string& error_out) {
    if (d.version.empty()) {
      error_out = "update manifest version is required";
      return std::nullopt;
    }
    if (d.url.empty()) {
      error_out = "update manifest url is required";
      return std::nullopt;
    }
    if (d.artifact.empty()) {
      error_out = "update artifact is empty";
      return std::nullopt;
    }
    if (d.sha256.size() != 64) {
      error_out = "update manifest sha256 is invalid";
      return std::nullopt;
    }
    if (!d.signature.empty()) {
      if (d.expected_public_key.empty()) {
        error_out = "signed update manifest requires expectedPublicKey";
        return std::nullopt;
      }
      if (!d.signature_algorithm.empty() && ascii_lower(d.signature_algorithm) != "ed25519") {
        error_out = "unsupported update signatureAlgorithm";
        return std::nullopt;
      }
      if (!verify_manifest_signature(d.signature, d.canonical_manifest, d.expected_public_key,
                                     error_out))
        return std::nullopt;
    }

    ensure_sodium_initialized();
    std::array<std::uint8_t, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), d.artifact.data(), d.artifact.size());
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string actual_hex;
    actual_hex.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
      actual_hex.push_back(k_hex[byte >> 4]);
      actual_hex.push_back(k_hex[byte & 0x0f]);
    }
    if (actual_hex != ascii_lower(d.sha256)) {
      error_out = "update artifact sha256 mismatch";
      return std::nullopt;
    }

    std::filesystem::path dir = updates_root(d.user_data_dir) / d.version;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      error_out = "cannot create update staging directory: " + ec.message();
      return std::nullopt;
    }

    const std::filesystem::path final_path = dir / filename_from_url(d.url);
    const std::filesystem::path partial_path = final_path.string() + ".partial";
    std::filesystem::remove(partial_path, ec);
    if (!write_all(partial_path, d.artifact, error_out))
      return std::nullopt;
    if (!verify_platform_code_signature(partial_path, d.expected_signing_authority,
                                        d.expected_subject, error_out)) {
      std::filesystem::remove(partial_path, ec);
      return std::nullopt;
    }
    std::filesystem::remove(final_path, ec);
    if (!rename_or_copy(partial_path, final_path, error_out))
      return std::nullopt;

    const std::filesystem::path marker = dir / ".staged";
    {
      std::ofstream m(marker, std::ios::binary | std::ios::trunc);
      if (!m) {
        error_out = "cannot write update staged marker: " + marker.string();
        return std::nullopt;
      }
      m << final_path.string() << "\n";
    }
    return final_path.string();
  }

  bool updater::apply_pending(std::string& error_out) {
    const auto root = updates_root();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
      return true;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec)
        break;
      if (!entry.is_directory(ec))
        continue;
      const auto marker = entry.path() / ".staged";
      if (std::filesystem::exists(marker, ec)) {
        const auto pending = entry.path() / ".pending";
        std::filesystem::rename(marker, pending, ec);
        if (ec) {
          error_out = "failed to mark staged update pending: " + ec.message();
          return false;
        }
        return record_installed_version(root, entry.path().filename().string(), error_out);
      }
    }
    return true;
  }

  bool updater::rollback(std::string& error_out) {
    const auto root = updates_root();
    auto versions = read_history_file(root);
    if (versions.size() < 2) {
      error_out = "no previous update version is available";
      return false;
    }
    const std::string current = versions[0];
    const std::string previous = versions[1];
    const auto previous_dir = root / previous;
    auto payload = update_payload_path(previous_dir);
    if (!payload) {
      error_out = "previous update payload is missing";
      return false;
    }
    std::error_code ec;
    const auto current_pending = root / current / ".pending";
    if (std::filesystem::exists(current_pending, ec)) {
      std::filesystem::rename(current_pending, root / current / ".rolledback", ec);
      if (ec) {
        error_out = "failed to mark current update rolled back: " + ec.message();
        return false;
      }
    }
    if (!write_marker(previous_dir / ".pending", *payload, error_out))
      return false;

    std::vector<std::string> next;
    next.reserve(3);
    next.push_back(previous);
    next.push_back(current);
    for (std::size_t i = 2; i < versions.size() && next.size() < 3; ++i) {
      if (versions[i] != previous && versions[i] != current)
        next.push_back(versions[i]);
    }
    if (!write_history_file(root, next, error_out))
      return false;
    prune_old_updates(root, next);
    return true;
  }

  std::vector<std::string> updater::history(std::string& error_out) {
    (void)error_out;
    auto out = read_history_file(updates_root());
    if (out.size() > 3)
      out.resize(3);
    return out;
  }

  bool updater::set_channel(update_channel channel, std::string& error_out) {
    (void)error_out;
    std::lock_guard<std::mutex> lock(channel_mutex());
    selected_channel() = channel;
    return true;
  }

  update_channel updater::channel() {
    std::lock_guard<std::mutex> lock(channel_mutex());
    return selected_channel();
  }

  const char* updater::channel_name(update_channel channel) {
    switch (channel) {
    case update_channel::stable:
      return "stable";
    case update_channel::beta:
      return "beta";
    case update_channel::alpha:
      return "alpha";
    }
    return "stable";
  }

  std::optional<update_channel> updater::parse_channel(std::string_view channel) {
    if (channel == "stable")
      return update_channel::stable;
    if (channel == "beta")
      return update_channel::beta;
    if (channel == "alpha")
      return update_channel::alpha;
    return std::nullopt;
  }

  std::string updater::substitute_channel(std::string_view feed_url) {
    std::string out(feed_url);
    const std::string name = channel_name(channel());
    std::size_t pos = 0;
    while ((pos = out.find("{channel}", pos)) != std::string::npos) {
      out.replace(pos, std::strlen("{channel}"), name);
      pos += name.size();
    }
    return out;
  }

  std::string updater::device_id(std::string& error_out) {
    const auto user_data = user_data_root();
    const auto path = user_data / "update_device_id";
    std::error_code ec;
    std::filesystem::create_directories(user_data, ec);
    if (ec) {
      error_out = "cannot create userData directory for update device id: " + ec.message();
      return {};
    }
    std::string existing = read_first_line(path);
    if (!existing.empty())
      return existing;
    std::string id = make_uuid_v4();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
      error_out = "cannot persist update device id";
      return {};
    }
    f << id << "\n";
    if (!f) {
      error_out = "cannot persist update device id";
      return {};
    }
    return id;
  }

  bool updater::rollout_eligible(int rollout_percent, std::string_view device_id) {
    if (rollout_percent <= 0)
      return false;
    if (rollout_percent >= 100)
      return true;
    return static_cast<int>(fnv1a64(device_id) % 100) < rollout_percent;
  }

} // namespace fxe::runtime
