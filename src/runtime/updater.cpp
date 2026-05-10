#include "updater.hpp"
#include "cbor.hpp"

#include "../os/os.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <fxe/string_utils.hpp>
#include <fxe/types.hpp>
#include <limits>
#include <mutex>
#include <pugixml.hpp>
#include <sodium.h>
#include <sstream>
#include <system_error>
#include <unordered_set>

#ifndef _WIN32
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
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

    std::optional<std::vector<u8>> base64_decode(std::string_view input) {
      ensure_sodium_initialized();
      std::vector<u8> out;
      // Worst-case 3 raw bytes per 4 b64 chars; over-allocate then shrink.
      out.resize(input.size());
      usize out_len = 0;
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

    std::string filename_from_url(std::string_view url) {
      const usize q = url.find_first_of("?#");
      const std::string_view clean = q == std::string_view::npos ? url : url.substr(0, q);
      const usize slash = clean.find_last_of("/\\");
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

    bool write_all(const std::filesystem::path& path, std::span<const u8> bytes,
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
    bool read_all(const std::filesystem::path& path, std::vector<u8>& bytes,
                  std::string& error_out) {
      std::ifstream f(path, std::ios::binary);
      if (!f) {
        error_out = "cannot open file for read: " + path.string();
        return false;
      }
      f.seekg(0, std::ios::end);
      const auto end = f.tellg();
      if (end < 0) {
        error_out = "cannot determine file size: " + path.string();
        return false;
      }
      bytes.resize(static_cast<size_t>(end));
      f.seekg(0, std::ios::beg);
      if (!bytes.empty())
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!f && !bytes.empty()) {
        error_out = "failed to read file: " + path.string();
        return false;
      }
      return true;
    }

    std::string sha256_hex(std::span<const u8> bytes) {
      ensure_sodium_initialized();
      std::array<u8, crypto_hash_sha256_BYTES> digest{};
      crypto_hash_sha256(digest.data(), bytes.data(), bytes.size());
      static constexpr char k_hex[] = "0123456789abcdef";
      std::string out;
      out.reserve(digest.size() * 2);
      for (u8 byte : digest) {
        out.push_back(k_hex[byte >> 4]);
        out.push_back(k_hex[byte & 0x0f]);
      }
      return out;
    }

    bool is_hex_sha256(std::string_view value) {
      if (value.size() != 64)
        return false;
      for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
          return false;
      }
      return true;
    }

    std::optional<int64_t> parse_i64_decimal(std::string_view text) {
      text = trim(text);
      if (text.empty())
        return std::nullopt;
      int64_t value = 0;
      for (char c : text) {
        if (c < '0' || c > '9')
          return std::nullopt;
        const int digit = c - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10)
          return std::nullopt;
        value = value * 10 + digit;
      }
      return value;
    }

    bool is_hex_sha1(std::string_view value) {
      if (value.size() != 40)
        return false;
      for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
          return false;
      }
      return true;
    }

    std::string_view host_sparkle_os() {
#if defined(__APPLE__)
      return "macos";
#elif defined(_WIN32)
      return "windows";
#else
      return {};
#endif
    }

    std::string_view normalize_sparkle_platform(std::string_view sparkle_os) {
      if (sparkle_os == "macos")
        return "darwin";
      if (sparkle_os == "windows")
        return "win32";
      return {};
    }

    bool extract_last_dotted_version(std::string_view text, std::string& out) {
      bool found = false;
      usize best_start = 0;
      usize best_end = 0;
      usize i = 0;
      while (i < text.size()) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
          ++i;
          continue;
        }
        const usize start = i;
        bool saw_dot = false;
        while (i < text.size()) {
          const char c = text[i];
          if (std::isdigit(static_cast<unsigned char>(c))) {
            ++i;
            continue;
          }
          if (c == '.') {
            saw_dot = true;
            ++i;
            continue;
          }
          break;
        }
        usize end = i;
        while (end > start && text[end - 1] == '.')
          --end;
        if (saw_dot && end > start) {
          found = true;
          best_start = start;
          best_end = end;
        }
      }
      if (!found)
        return false;
      out.assign(text.substr(best_start, best_end - best_start));
      return true;
    }

    std::vector<uint32_t> parse_version_parts(std::string_view version) {
      std::vector<uint32_t> out;
      usize start = 0;
      while (start <= version.size()) {
        const usize end = version.find('.', start);
        const std::string_view token = end == std::string_view::npos
                                           ? version.substr(start)
                                           : version.substr(start, end - start);
        uint64_t part = 0;
        for (char c : token) {
          if (c < '0' || c > '9') {
            part = 0;
            break;
          }
          part = part * 10 + static_cast<uint64_t>(c - '0');
          if (part > std::numeric_limits<uint32_t>::max()) {
            part = std::numeric_limits<uint32_t>::max();
            break;
          }
        }
        out.push_back(static_cast<uint32_t>(part));
        if (end == std::string_view::npos)
          break;
        start = end + 1;
      }
      return out;
    }

    int compare_versions(std::string_view lhs, std::string_view rhs) {
      const auto left = parse_version_parts(lhs);
      const auto right = parse_version_parts(rhs);
      const usize count = std::max(left.size(), right.size());
      for (usize i = 0; i < count; ++i) {
        const uint32_t l = i < left.size() ? left[i] : 0;
        const uint32_t r = i < right.size() ? right[i] : 0;
        if (l < r)
          return -1;
        if (l > r)
          return 1;
      }
      return 0;
    }

    const cbor::map* cbor_map(const cbor::value& v) {
      return std::get_if<cbor::map>(&static_cast<const cbor::storage&>(v));
    }

    const cbor::array* cbor_array(const cbor::value& v) {
      return std::get_if<cbor::array>(&static_cast<const cbor::storage&>(v));
    }

    const std::string* cbor_text(const cbor::value& v) {
      return std::get_if<std::string>(&static_cast<const cbor::storage&>(v));
    }

    const std::vector<uint8_t>* cbor_bytes(const cbor::value& v) {
      return std::get_if<std::vector<uint8_t>>(&static_cast<const cbor::storage&>(v));
    }

    std::optional<uint64_t> cbor_uint(const cbor::value& v) {
      if (const auto* n = std::get_if<uint64_t>(&static_cast<const cbor::storage&>(v)))
        return *n;
      if (const auto* n = std::get_if<int64_t>(&static_cast<const cbor::storage&>(v)); n && *n >= 0)
        return static_cast<uint64_t>(*n);
      return std::nullopt;
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

    std::filesystem::path pending_first_launch_flag_path(const std::filesystem::path& root) {
      return root / "first-launch.flag";
    }

    bool write_pending_first_launch_flag(const std::filesystem::path& root,
                                         std::string_view version, std::string& error_out) {
      std::error_code ec;
      std::filesystem::create_directories(root, ec);
      if (ec) {
        error_out = "cannot create updates directory for first-launch marker: " + ec.message();
        return false;
      }
      std::ofstream f(pending_first_launch_flag_path(root), std::ios::binary | std::ios::trunc);
      if (!f) {
        error_out = "cannot write first-launch marker";
        return false;
      }
      f << version << "\n";
      if (!f) {
        error_out = "cannot write first-launch marker";
        return false;
      }
      return true;
    }

    std::mutex& channel_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    update_channel& selected_channel() {
      static update_channel channel = update_channel::stable;
      return channel;
    }

    u64 fnv1a64(std::string_view value) {
      u64 hash = 1469598103934665603ull;
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
      for (usize i = 0; i < bytes.size(); ++i) {
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
    std::optional<std::filesystem::path>& platform_swap_destination_override() {
      static std::optional<std::filesystem::path> path;
      return path;
    }

    std::vector<std::string>& last_platform_swap_argv_storage() {
      static std::vector<std::string> argv;
      return argv;
    }

    bool path_looks_like_app_bundle(const std::filesystem::path& path) {
      return path.extension() == ".app";
    }

#ifdef __APPLE__
    std::optional<std::filesystem::path> current_executable_path() {
      u32 size = 0;
      if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0)
        return std::nullopt;
      std::vector<char> buf(size);
      if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return std::nullopt;
      std::error_code ec;
      auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(buf.data()), ec);
      if (ec)
        return std::filesystem::path(buf.data());
      return resolved;
    }

    std::optional<std::filesystem::path> current_bundle_path() {
      if (auto override_path = platform_swap_destination_override(); override_path)
        return *override_path;
      auto exe = current_executable_path();
      if (!exe)
        return std::nullopt;
      for (std::filesystem::path cur = exe->parent_path(); !cur.empty(); cur = cur.parent_path()) {
        if (path_looks_like_app_bundle(cur))
          return cur;
        if (cur == cur.parent_path())
          break;
      }
      return std::nullopt;
    }
#endif

    std::string run_command_capture(const std::string& command, int* exit_code_out = nullptr) {
      std::array<char, 256> buffer{};
      std::string out;
#if defined(_WIN32)
      FILE* pipe = _popen(command.c_str(), "r");
#else
      FILE* pipe = popen(command.c_str(), "r");
#endif
      if (!pipe) {
        if (exit_code_out)
          *exit_code_out = -1;
        return out;
      }
      while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        out += buffer.data();
#if defined(_WIN32)
      const int status = _pclose(pipe);
      if (exit_code_out)
        *exit_code_out = status;
#else
      const int status = pclose(pipe);
      if (exit_code_out)
        *exit_code_out = status == -1 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : status);
#endif
      return out;
    }

