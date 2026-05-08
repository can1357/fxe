#include "runtime/cbor.hpp"
#include "runtime/updater.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sodium.h>
#include <string>
#include <vector>

namespace {
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  void append_u64_le(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }

  void append_i64_le(std::vector<uint8_t>& out, int64_t value) {
    append_u64_le(out, static_cast<uint64_t>(value));
  }

  std::vector<uint8_t> make_patch(std::string_view extra_text) {
    std::vector<uint8_t> control;
    append_i64_le(control, 3);
    append_i64_le(control, static_cast<int64_t>(extra_text.size()));
    append_i64_le(control, 0);
    append_i64_le(control, 3);
    append_i64_le(control, 0);
    append_i64_le(control, 0);

    std::vector<uint8_t> diff(6, 0);
    std::vector<uint8_t> extra(extra_text.begin(), extra_text.end());
    std::vector<uint8_t> patch{'F', 'X', 'E', 'B', 'S', 'D', '\0', '\0'};
    append_u64_le(patch, control.size());
    append_u64_le(patch, diff.size());
    append_u64_le(patch, extra.size());
    patch.insert(patch.end(), control.begin(), control.end());
    patch.insert(patch.end(), diff.begin(), diff.end());
    patch.insert(patch.end(), extra.begin(), extra.end());
    return patch;
  }

  std::string hex_sha256(std::string_view bytes) {
    std::vector<u8> data(bytes.begin(), bytes.end());
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), data.data(), data.size());
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
      out.push_back(k_hex[byte >> 4]);
      out.push_back(k_hex[byte & 0x0f]);
    }
    return out;
  }
} // namespace

