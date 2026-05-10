#include "../src/runtime/bundle_loader.hpp"
#include "../src/runtime/fxa_archive.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using fxe::runtime::fxa_archive::Bundle;
  using fxe::runtime::fxa_archive::ManifestMetadata;
  using fxe::runtime::fxa_archive::PackOptions;

  constexpr std::string_view kSecretKeyB64 =
      "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8DoQe/884Qvh1w3RjnS8CZZ+TWMJulDV8d3IZkElUxuA==";
  constexpr std::string_view kPublicKeyB64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  constexpr uint64_t kTrailerSize = 144;

  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
      return;
    }
    ++g_fail;
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void mutate_byte(const fs::path& path, uint64_t offset, unsigned char mask) {
    std::string body = read_text(path);
    body.at(static_cast<size_t>(offset)) =
        static_cast<char>(static_cast<unsigned char>(body.at(static_cast<size_t>(offset))) ^ mask);
    write_text(path, body);
  }

  std::optional<uint64_t> find_bytes(std::string_view haystack, std::string_view needle) {
    const size_t pos = haystack.find(needle);
    if (pos == std::string_view::npos)
      return std::nullopt;
    return static_cast<uint64_t>(pos);
  }

  struct TrailerFields {
    uint64_t manifest_offset = 0;
    uint64_t manifest_size = 0;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
  };

  uint32_t read_u32_le(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
  }

  uint64_t read_u64_le(const char* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
      value |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (i * 8);
    return value;
  }

  TrailerFields read_trailer(const fs::path& path) {
    const std::string body = read_text(path);
    const size_t base = body.size() - static_cast<size_t>(kTrailerSize);
    CHECK(read_u32_le(body.data() + base + 8) == 1u);
    return TrailerFields{
        .manifest_offset = read_u64_le(body.data() + base + 16),
        .manifest_size = read_u64_le(body.data() + base + 24),
        .payload_offset = read_u64_le(body.data() + base + 32),
        .payload_size = read_u64_le(body.data() + base + 40),
    };
  }

  std::string shell_quote(std::string_view text) {
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : text) {
      if (ch == '\"')
        out += "\\\"";
      else
        out.push_back(ch);
    }
    out += '\"';
    return out;
#else
    std::string out = "'";
    for (char ch : text) {
      if (ch == '\'')
        out += "'\\''";
      else
        out.push_back(ch);
    }
    out += '\'';
    return out;