#if defined(__linux__)
    bool tool_on_path(std::string_view name) {
      if (name.empty())
        return false;
      if (name.find('/') != std::string_view::npos)
        return access(std::string(name).c_str(), X_OK) == 0;
      const char* path_env = std::getenv("PATH");
      if (!path_env)
        return false;
      std::string_view path(path_env);
      usize start = 0;
      while (start <= path.size()) {
        const usize end = path.find(':', start);
        const std::string_view entry =
            end == std::string_view::npos ? path.substr(start) : path.substr(start, end - start);
        const std::filesystem::path dir =
            entry.empty() ? std::filesystem::current_path() : std::filesystem::path(entry);
        const std::filesystem::path candidate = dir / std::string(name);
        if (access(candidate.string().c_str(), X_OK) == 0)
          return true;
        if (end == std::string_view::npos)
          break;
        start = end + 1;
      }
      return false;
    }

    std::string normalize_fingerprint(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)))
          continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      return out;
    }

    bool is_hex_fingerprint(std::string_view text) {
      if (text.size() != 40)
        return false;
      for (char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
          return false;
      }
      return true;
    }

    bool read_minisign_untrusted_comment(const std::filesystem::path& sig_path,
                                         std::string& comment_out) {
      std::ifstream input(sig_path, std::ios::binary);
      if (!input)
        return false;
      return static_cast<bool>(std::getline(input, comment_out));
    }

    std::optional<std::filesystem::path> make_temp_gpg_home(std::string& error_out) {
      std::string tmpl =
          (std::filesystem::temp_directory_path() / "fxe-updater-gpg-XXXXXX").string();
      std::vector<char> buffer(tmpl.begin(), tmpl.end());
      buffer.push_back('\0');
      char* created = mkdtemp(buffer.data());
      if (!created) {
        error_out = "failed to create temporary gpg homedir";
        return std::nullopt;
      }
      return std::filesystem::path(created);
    }