int main() {
  using fxe::runtime::cbor::value;

  const std::vector<uint8_t> manifest_bytes = {
      0xa6, 0x64, 'a',  'r',  'c',  'h',  0x65, 'a',  'r',  'm',  '6',  '4', 0x67, 'c', 'h', 'a',
      'n',  'n',  'e',  'l',  0x64, 'b',  'e',  't',  'a',  0x67, 'v',  'e', 'r',  's', 'i', 'o',
      'n',  0x65, '1',  '.',  '2',  '.',  '3',  0x69, 'a',  'r',  't',  'i', 'f',  'a', 'c', 't',
      's',  0x81, 0xa6, 0x64, 'k',  'i',  'n',  'd',  0x66, 'b',  's',  'd', 'i',  'f', 'f', 0x63,
      'u',  'r',  'l',  0x78, 0x1e, 'h',  't',  't',  'p',  's',  ':',  '/', '/',  'e', 'x', 'a',
      'm',  'p',  'l',  'e',  '.',  'c',  'o',  'm',  '/',  'a',  'p',  'p', '.',  'b', 's', 'd',
      'i',  'f',  'f',  0x64, 's',  'i',  'z',  'e',  0x18, 0x29, 0x66, 's', 'h',  'a', '2', '5',
      '6',  0x78, 0x40, '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8', '9',  'a', 'b', 'c',
      'd',  'e',  'f',  '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8', '9',  'a', 'b', 'c',
      'd',  'e',  'f',  '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8', '9',  'a', 'b', 'c',
      'd',  'e',  'f',  '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8', '9',  'a', 'b', 'c',
      'd',  'e',  'f',  0x6c, 'f',  'r',  'o',  'm',  '_',  'v',  'e',  'r', 's',  'i', 'o', 'n',
      0x65, '1',  '.',  '2',  '.',  '2',  0x6d, 't',  'a',  'r',  'g',  'e', 't',  '_', 's', 'h',
      'a',  '2',  '5',  '6',  0x78, 0x40, 'f',  'e',  'd',  'c',  'b',  'a', '9',  '8', '7', '6',
      '5',  '4',  '3',  '2',  '1',  '0',  'f',  'e',  'd',  'c',  'b',  'a', '9',  '8', '7', '6',
      '5',  '4',  '3',  '2',  '1',  '0',  'f',  'e',  'd',  'c',  'b',  'a', '9',  '8', '7', '6',
      '5',  '4',  '3',  '2',  '1',  '0',  'f',  'e',  'd',  'c',  'b',  'a', '9',  '8', '7', '6',
      '5',  '4',  '3',  '2',  '1',  '0',  0x68, 'p',  'l',  'a',  't',  'f', 'o',  'r', 'm', 0x66,
      'd',  'a',  'r',  'w',  'i',  'n',  0x69, 's',  'i',  'g',  'n',  'a', 't',  'u', 'r', 'e',
      0x43, 0xaa, 0xbb, 0xcc,
  };
  std::string error;
  auto manifest = fxe::runtime::parse_manifest_v2_cbor(manifest_bytes, error);
  CHECK(manifest.has_value());
  CHECK(error.empty());
  if (manifest) {
    CHECK(manifest->version == "1.2.3");
    CHECK(manifest->channel == fxe::runtime::update_channel::beta);
    CHECK(manifest->platform == "darwin");
    CHECK(manifest->arch == "arm64");
    CHECK(manifest->artifacts.size() == 1);
    if (manifest->artifacts.size() == 1) {
      CHECK(manifest->artifacts[0].kind == "bsdiff");
      CHECK(manifest->artifacts[0].from_version == "1.2.2");
    }
    CHECK(manifest->signature == std::vector<uint8_t>({0xaa, 0xbb, 0xcc}));
  }

  const auto expected_canonical = fxe::runtime::cbor::encode(value(fxe::runtime::cbor::map{
      {"arch", value(std::string("arm64"))},
      {"channel", value(std::string("beta"))},
      {"version", value(std::string("1.2.3"))},
      {"artifacts",
       value(fxe::runtime::cbor::array{value(fxe::runtime::cbor::map{
           {"kind", value(std::string("bsdiff"))},
           {"url", value(std::string("https://example.com/app.bsdiff"))},
           {"size", value(uint64_t(41))},
           {"sha256",
            value(std::string("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"))},
           {"from_version", value(std::string("1.2.2"))},
           {"target_sha256",
            value(std::string("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"))},
       })})},
      {"platform", value(std::string("darwin"))},
  }));
  CHECK(manifest && manifest->canonical_bytes == expected_canonical);

  const std::vector<uint8_t> old_bytes{'a', 'b', 'c', 'd', 'e', 'f'};
  const auto patch = make_patch("XYZ");
  std::vector<uint8_t> reconstructed;
  error.clear();
  CHECK(fxe::runtime::apply_bsdiff(old_bytes, patch, reconstructed, error));
  CHECK(error.empty());
  CHECK(std::string(reconstructed.begin(), reconstructed.end()) == "abcXYZdef");

  const auto tmp = std::filesystem::temp_directory_path() / "fxe-update-manifest-v2-test";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  {
    std::ofstream old_file(tmp / "current.bin", std::ios::binary);
    old_file.write("abcdef", 6);
  }
  {
    std::ofstream patch_file(tmp / "patch.bsdiff", std::ios::binary);
    patch_file.write(reinterpret_cast<const char*>(patch.data()),
                     static_cast<std::streamsize>(patch.size()));
  }
  error.clear();
  CHECK(fxe::runtime::apply_bsdiff_delta(tmp / "patch.bsdiff", tmp / "current.bin",
                                         tmp / "staged.bin", hex_sha256("abcXYZdef"), error));
  CHECK(error.empty());
  std::ifstream staged_file(tmp / "staged.bin", std::ios::binary);
  std::string staged((std::istreambuf_iterator<char>(staged_file)),
                     std::istreambuf_iterator<char>());
  CHECK(staged == "abcXYZdef");
  std::filesystem::remove_all(tmp);

  return g_fail == 0 ? 0 : 1;
}
