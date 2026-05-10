#include "fxa_archive.hpp"

#include "cbor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <optional>
#include <sodium.h>
#include <span>

#if FXE_HAS_ZSTD
#include <zstd.h>
#endif

namespace fxe::runtime::fxa_archive {
  namespace {

    inline constexpr char k_magic[8] = {'F', 'X', 'E', 'A', 'X', 'A', '\0', '\0'};
    inline constexpr u32 k_version = 1;
    inline constexpr u32 k_signed_flag = 1;
    inline constexpr usize k_trailer_size = 144;

    void ensure_sodium_initialized() {
      static std::once_flag flag;
      std::call_once(flag, []() {
        if (sodium_init() < 0)
          std::abort();
      });
    }

    void put_u32(std::string& out, u32 v) {
      char b[4];
      b[0] = static_cast<char>(v & 0xff);
      b[1] = static_cast<char>((v >> 8) & 0xff);
      b[2] = static_cast<char>((v >> 16) & 0xff);
      b[3] = static_cast<char>((v >> 24) & 0xff);
      out.append(b, 4);
    }

    void put_u64(std::string& out, u64 v) {
      for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    }

    u32 read_u32(const char* p) {
      return (static_cast<u32>(static_cast<unsigned char>(p[0]))) |
             (static_cast<u32>(static_cast<unsigned char>(p[1])) << 8) |
             (static_cast<u32>(static_cast<unsigned char>(p[2])) << 16) |
             (static_cast<u32>(static_cast<unsigned char>(p[3])) << 24);
    }