#endif

    std::vector<u8> hex_bytes(std::string_view hex) {
      auto hex_value = [](char c) -> u8 {
        if (c >= '0' && c <= '9')
          return static_cast<u8>(c - '0');
        if (c >= 'a' && c <= 'f')
          return static_cast<u8>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
          return static_cast<u8>(c - 'A' + 10);
        return 0;
      };
      std::vector<u8> out;
      out.reserve(hex.size() / 2);
      for (usize i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<u8>((hex_value(hex[i]) << 4) | hex_value(hex[i + 1])));
      return out;
    }

    struct ed25519_self_test {
      ed25519_self_test() {
        const auto pk =
            hex_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        const auto sig =
            hex_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        const std::array<u8, 0> msg{};
        if (!ed25519_verify(sig, msg, pk))
          std::abort();
      }
    };

    static const ed25519_self_test k_ed25519_self_test;

  } // namespace

  bool detail::detect_detached_signature(const std::filesystem::path& artifact,
                                         std::filesystem::path& sig_path_out,
                                         std::string& flavor_out) {
    const std::array<std::pair<std::string_view, std::string_view>, 3> candidates = {{
        {".minisig", "minisign"},
        {".sig", "minisign"},
        {".asc", "gpg"},
    }};
    std::error_code ec;
    for (const auto& [suffix, flavor] : candidates) {
      std::filesystem::path candidate = artifact;
      candidate += suffix;
      ec.clear();
      if (std::filesystem::exists(candidate, ec) && !ec) {
        sig_path_out = std::move(candidate);
        flavor_out = std::string(flavor);
        return true;
      }
    }
    sig_path_out.clear();
    flavor_out.clear();
    return false;
  }

  bool detail::verify_platform_code_signature(const std::filesystem::path& artifact,
                                              std::string_view expected_authority,
                                              std::string_view expected_subject,
                                              std::string& error_out) {
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
#elif defined(__linux__)
    if (expected_authority.empty()) {
      error_out = "detached signature verification requires expected_authority";
      return false;
    }
    if (expected_subject.empty()) {
      error_out = "detached signature verification requires expected_subject public key path";
      return false;
    }
    std::filesystem::path sig_path;
    std::string flavor;
    if (!detect_detached_signature(artifact, sig_path, flavor)) {
      error_out =
          "no detached signature found alongside artifact (expected .minisig, .sig, or .asc)";
      return false;
    }
    const std::filesystem::path subject_path(expected_subject);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(subject_path, ec) || ec) {
      error_out = "detached-signature public key file not found: " + subject_path.string();
      return false;
    }
    if (flavor == "minisign") {
      if (!tool_on_path("minisign")) {
        error_out = "minisign is not available on PATH";
        return false;
      }
      std::string first_line;
      if (!read_minisign_untrusted_comment(sig_path, first_line)) {
        error_out = "failed to read minisign signature metadata: " + sig_path.string();
        return false;
      }
      constexpr std::string_view k_untrusted_prefix = "untrusted comment:";
      if (first_line.rfind(std::string(k_untrusted_prefix), 0) != 0) {
        error_out = "minisign signature missing untrusted comment";
        return false;
      }
      const std::string_view comment =
          trim(std::string_view(first_line).substr(k_untrusted_prefix.size()));
      if (!comment.starts_with(expected_authority)) {
        error_out = "minisign untrusted comment mismatch";
        return false;
      }
      int verify_exit = -1;
      const std::string output = run_command_capture("minisign -V -x " + shell_quote(sig_path) +
                                                         " -p " + shell_quote(subject_path) +
                                                         " -m " + shell_quote(artifact) + " 2>&1",
                                                     &verify_exit);
      if (verify_exit != 0) {
        error_out = "minisign verification failed: " + std::string(trim(output));
        return false;
      }
      return true;
    }
    if (flavor == "gpg") {
      if (!tool_on_path("gpg")) {
        error_out = "gpg is not available on PATH";
        return false;
      }
      const std::string expected_fingerprint = normalize_fingerprint(expected_authority);
      if (!is_hex_fingerprint(expected_fingerprint)) {
        error_out = "gpg signing fingerprint must be 40 hexadecimal characters";
        return false;
      }
      auto homedir = make_temp_gpg_home(error_out);
      if (!homedir)
        return false;
      struct gpg_home_cleanup {
        std::filesystem::path path;
        ~gpg_home_cleanup() {
          std::error_code cleanup_error;
          std::filesystem::remove_all(path, cleanup_error);
        }
      } cleanup{*homedir};
      int import_exit = -1;
      const std::string import_output =
          run_command_capture("gpg --batch --homedir " + shell_quote(*homedir) + " --import " +
                                  shell_quote(subject_path) + " 2>&1",
                              &import_exit);
      if (import_exit != 0) {
        error_out = "gpg key import failed: " + std::string(trim(import_output));
        return false;
      }
      int verify_exit = -1;
      const std::string verify_output =
          run_command_capture("gpg --batch --no-auto-check-trustdb --homedir " +
                                  shell_quote(*homedir) + " --status-fd 2 --verify " +
                                  shell_quote(sig_path) + " " + shell_quote(artifact) + " 2>&1",
                              &verify_exit);
      bool saw_goodsig = false;
      bool saw_validsig = false;
      std::string observed_fingerprint;
      std::istringstream status_lines(verify_output);
      std::string line;
      while (std::getline(status_lines, line)) {
        if (line.rfind("[GNUPG:] GOODSIG ", 0) == 0) {
          saw_goodsig = true;
          const std::string_view rest = trim(std::string_view(line).substr(17));
          const usize end = rest.find_first_of(" \t\r\n");
          const std::string token =
              normalize_fingerprint(end == std::string_view::npos ? rest : rest.substr(0, end));
          if (is_hex_fingerprint(token))
            observed_fingerprint = token;
        } else if (line.rfind("[GNUPG:] VALIDSIG ", 0) == 0) {
          saw_validsig = true;
          const std::string_view rest = trim(std::string_view(line).substr(18));
          const usize end = rest.find_first_of(" \t\r\n");
          observed_fingerprint =
              normalize_fingerprint(end == std::string_view::npos ? rest : rest.substr(0, end));
        }
      }
      if (verify_exit != 0) {
        error_out = "gpg verification failed: " + std::string(trim(verify_output));
        return false;
      }
      if (!saw_goodsig && !saw_validsig) {
        error_out = "gpg verification did not report a valid detached signature";
        return false;
      }
      if (!is_hex_fingerprint(observed_fingerprint)) {
        error_out = "gpg verification did not report a signing fingerprint";
        return false;
      }
      if (observed_fingerprint != expected_fingerprint) {
        error_out = "gpg signing fingerprint mismatch";
        return false;
      }
      return true;
    }
    error_out = "unsupported detached signature flavor: " + flavor;
    return false;