#endif
  }

  void set_allow_unsigned_env(bool enabled) {
#ifdef _WIN32
    _putenv_s("FXE_BUNDLE_ALLOW_UNSIGNED", enabled ? "1" : "");
#else
    if (enabled)
      setenv("FXE_BUNDLE_ALLOW_UNSIGNED", "1", 1);
    else
      unsetenv("FXE_BUNDLE_ALLOW_UNSIGNED");
#endif
  }

  int run_mount_case(const char* self, const fs::path& archive, bool allow_unsigned,
                     bool expect_mount, bool expect_verified) {
    set_allow_unsigned_env(allow_unsigned);
    std::string cmd = shell_quote(self);
    cmd += " --mount-case ";
    cmd += shell_quote(archive.string());
    cmd += expect_mount ? " 1" : " 0";
    cmd += expect_verified ? " 1" : " 0";
    const int rc = std::system(cmd.c_str());
    set_allow_unsigned_env(false);
    return rc;
  }

  fs::path make_signed_archive(const fs::path& dir, std::string_view stem, bool compress) {
    fs::create_directories(dir);
    const fs::path host = dir / (std::string(stem) + ".bin");
    write_text(host, "HOST");
    const fs::path main_js = dir / (std::string(stem) + "-main.js");
    const fs::path data_txt = dir / (std::string(stem) + "-data.txt");
    const fs::path entry_txt = dir / (std::string(stem) + "-entry.txt");
    write_text(main_js, "console.log('ok');\n");
    write_text(data_txt, "asset payload\n");
    write_text(entry_txt, "main.js");

    ManifestMetadata manifest;
    manifest.app_name = "demo-app";
    manifest.version = "1.2.3";
    manifest.entry = "main.js";
    manifest.created_at = "2026-05-10T04:20:06Z";
    manifest.public_key = std::string(kPublicKeyB64);
    manifest.signer_public_key_b64 = std::string(kPublicKeyB64);
    manifest.signer_secret_key_b64 = std::string(kSecretKeyB64);
    manifest.channel = "stable";
    manifest.update_url = "https://example.test/update";

    PackOptions opts;
    opts.sign = true;
    opts.secret_key_b64 = std::string(kSecretKeyB64);
    opts.public_key_b64 = std::string(kPublicKeyB64);
    opts.compress = compress;

    std::string error;
    const bool packed =
        fxe::runtime::fxa_archive::pack_files(host.string(),
                                              {{main_js.string(), "main.js"},
                                               {data_txt.string(), "assets/data.txt"},
                                               {entry_txt.string(), "__entry__"}},
                                              manifest, opts, &error);
    CHECK(packed);
    CHECK(error.empty());
    return host;
  }

  void test_roundtrip(const char* self, const fs::path& tmp) {
    const fs::path archive = make_signed_archive(tmp / "roundtrip", "signed", true);
    Bundle bundle(archive.string());
    CHECK(bundle.valid());
    CHECK(bundle.signed_archive());
    CHECK(bundle.signature_verified());
    CHECK(bundle.signer_pubkey().size() == 32u);
    CHECK(bundle.payload_sha256().size() == 64u);
    CHECK(bundle.read("main.js") == std::optional<std::string>("console.log('ok');\n"));
    CHECK(bundle.read("assets/data.txt") == std::optional<std::string>("asset payload\n"));
    CHECK(bundle.read("__entry__") == std::optional<std::string>("main.js"));
    const auto names = bundle.list();
    CHECK(names.size() == 3u);
    CHECK(std::find(names.begin(), names.end(), "main.js") != names.end());
    CHECK(std::find(names.begin(), names.end(), "assets/data.txt") != names.end());
    CHECK(std::find(names.begin(), names.end(), "__entry__") != names.end());
    CHECK(run_mount_case(self, archive, false, true, true) == 0);
  }

  fs::path copy_archive(const fs::path& tmp, const fs::path& src, std::string_view stem) {
    const fs::path dst = tmp / (std::string(stem) + ".bin");
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    return dst;
  }

  void test_signature_tamper(const char* self, const fs::path& tmp) {
    const fs::path archive = make_signed_archive(tmp / "tamper", "signed", false);
    const TrailerFields trailer = read_trailer(archive);
    const std::string body = read_text(archive);

    const fs::path payload_bad = copy_archive(tmp, archive, "payload-bad");
    const auto payload_rel =
        find_bytes(std::string_view(body).substr(static_cast<size_t>(trailer.payload_offset),
                                                 static_cast<size_t>(trailer.payload_size)),
                   "asset payload\n");
    CHECK(payload_rel.has_value());
    if (payload_rel)
      mutate_byte(payload_bad, trailer.payload_offset + *payload_rel + 1, 0x20);

    const fs::path manifest_bad = copy_archive(tmp, archive, "manifest-bad");
    const auto manifest_rel =
        find_bytes(std::string_view(body).substr(static_cast<size_t>(trailer.manifest_offset),
                                                 static_cast<size_t>(trailer.manifest_size)),
                   "demo-app");
    CHECK(manifest_rel.has_value());
    if (manifest_rel)
      mutate_byte(manifest_bad, trailer.manifest_offset + *manifest_rel + 1, 0x01);

    const fs::path header_bad = copy_archive(tmp, archive, "header-bad");
    mutate_byte(header_bad, static_cast<uint64_t>(body.size()) - kTrailerSize + 12u, 0x80);

    Bundle payload_bundle(payload_bad.string());
    Bundle manifest_bundle(manifest_bad.string());
    Bundle header_bundle(header_bad.string());
    CHECK(payload_bundle.valid());
    CHECK(manifest_bundle.valid());
    CHECK(header_bundle.valid());
    CHECK(!payload_bundle.signature_verified());
    CHECK(!manifest_bundle.signature_verified());
    CHECK(!header_bundle.signature_verified());

    CHECK(run_mount_case(self, payload_bad, false, false, false) == 0);
    CHECK(run_mount_case(self, payload_bad, true, true, false) == 0);
    CHECK(run_mount_case(self, manifest_bad, false, false, false) == 0);
    CHECK(run_mount_case(self, manifest_bad, true, true, false) == 0);
    CHECK(run_mount_case(self, header_bad, false, false, false) == 0);
    CHECK(run_mount_case(self, header_bad, true, true, false) == 0);
  }

  void test_trailer_magic_mismatch(const fs::path& tmp) {
    const fs::path archive = make_signed_archive(tmp / "magic", "signed", false);
    const fs::path broken = copy_archive(tmp, archive, "magic-bad");
    const std::string body = read_text(broken);
    mutate_byte(broken, static_cast<uint64_t>(body.size()) - kTrailerSize, 0xff);
    Bundle bundle(broken.string());
    CHECK(!bundle.valid());
  }

  int run_mount_case_child(const char* archive_path, const char* expect_mount_arg,
                           const char* expect_verified_arg) {
    const bool expect_mount = std::string_view(expect_mount_arg) == "1";
    const bool expect_verified = std::string_view(expect_verified_arg) == "1";
    const bool mounted = fxe::runtime::mount_bundle_from_argv0(archive_path);
    if (mounted != expect_mount)
      return 1;
    if (fxe::runtime::bundle_mounted() != expect_mount)
      return 1;
    if (fxe::runtime::bundle_signature_verified() != expect_verified)
      return 1;
    if (!expect_mount)
      return 0;
    const auto main = fxe::runtime::read_virtual("./main.js");
    const auto stripped = fxe::runtime::read_virtual("./assets/data.txt");
    const auto fallback = fxe::runtime::read_virtual("/virtual/main.js");
    if (!main || !stripped || !fallback)
      return 1;
    if (expect_verified) {
      if (*main != "console.log('ok');\n" || *stripped != "asset payload\n" ||
          *fallback != "console.log('ok');\n") {
        return 1;
      }
    }
    if (fxe::runtime::bundle_entry() != "main.js")
      return 1;
    return 0;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string_view(argv[1]) == "--mount-case")
    return run_mount_case_child(argv[2], argv[3], argv[4]);

  const fs::path tmp = fs::temp_directory_path() / "fxe-fxa-archive-test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  test_roundtrip(argv[0], tmp);
  test_signature_tamper(argv[0], tmp);
  test_trailer_magic_mismatch(tmp);

  fs::remove_all(tmp);
  std::fprintf(stderr, "fxa_archive_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