    u64 read_u64(const char* p) {
      u64 v = 0;
      for (int i = 0; i < 8; ++i)
        v |= static_cast<u64>(static_cast<unsigned char>(p[i])) << (i * 8);
      return v;
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

    std::optional<std::vector<u8>> base64_decode(std::string_view input) {
      ensure_sodium_initialized();
      std::vector<u8> out(input.size());
      usize out_len = 0;
      const char* end = nullptr;
      if (sodium_base642bin(out.data(), out.size(), input.data(), input.size(), " \t\n\r", &out_len,
                            &end, sodium_base64_VARIANT_ORIGINAL) != 0) {
        return std::nullopt;
      }
      const char* input_end = input.data() + input.size();
      while (end != input_end) {
        const char c = *end++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
          return std::nullopt;
      }
      out.resize(out_len);
      return out;
    }

    bool read_all_bytes(const std::string& path, std::vector<u8>& out, std::string* error) {
      std::ifstream f(path, std::ios::binary);
      if (!f) {
        if (error)
          *error = "cannot open " + path;
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    std::string make_signed_prefix(u32 flags, u64 manifest_offset, u64 manifest_size,
                                   u64 payload_offset, u64 payload_size) {
      std::string out;
      out.reserve(48);
      out.append(k_magic, sizeof(k_magic));
      put_u32(out, k_version);
      put_u32(out, flags);
      put_u64(out, manifest_offset);
      put_u64(out, manifest_size);
      put_u64(out, payload_offset);
      put_u64(out, payload_size);
      return out;
    }

    bool compress_payload(std::span<const u8> raw_payload, bool enabled, std::vector<u8>& out,
                          std::string& compression, std::string* error) {
      if (!enabled || raw_payload.empty()) {
        out.assign(raw_payload.begin(), raw_payload.end());
        compression = "none";
        return true;
      }
#if FXE_HAS_ZSTD
      const usize bound = ZSTD_compressBound(raw_payload.size());
      out.resize(bound);
      const usize written = ZSTD_compress(out.data(), out.size(), raw_payload.data(),
                                          raw_payload.size(), ZSTD_CLEVEL_DEFAULT);
      if (ZSTD_isError(written) != 0) {
        if (error)
          *error = std::string("zstd compression failed: ") + ZSTD_getErrorName(written);
        return false;
      }
      out.resize(written);
      compression = "zstd";
      return true;
#else
      // TODO(C4): zstd compression when zstd is available in this build.
      out.assign(raw_payload.begin(), raw_payload.end());
      compression = "none";
      return true;
#endif
    }

    bool decompress_payload(std::span<const u8> payload, std::vector<u8>& out, std::string* error) {
#if FXE_HAS_ZSTD
      const unsigned long long size = ZSTD_getFrameContentSize(payload.data(), payload.size());
      if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
        if (error)
          *error = "zstd payload frame size unavailable";
        return false;
      }
      out.resize(static_cast<usize>(size));
      const usize written = ZSTD_decompress(out.data(), out.size(), payload.data(), payload.size());
      if (ZSTD_isError(written) != 0 || written != out.size()) {
        if (error)
          *error = std::string("zstd decompression failed: ") + ZSTD_getErrorName(written);
        return false;
      }
      return true;
#else
      (void)payload;
      (void)out;
      if (error)
        *error = "zstd payloads are not supported in this build";
      return false;
#endif
    }

    std::optional<std::string> cbor_text(const cbor::map& m, std::string_view key) {
      const auto it = m.find(std::string(key));
      if (it == m.end())
        return std::nullopt;
      const auto* text = std::get_if<std::string>(&static_cast<const cbor::storage&>(it->second));
      if (!text)
        return std::nullopt;
      return *text;
    }

    std::optional<u64> cbor_u64(const cbor::map& m, std::string_view key) {
      const auto it = m.find(std::string(key));
      if (it == m.end())
        return std::nullopt;
      const auto& storage = static_cast<const cbor::storage&>(it->second);
      if (const auto* value = std::get_if<uint64_t>(&storage))
        return *value;
      if (const auto* value = std::get_if<int64_t>(&storage); value && *value >= 0)
        return static_cast<u64>(*value);
      return std::nullopt;
    }

    const cbor::array* cbor_array_value(const cbor::map& m, std::string_view key) {
      const auto it = m.find(std::string(key));
      if (it == m.end())
        return nullptr;
      return std::get_if<cbor::array>(&static_cast<const cbor::storage&>(it->second));
    }

    const cbor::map* cbor_map_value(const cbor::value& value) {
      return std::get_if<cbor::map>(&static_cast<const cbor::storage&>(value));
    }

    struct ParsedManifest {
      std::string compression;
      std::string payload_sha256;
      std::string public_key;
      std::string signature_algorithm;
      std::vector<std::pair<std::string, Entry>> entries;
    };

    std::optional<ParsedManifest> parse_manifest(std::span<const u8> bytes, std::string* error) {
      const auto decoded = cbor::decode(bytes.data(), bytes.size());
      if (!decoded) {
        if (error)
          *error = "archive manifest CBOR decode failed";
        return std::nullopt;
      }
      const auto* root = cbor_map_value(*decoded);
      if (!root) {
        if (error)
          *error = "archive manifest root must be a map";
        return std::nullopt;
      }
      const auto magic = cbor_text(*root, "magic");
      const auto app_name = cbor_text(*root, "appName");
      const auto version = cbor_text(*root, "version");
      const auto entry = cbor_text(*root, "entry");
      const auto created_at = cbor_text(*root, "createdAt");
      const auto compression = cbor_text(*root, "compression");
      const auto payload_sha256 = cbor_text(*root, "payloadSha256");
      const auto* entries_value = cbor_array_value(*root, "entries");
      if (!magic || *magic != "fxa1" || !app_name || !version || !entry || !created_at ||
          !compression || !payload_sha256 || !entries_value) {
        if (error)
          *error = "archive manifest missing required keys";
        return std::nullopt;
      }
      if (*compression != "none" && *compression != "zstd") {
        if (error)
          *error = "archive manifest compression is invalid";
        return std::nullopt;
      }
      ParsedManifest out;
      out.compression = *compression;
      out.payload_sha256 = *payload_sha256;
      if (const auto text = cbor_text(*root, "publicKey"))
        out.public_key = *text;
      if (const auto text = cbor_text(*root, "signatureAlgorithm"))
        out.signature_algorithm = *text;
      out.entries.reserve(entries_value->size());
      for (const auto& item : *entries_value) {
        const auto* entry_map = cbor_map_value(item);
        if (!entry_map) {
          if (error)
            *error = "archive manifest entry must be a map";
          return std::nullopt;
        }
        const auto name = cbor_text(*entry_map, "name");
        const auto offset = cbor_u64(*entry_map, "offset");
        const auto size = cbor_u64(*entry_map, "size");
        if (!name || !offset || !size) {
          if (error)
            *error = "archive manifest entry missing required fields";
          return std::nullopt;
        }
        out.entries.emplace_back(*name, Entry{*offset, *size});
      }
      return out;
    }

    bool parse_signing_keys(const ManifestMetadata& meta, const PackOptions& opts,
                            std::array<u8, crypto_sign_SECRETKEYBYTES>& secret_key,
                            std::array<u8, crypto_sign_PUBLICKEYBYTES>& public_key,
                            std::string* error) {
      const std::string secret_b64 =
          !opts.secret_key_b64.empty() ? opts.secret_key_b64 : meta.signer_secret_key_b64;
      const std::string public_b64 =
          !opts.public_key_b64.empty()
              ? opts.public_key_b64
              : (!meta.signer_public_key_b64.empty() ? meta.signer_public_key_b64
                                                     : meta.public_key);
      auto secret = base64_decode(secret_b64);
      if (!secret || secret->size() != secret_key.size()) {
        if (error)
          *error = "invalid Ed25519 secret key";
        return false;
      }
      std::copy(secret->begin(), secret->end(), secret_key.begin());

      std::array<u8, crypto_sign_PUBLICKEYBYTES> derived_public{};
      if (crypto_sign_ed25519_sk_to_pk(derived_public.data(), secret_key.data()) != 0) {
        if (error)
          *error = "failed to derive Ed25519 public key";
        return false;
      }
      if (!public_b64.empty()) {
        auto provided = base64_decode(public_b64);
        if (!provided || provided->size() != public_key.size()) {
          if (error)
            *error = "invalid Ed25519 public key";
          return false;
        }
        std::copy(provided->begin(), provided->end(), public_key.begin());
        if (!std::equal(public_key.begin(), public_key.end(), derived_public.begin())) {
          if (error)
            *error = "Ed25519 secret/public key mismatch";
          return false;
        }
      } else {
        public_key = derived_public;
      }
      return true;
    }

  } // namespace

  bool pack_files(const std::string& binary_path,
                  const std::vector<std::pair<std::string, std::string>>& files,
                  const ManifestMetadata& meta, const PackOptions& opts, std::string* error) {
    ensure_sodium_initialized();

    std::vector<std::pair<std::string, Entry>> entries;
    entries.reserve(files.size());
    std::vector<u8> raw_payload;
    for (const auto& [disk_path, archive_name] : files) {
      std::vector<u8> body;
      if (!read_all_bytes(disk_path, body, error))
        return false;
      const u64 offset = raw_payload.size();
      raw_payload.insert(raw_payload.end(), body.begin(), body.end());
      entries.emplace_back(archive_name, Entry{offset, body.size()});
    }

    std::vector<u8> payload;
    std::string compression;
    if (!compress_payload(raw_payload, opts.compress, payload, compression, error))
      return false;

    std::string manifest_public_key = meta.public_key;
    if (manifest_public_key.empty()) {
      manifest_public_key =
          !opts.public_key_b64.empty() ? opts.public_key_b64 : meta.signer_public_key_b64;
    }

    cbor::array manifest_entries;
    manifest_entries.reserve(entries.size());
    for (const auto& [name, entry] : entries) {
      manifest_entries.push_back(
          cbor::value(cbor::map{{"name", cbor::value(name)},
                                {"offset", cbor::value(uint64_t(entry.offset))},
                                {"size", cbor::value(uint64_t(entry.size))}}));
    }

    cbor::map manifest_map{{"magic", cbor::value(std::string("fxa1"))},
                           {"appName", cbor::value(meta.app_name)},
                           {"version", cbor::value(meta.version)},
                           {"entry", cbor::value(meta.entry)},
                           {"createdAt", cbor::value(meta.created_at)},
                           {"compression", cbor::value(compression)},
                           {"payloadSha256", cbor::value(sha256_hex(payload))},
                           {"entries", cbor::value(std::move(manifest_entries))}};
    if (!meta.channel.empty())
      manifest_map["channel"] = cbor::value(meta.channel);
    if (!meta.update_url.empty())
      manifest_map["updateUrl"] = cbor::value(meta.update_url);
    if (!manifest_public_key.empty())
      manifest_map["publicKey"] = cbor::value(manifest_public_key);

    u32 flags = 0;
    std::array<u8, crypto_sign_BYTES> signature{};
    std::array<u8, crypto_sign_PUBLICKEYBYTES> signer_public_key{};

    if (opts.sign) {
      std::array<u8, crypto_sign_SECRETKEYBYTES> signer_secret_key{};
      if (!parse_signing_keys(meta, opts, signer_secret_key, signer_public_key, error))
        return false;
      flags |= k_signed_flag;
      manifest_map["signatureAlgorithm"] = cbor::value(std::string("ed25519"));
    }

    const std::vector<u8> manifest_bytes = cbor::encode(cbor::value(manifest_map));
    std::ifstream current(binary_path, std::ios::binary | std::ios::ate);
    if (!current) {
      if (error)
        *error = "cannot open " + binary_path;
      return false;
    }
    const u64 payload_offset = static_cast<u64>(current.tellg());
    const u64 payload_size = payload.size();
    const u64 manifest_offset = payload_offset + payload_size;
    const u64 manifest_size = manifest_bytes.size();

    if ((flags & k_signed_flag) != 0) {
      std::array<u8, crypto_sign_SECRETKEYBYTES> signer_secret_key{};
      if (!parse_signing_keys(meta, opts, signer_secret_key, signer_public_key, error))
        return false;
      std::string signed_message =
          make_signed_prefix(flags, manifest_offset, manifest_size, payload_offset, payload_size);
      signed_message.append(reinterpret_cast<const char*>(manifest_bytes.data()),
                            manifest_bytes.size());
      signed_message.append(reinterpret_cast<const char*>(payload.data()), payload.size());
      if (crypto_sign_detached(signature.data(), nullptr,
                               reinterpret_cast<const unsigned char*>(signed_message.data()),
                               signed_message.size(), signer_secret_key.data()) != 0) {
        if (error)
          *error = "Ed25519 signing failed";
        return false;
      }
    }

    std::ofstream out(binary_path, std::ios::binary | std::ios::app);
    if (!out) {
      if (error)
        *error = "cannot open " + binary_path + " for append";
      return false;
    }
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    out.write(reinterpret_cast<const char*>(manifest_bytes.data()),
              static_cast<std::streamsize>(manifest_bytes.size()));

    std::string trailer =
        make_signed_prefix(flags, manifest_offset, manifest_size, payload_offset, payload_size);
    trailer.append(reinterpret_cast<const char*>(signature.data()), signature.size());
    trailer.append(reinterpret_cast<const char*>(signer_public_key.data()),
                   signer_public_key.size());
    out.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    if (!out.good()) {
      if (error)
        *error = "write failed for archive trailer";
      return false;
    }
    return true;
  }

  Bundle::Bundle(std::string path) : path_(std::move(path)) {
    std::ifstream f(path_, std::ios::binary);
    if (!f)
      return;
    f.seekg(0, std::ios::end);
    const auto end = f.tellg();
    if (end < static_cast<std::streamoff>(k_trailer_size))
      return;
    f.seekg(end - static_cast<std::streamoff>(k_trailer_size));
    char trailer[k_trailer_size];
    f.read(trailer, k_trailer_size);
    if (!f)
      return;
    if (std::memcmp(trailer, k_magic, sizeof(k_magic)) != 0)
      return;
    const u32 version = read_u32(trailer + 8);
    if (version != k_version)
      return;

    const u32 flags = read_u32(trailer + 12);
    const u64 manifest_offset = read_u64(trailer + 16);
    const u64 manifest_size = read_u64(trailer + 24);
    payload_offset_ = read_u64(trailer + 32);
    const u64 payload_size = read_u64(trailer + 40);
    const auto file_size = static_cast<u64>(end);
    const u64 trailer_offset = file_size - k_trailer_size;

    if (manifest_offset > trailer_offset || manifest_size > trailer_offset ||
        payload_offset_ > manifest_offset || payload_offset_ + payload_size != manifest_offset ||
        manifest_offset + manifest_size != trailer_offset) {
      return;
    }

    std::vector<u8> payload(payload_size);
    std::vector<u8> manifest(manifest_size);
    f.seekg(static_cast<std::streamoff>(payload_offset_));
    f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    f.read(reinterpret_cast<char*>(manifest.data()), static_cast<std::streamsize>(manifest.size()));
    if (!f)
      return;

    std::string parse_error;
    auto parsed = parse_manifest(manifest, &parse_error);
    if (!parsed)
      return;
    compression_ = parsed->compression;
    payload_sha256_ = parsed->payload_sha256;

    signed_archive_ = (flags & k_signed_flag) != 0;
    const auto* signature = reinterpret_cast<const u8*>(trailer + 48);
    const auto* signer_pubkey = reinterpret_cast<const u8*>(trailer + 112);
    if (signed_archive_) {
      signer_pubkey_.assign(signer_pubkey, signer_pubkey + crypto_sign_PUBLICKEYBYTES);
      if (!parsed->signature_algorithm.empty()) {
        std::string algorithm = parsed->signature_algorithm;
        for (char& ch : algorithm)
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (algorithm != "ed25519")
          return;
      }
      if (!parsed->public_key.empty()) {
        auto public_key_b64 = base64_decode(parsed->public_key);
        if (!public_key_b64 || public_key_b64->size() != signer_pubkey_.size() ||
            !std::equal(public_key_b64->begin(), public_key_b64->end(), signer_pubkey_.begin())) {
          return;
        }
      }
      std::string signed_message =
          make_signed_prefix(flags, manifest_offset, manifest_size, payload_offset_, payload_size);
      signed_message.append(reinterpret_cast<const char*>(manifest.data()), manifest.size());
      signed_message.append(reinterpret_cast<const char*>(payload.data()), payload.size());
      signature_verified_ =
          crypto_sign_verify_detached(signature,
                                      reinterpret_cast<const unsigned char*>(signed_message.data()),
                                      signed_message.size(), signer_pubkey) == 0;
    }

    if (compression_ == "zstd") {
      if (!decompress_payload(payload, decompressed_payload_, &parse_error))
        return;
    } else if (compression_ != "none") {
      return;
    }

    const u64 logical_payload_size =
        compression_ == "zstd" ? decompressed_payload_.size() : payload.size();
    for (const auto& [name, entry] : parsed->entries) {
      if (entry.offset > logical_payload_size || entry.size > logical_payload_size - entry.offset)
        return;
      index_.emplace(name, entry);
      names_.push_back(name);
    }

    valid_ = true;
  }

  std::optional<std::string> Bundle::read(std::string_view name) const {
    if (!valid_)
      return std::nullopt;
    auto it = index_.find(std::string(name));
    if (it == index_.end())
      return std::nullopt;
    if (compression_ == "zstd") {
      const auto begin = static_cast<usize>(it->second.offset);
      const auto end = it->second.offset + it->second.size;
      return std::string(reinterpret_cast<const char*>(decompressed_payload_.data() + begin),
                         end - begin);
    }
    std::ifstream f(path_, std::ios::binary);
    if (!f)
      return std::nullopt;
    f.seekg(static_cast<std::streamoff>(payload_offset_ + it->second.offset));
    std::string body(it->second.size, '\0');
    f.read(body.data(), static_cast<std::streamsize>(it->second.size));
    if (!f)
      return std::nullopt;
    return body;
  }

  std::vector<std::string> Bundle::list() const {
    return names_;
  }

} // namespace fxe::runtime::fxa_archive