#else
    error_out = "update artifact code-signature verification is not supported on this platform";
    return false;
#endif
  }

  bool ed25519_verify(std::span<const u8> sig, std::span<const u8> message,
                      std::span<const u8> public_key) {
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
    const auto* msg = reinterpret_cast<const u8*>(canonical_manifest.data());
    if (!ed25519_verify(*sig, std::span<const u8>(msg, canonical_manifest.size()), *pk)) {
      error_out = "signature verification failed";
      return false;
    }
    return true;
  }

  std::optional<update_manifest_v2> parse_appcast_xml(std::string_view xml,
                                                      std::string& error_out) {
    error_out.clear();
    if (xml.size() > 4u * 1024u * 1024u) {
      error_out = "XML input exceeds 4 MiB limit";
      return std::nullopt;
    }
    // Reject DOCTYPE / ENTITY declarations to prevent XXE-style payloads.
    // pugixml itself never resolves external entities (no DTD loader), so
    // disallowing the markers up-front is sufficient and keeps the strict
    // input contract the previous hand-rolled parser enforced.
    for (usize i = 0; i + 1 < xml.size(); ++i) {
      if (xml[i] != '<' || xml[i + 1] != '!')
        continue;
      const auto rest = xml.substr(i + 2);
      const auto matches_ci = [&](std::string_view needle) {
        if (rest.size() < needle.size())
          return false;
        for (usize j = 0; j < needle.size(); ++j) {
          if (std::tolower(static_cast<unsigned char>(rest[j])) !=
              std::tolower(static_cast<unsigned char>(needle[j])))
            return false;
        }
        return true;
      };
      if (matches_ci("DOCTYPE") || matches_ci("ENTITY")) {
        error_out = "XML DOCTYPE/entity declarations are not allowed";
        return std::nullopt;
      }
    }

    pugi::xml_document doc;
    const pugi::xml_parse_result result =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default);
    if (!result) {
      error_out = std::string("XML parse failed: ") + result.description();
      return std::nullopt;
    }

    const pugi::xml_node root = doc.document_element();
    if (std::string_view(root.name()) != "rss") {
      error_out = "appcast root must be rss";
      return std::nullopt;
    }
    const pugi::xml_node channel = root.child("channel");
    if (!channel) {
      error_out = "appcast missing channel";
      return std::nullopt;
    }

    const auto child_text = [](const pugi::xml_node& node, const char* name) {
      return std::string(trim(node.child(name).child_value()));
    };

    pugi::xml_node first_item;
    pugi::xml_node host_item;
    const std::string_view host_os = host_sparkle_os();
    for (pugi::xml_node child = channel.child("item"); child; child = child.next_sibling("item")) {
      if (!first_item)
        first_item = child;
      if (!host_os.empty() && !host_item) {
        const pugi::xml_node enclosure = child.child("enclosure");
        const std::string_view sparkle_os = trim(enclosure.attribute("sparkle:os").as_string());
        if (!sparkle_os.empty() && sparkle_os == host_os)
          host_item = child;
      }
    }
    const pugi::xml_node item = host_item ? host_item : first_item;
    if (!item) {
      error_out = "appcast has no items";
      return std::nullopt;
    }
    const pugi::xml_node enclosure = item.child("enclosure");
    if (!enclosure) {
      error_out = "appcast item missing enclosure";
      return std::nullopt;
    }
    const std::string_view enclosure_url = trim(enclosure.attribute("url").as_string());
    if (enclosure_url.empty()) {
      error_out = "appcast item missing enclosure url";
      return std::nullopt;
    }

    update_manifest_v2 out;
    out.version = child_text(item, "sparkle:version");
    if (out.version.empty())
      out.version = std::string(trim(enclosure.attribute("sparkle:version").as_string()));
    if (out.version.empty())
      out.version = child_text(item, "sparkle:shortVersionString");
    if (out.version.empty()) {
      error_out = "appcast item missing version";
      return std::nullopt;
    }
    if (const std::string channel_text = child_text(item, "sparkle:channel");
        !channel_text.empty()) {
      if (const auto parsed = updater::parse_channel(channel_text))
        out.channel = *parsed;
    }
    if (const std::string_view platform =
            normalize_sparkle_platform(trim(enclosure.attribute("sparkle:os").as_string()));
        !platform.empty()) {
      out.platform = std::string(platform);
    }
    out.rollout_percent = 100;

    update_manifest_v2::artifact artifact;
    artifact.kind = "full";
    artifact.url = std::string(enclosure_url);
    artifact.sha256 =
        ascii_lower(std::string(trim(enclosure.attribute("sparkle:installerSha256").as_string())));
    if (artifact.sha256.empty()) {
      error_out = "appcast item missing sha256";
      return std::nullopt;
    }
    if (!is_hex_sha256(artifact.sha256)) {
      error_out = "appcast item sha256 is invalid";
      return std::nullopt;
    }
    if (const auto size = parse_i64_decimal(enclosure.attribute("length").as_string()))
      artifact.size = *size;
    out.artifacts.push_back(std::move(artifact));
    return out;
  }

  std::optional<update_manifest_v2> parse_squirrel_releases(std::string_view text,
                                                            std::string& error_out) {
    error_out.clear();
    update_manifest_v2 out;
    std::string best_full_version;
    bool saw_full = false;
    usize offset = 0;
    while (offset <= text.size()) {
      const usize line_end = text.find('\n', offset);
      std::string_view line = line_end == std::string_view::npos
                                  ? text.substr(offset)
                                  : text.substr(offset, line_end - offset);
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      offset = line_end == std::string_view::npos ? text.size() + 1 : line_end + 1;

      line = trim(line);
      if (line.empty() || line.front() == '#')
        continue;

      const auto next_token = [&](std::string_view input, usize& pos) -> std::string_view {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
          ++pos;
        const usize start = pos;
        while (pos < input.size() && !std::isspace(static_cast<unsigned char>(input[pos])))
          ++pos;
        return input.substr(start, pos - start);
      };

      usize pos = 0;
      const std::string_view sha1 = next_token(line, pos);
      const std::string_view filename = next_token(line, pos);
      const std::string_view length_text = next_token(line, pos);
      while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
        ++pos;
      if (sha1.empty() || filename.empty() || length_text.empty() || pos != line.size()) {
        error_out = "squirrel feed line must have sha1 filename length";
        return std::nullopt;
      }
      if (!is_hex_sha1(sha1)) {
        error_out = "squirrel feed sha1 is invalid";
        return std::nullopt;
      }
      const auto length = parse_i64_decimal(length_text);
      if (!length) {
        error_out = "squirrel feed length is invalid";
        return std::nullopt;
      }
      std::string version;
      if (!extract_last_dotted_version(filename, version)) {
        error_out = "squirrel feed filename missing version";
        return std::nullopt;
      }

      update_manifest_v2::artifact artifact;
      artifact.kind = filename.ends_with("-delta.nupkg") ? "bsdiff" : "full";
      artifact.url = std::string(filename);
      artifact.size = *length;
      artifact.code_signature = "sha1=" + ascii_lower(std::string(sha1));
      out.artifacts.push_back(std::move(artifact));

      if (out.artifacts.back().kind == "full") {
        if (!saw_full || compare_versions(best_full_version, version) < 0)
          best_full_version = version;
        saw_full = true;
      }
    }

    if (!saw_full) {
      error_out = "squirrel feed has no full release";
      return std::nullopt;
    }
    out.version = best_full_version;
    return out;
  }

  std::optional<update_manifest_v2> parse_manifest_v2_cbor(const std::vector<uint8_t>& bytes,
                                                           std::string& error_out) {
    const auto decoded = cbor::decode(bytes.data(), bytes.size());
    if (!decoded) {
      error_out = "manifest v2 CBOR decode failed";
      return std::nullopt;
    }
    const auto* root = cbor_map(*decoded);
    if (!root) {
      error_out = "manifest v2 root must be a map";
      return std::nullopt;
    }

    update_manifest_v2 out;
    cbor::map signed_map = *root;
    if (const auto it = root->find("signature"); it != root->end()) {
      const auto* sig = cbor_bytes(it->second);
      if (!sig) {
        error_out = "manifest v2 signature must be bytes";
        return std::nullopt;
      }
      out.signature = *sig;
      signed_map.erase("signature");
    }
    out.canonical_bytes = cbor::encode(cbor::value(std::move(signed_map)));

    if (const auto it = root->find("version"); it != root->end()) {
      const auto* text = cbor_text(it->second);
      if (!text || text->empty()) {
        error_out = "manifest v2 version must be text";
        return std::nullopt;
      }
      out.version = *text;
    }
    if (out.version.empty()) {
      error_out = "manifest v2 version is required";
      return std::nullopt;
    }
    if (const auto it = root->find("channel"); it != root->end()) {
      const auto* text = cbor_text(it->second);
      if (!text) {
        error_out = "manifest v2 channel must be text";
        return std::nullopt;
      }
      const auto channel = updater::parse_channel(*text);
      if (!channel) {
        error_out = "manifest v2 channel is invalid";
        return std::nullopt;
      }
      out.channel = *channel;
    }
    if (const auto it = root->find("rollout_percent"); it != root->end()) {
      const auto percent = cbor_uint(it->second);
      if (!percent || *percent > 100) {
        error_out = "manifest v2 rollout_percent must be 0..100";
        return std::nullopt;
      }
      out.rollout_percent = static_cast<uint32_t>(*percent);
    }
    if (const auto it = root->find("platform"); it != root->end()) {
      const auto* text = cbor_text(it->second);
      if (!text) {
        error_out = "manifest v2 platform must be text";
        return std::nullopt;
      }
      out.platform = *text;
    }
    if (const auto it = root->find("arch"); it != root->end()) {
      const auto* text = cbor_text(it->second);
      if (!text) {
        error_out = "manifest v2 arch must be text";
        return std::nullopt;
      }
      out.arch = *text;
    }
    const auto artifacts_it = root->find("artifacts");
    if (artifacts_it == root->end()) {
      error_out = "manifest v2 artifacts are required";
      return std::nullopt;
    }
    const auto* artifacts = cbor_array(artifacts_it->second);
    if (!artifacts) {
      error_out = "manifest v2 artifacts must be an array";
      return std::nullopt;
    }
    out.artifacts.reserve(artifacts->size());
    for (const auto& entry : *artifacts) {
      const auto* artifact_map = cbor_map(entry);
      if (!artifact_map) {
        error_out = "manifest v2 artifact must be a map";
        return std::nullopt;
      }
      update_manifest_v2::artifact artifact;
      auto load_text = [&](std::string_view key, std::string& field, bool required) -> bool {
        const auto it = artifact_map->find(std::string(key));
        if (it == artifact_map->end()) {
          if (required) {
            error_out = "manifest v2 artifact missing " + std::string(key);
            return false;
          }
          return true;
        }
        const auto* text = cbor_text(it->second);
        if (!text) {
          error_out = "manifest v2 artifact field " + std::string(key) + " must be text";
          return false;
        }
        field = *text;
        return true;
      };
      if (!load_text("kind", artifact.kind, true) || !load_text("url", artifact.url, true) ||
          !load_text("sha256", artifact.sha256, true) ||
          !load_text("from_version", artifact.from_version, false) ||
          !load_text("target_sha256", artifact.target_sha256, false) ||
          !load_text("code_signature", artifact.code_signature, false)) {
        return std::nullopt;
      }
      if (!is_hex_sha256(artifact.sha256)) {
        error_out = "manifest v2 artifact sha256 is invalid";
        return std::nullopt;
      }
      if (const auto it = artifact_map->find("size"); it != artifact_map->end()) {
        const auto size = cbor_uint(it->second);
        if (!size || *size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          error_out = "manifest v2 artifact size is invalid";
          return std::nullopt;
        }
        artifact.size = static_cast<int64_t>(*size);
      }
      if (artifact.kind != "full" && artifact.kind != "bsdiff") {
        error_out = "manifest v2 artifact kind is invalid";
        return std::nullopt;
      }
      if (artifact.kind == "bsdiff") {
        if (artifact.from_version.empty() || !is_hex_sha256(artifact.target_sha256)) {
          error_out = "manifest v2 bsdiff artifact fields are invalid";
          return std::nullopt;
        }
      }
      out.artifacts.push_back(std::move(artifact));
    }
    return out;
  }

  bool apply_bsdiff_delta(const std::filesystem::path& patch_path,
                          const std::filesystem::path& current_path,
                          const std::filesystem::path& staged_path,
                          std::string_view expected_target_sha256, std::string& error_out) {
    if (!is_hex_sha256(expected_target_sha256)) {
      error_out = "expected bsdiff target sha256 is invalid";
      return false;
    }
    std::vector<u8> patch_bytes;
    if (!read_all(patch_path, patch_bytes, error_out))
      return false;
    std::vector<u8> current_bytes;
    if (!read_all(current_path, current_bytes, error_out))
      return false;
    std::vector<u8> staged_bytes;
    if (!apply_bsdiff(current_bytes, patch_bytes, staged_bytes, error_out))
      return false;
    if (sha256_hex(staged_bytes) != ascii_lower(std::string(expected_target_sha256))) {
      error_out = "bsdiff reconstructed target sha256 mismatch";
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(staged_path.parent_path(), ec);
    if (ec) {
      error_out = "cannot create staged delta directory: " + ec.message();
      return false;
    }
    return write_all(staged_path, staged_bytes, error_out);
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
    std::array<u8, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), d.artifact.data(), d.artifact.size());
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string actual_hex;
    actual_hex.reserve(digest.size() * 2);
    for (u8 byte : digest) {
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
    if (!detail::verify_platform_code_signature(partial_path, d.expected_signing_authority,
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

  void detail::set_platform_swap_destination_override_for_tests(
      const std::optional<std::filesystem::path>& path) {
    platform_swap_destination_override() = path;
  }

  const std::vector<std::string>& detail::last_platform_swap_argv() {
    return last_platform_swap_argv_storage();
  }

  bool updater::perform_platform_swap(const std::string& staged_path, std::string& error_out,
                                      bool dry_run) {
    error_out.clear();
    auto& argv_out = last_platform_swap_argv_storage();
    argv_out.clear();
#ifdef __APPLE__
    const std::filesystem::path staged(staged_path);
    if (!path_looks_like_app_bundle(staged))
      return true;
    auto destination = current_bundle_path();
    if (!destination) {
      error_out = "failed to resolve running app bundle destination for staged swap";
      return false;
    }
    std::error_code ec;
    const std::filesystem::path staged_resolved = std::filesystem::weakly_canonical(staged, ec);
    const std::filesystem::path staged_final = ec ? staged : staged_resolved;
    ec.clear();
    const std::filesystem::path dest_resolved = std::filesystem::weakly_canonical(*destination, ec);
    const std::filesystem::path dest_final = ec ? *destination : dest_resolved;
    const std::string script = std::format("while kill -0 {} 2>/dev/null; do /bin/sleep 0.1; done; "
                                           "/bin/mv -f {} {} && /usr/bin/open {}",
                                           static_cast<i64>(::getpid()), shell_quote(staged_final),
                                           shell_quote(dest_final), shell_quote(dest_final));
    argv_out = {"/bin/sh", "-c", script};
    if (dry_run)
      return true;
    std::array<char*, 4> argv = {
        argv_out[0].data(),
        argv_out[1].data(),
        argv_out[2].data(),
        nullptr,
    };
    posix_spawnattr_t attr;
    int rv = posix_spawnattr_init(&attr);
    if (rv != 0) {
      error_out = "posix_spawnattr_init failed: " + std::system_category().message(rv);
      return false;
    }
#ifdef POSIX_SPAWN_SETSID
    rv = posix_spawnattr_setflags(&attr, static_cast<short>(POSIX_SPAWN_SETSID));
    if (rv != 0) {
      posix_spawnattr_destroy(&attr);
      error_out = "posix_spawnattr_setflags failed: " + std::system_category().message(rv);
      return false;
    }
#endif
    char* const envp[] = {nullptr};
    [[maybe_unused]] pid_t child_pid = 0;
    rv = posix_spawn(&child_pid, "/bin/sh", nullptr, &attr, argv.data(), envp);
    posix_spawnattr_destroy(&attr);
    if (rv != 0) {
      error_out = "posix_spawn failed: " + std::system_category().message(rv);
      return false;
    }
    return true;
#else
    (void)staged_path;
    (void)dry_run;
    // Non-macOS still uses the legacy in-process marker transition; platform swap stays unchanged.
    return true;
#endif
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
        const std::filesystem::path staged_payload = read_first_line(marker);
        const auto pending = entry.path() / ".pending";
        std::filesystem::rename(marker, pending, ec);
        if (ec) {
          error_out = "failed to mark staged update pending: " + ec.message();
          return false;
        }
        const std::string version = entry.path().filename().string();
        if (!record_installed_version(root, version, error_out))
          return false;
        if (!write_pending_first_launch_flag(root, version, error_out))
          return false;
#ifdef __APPLE__
        if (path_looks_like_app_bundle(staged_payload) &&
            staged_payload.parent_path() == entry.path() &&
            !perform_platform_swap(staged_payload.string(), error_out))
          return false;
#else
        // Windows/Linux keep the legacy marker-only path until their swap helpers land.
        (void)staged_payload;
#endif
        return true;
      }
    }
    return true;
  }

  bool updater::mark_ready() {
    const auto flag = pending_first_launch_flag_path(updates_root());
    std::error_code ec;
    const bool exists = std::filesystem::exists(flag, ec);
    if (ec || !exists)
      return false;
    const bool removed = std::filesystem::remove(flag, ec);
    if (ec)
      return false;
    return removed;
  }

  bool updater::has_pending_first_launch() {
    std::error_code ec;
    const bool exists = std::filesystem::exists(pending_first_launch_flag_path(updates_root()), ec);
    return !ec && exists;
  }

  bool updater::auto_rollback_if_unready(std::string& rolled_from_out, std::string& error_out) {
    rolled_from_out.clear();
    error_out.clear();
    const auto root = updates_root();
    const auto flag = pending_first_launch_flag_path(root);
    std::error_code ec;
    const bool has_flag = std::filesystem::exists(flag, ec);
    if (ec) {
      error_out = "failed to inspect first-launch marker: " + ec.message();
      return false;
    }
    if (!has_flag)
      return true;

    rolled_from_out = read_first_line(flag);
    if (rolled_from_out.empty()) {
      auto versions = read_history_file(root);
      if (!versions.empty())
        rolled_from_out = versions.front();
    }

    if (!rollback(error_out))
      return false;

    (void)mark_ready();
    if (rolled_from_out.empty()) {
      auto versions = read_history_file(root);
      if (versions.size() > 1)
        rolled_from_out = versions[1];
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
    for (usize i = 2; i < versions.size() && next.size() < 3; ++i) {
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
    usize pos = 0;
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
