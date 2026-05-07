#include "bundle.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>

namespace fxe::bundle {

  namespace {

    void put_u32(std::string& out, std::uint32_t v) {
      char b[4];
      b[0] = static_cast<char>(v & 0xff);
      b[1] = static_cast<char>((v >> 8) & 0xff);
      b[2] = static_cast<char>((v >> 16) & 0xff);
      b[3] = static_cast<char>((v >> 24) & 0xff);
      out.append(b, 4);
    }

    void put_u64(std::string& out, std::uint64_t v) {
      for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    }

    std::uint32_t read_u32(const char* p) {
      return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0]))) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
    }

    std::uint64_t read_u64(const char* p) {
      std::uint64_t v = 0;
      for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (i * 8);
      return v;
    }

    class Sha256 {
    public:
      void update(std::string_view data) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
        bit_len_ += static_cast<std::uint64_t>(data.size()) * 8;
        while (!data.empty()) {
          const std::size_t n = std::min<std::size_t>(data.size(), 64 - buffer_len_);
          std::memcpy(buffer_.data() + buffer_len_, p, n);
          buffer_len_ += n;
          p += n;
          data.remove_prefix(n);
          if (buffer_len_ == 64) {
            transform(buffer_.data());
            buffer_len_ = 0;
          }
        }
      }

      std::string hex_digest() {
        buffer_[buffer_len_++] = 0x80;
        if (buffer_len_ > 56) {
          while (buffer_len_ < 64)
            buffer_[buffer_len_++] = 0;
          transform(buffer_.data());
          buffer_len_ = 0;
        }
        while (buffer_len_ < 56)
          buffer_[buffer_len_++] = 0;
        for (int i = 7; i >= 0; --i) {
          buffer_[buffer_len_++] = static_cast<std::uint8_t>((bit_len_ >> (i * 8)) & 0xff);
        }
        transform(buffer_.data());

        static constexpr char k_hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t word : state_) {
          for (int i = 3; i >= 0; --i) {
            const std::uint8_t byte = static_cast<std::uint8_t>((word >> (i * 8)) & 0xff);
            out.push_back(k_hex[byte >> 4]);
            out.push_back(k_hex[byte & 0x0f]);
          }
        }
        return out;
      }

    private:
      static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
        return (x >> n) | (x << (32 - n));
      }

      void transform(const std::uint8_t* chunk) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
          w[i] = (static_cast<std::uint32_t>(chunk[i * 4]) << 24) |
                 (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16) |
                 (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8) |
                 static_cast<std::uint32_t>(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
          const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
          const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
          w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
          const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
          const std::uint32_t ch = (e & f) ^ (~e & g);
          const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
          const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
          const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
          const std::uint32_t temp2 = s0 + maj;
          h = g;
          g = f;
          f = e;
          e = d + temp1;
          d = c;
          c = b;
          b = a;
          a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
      }

      std::array<std::uint8_t, 64> buffer_{};
      std::size_t buffer_len_ = 0;
      std::uint64_t bit_len_ = 0;
      std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    };

    std::string json_escape(std::string_view s) {
      std::string out;
      out.reserve(s.size() + 8);
      for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\b':
          out += "\\b";
          break;
        case '\f':
          out += "\\f";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (c < 0x20) {
            static constexpr char k_hex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(k_hex[static_cast<std::size_t>(c >> 4)]);
            out.push_back(k_hex[static_cast<std::size_t>(c & 0x0f)]);
          } else {
            out.push_back(static_cast<char>(c));
          }
          break;
        }
      }
      return out;
    }

    void append_json_field(std::string& out, std::string_view name, std::string_view value,
                           bool& first) {
      if (!first)
        out += ",\n";
      first = false;
      out += "  \"";
      out += name;
      out += "\": \"";
      out += json_escape(value);
      out += "\"";
    }

    std::string make_manifest(const ManifestMetadata& metadata, std::string_view payload_sha256) {
      std::string out = "{\n";
      bool first = true;
      append_json_field(out, "appName", metadata.app_name, first);
      append_json_field(out, "version", metadata.version, first);
      append_json_field(out, "entry", metadata.entry, first);
      append_json_field(out, "createdAt", metadata.created_at, first);
      append_json_field(out, "compression", metadata.compression, first);
      append_json_field(out, "payloadSha256", payload_sha256, first);
      if (!metadata.channel.empty())
        append_json_field(out, "channel", metadata.channel, first);
      if (!metadata.update_url.empty())
        append_json_field(out, "updateUrl", metadata.update_url, first);
      if (!metadata.public_key.empty())
        append_json_field(out, "publicKey", metadata.public_key, first);
      out += "\n}\n";
      return out;
    }
    bool read_all(const std::string& path, std::string& out, std::string* error) {
      std::ifstream f(path, std::ios::binary);
      if (!f) {
        if (error)
          *error = "cannot open " + path;
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

  } // namespace

  bool write_trailer(const std::string& binary_path,
                     const std::vector<std::pair<std::string, Entry>>& entries,
                     std::string* error) {
    std::ofstream f(binary_path, std::ios::binary | std::ios::app);
    if (!f) {
      if (error)
        *error = "cannot open " + binary_path + " for append";
      return false;
    }
    const std::uint64_t index_offset = static_cast<std::uint64_t>(f.tellp());
    std::string buf;
    buf.reserve(entries.size() * 32);
    for (const auto& [name, e] : entries) {
      put_u32(buf, static_cast<std::uint32_t>(name.size()));
      buf.append(name);
      put_u64(buf, e.offset);
      put_u64(buf, e.size);
    }
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));

    std::string trailer;
    trailer.reserve(k_trailer_size);
    trailer.append(k_magic, 8);
    put_u32(trailer, k_version);
    put_u32(trailer, static_cast<std::uint32_t>(entries.size()));
    put_u64(trailer, index_offset);
    put_u64(trailer, /*payload_offset placeholder*/ 0);
    f.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    return f.good();
  }

  bool pack_files(const std::string& binary_path,
                  const std::vector<std::pair<std::string, std::string>>& files,
                  const ManifestMetadata* metadata, std::string* error) {
    std::vector<std::pair<std::string, Entry>> entries;
    entries.reserve(files.size() + (metadata ? 1 : 0));
    Sha256 payload_hash;
    {
      std::ofstream f(binary_path, std::ios::binary | std::ios::app);
      if (!f) {
        if (error)
          *error = "cannot open " + binary_path + " for append";
        return false;
      }
      for (const auto& [disk_path, archive_name] : files) {
        if (archive_name == k_manifest_name) {
          if (error)
            *error = std::string("reserved bundle archive name: ") + std::string(k_manifest_name);
          return false;
        }
        std::string body;
        if (!read_all(disk_path, body, error))
          return false;
        payload_hash.update(body);
        const std::uint64_t off = static_cast<std::uint64_t>(f.tellp());
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f.good()) {
          if (error)
            *error = "write failed for " + disk_path;
          return false;
        }
        entries.emplace_back(archive_name, Entry{off, body.size()});
      }
      if (metadata) {
        std::string manifest = make_manifest(*metadata, payload_hash.hex_digest());
        const std::uint64_t off = static_cast<std::uint64_t>(f.tellp());
        f.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
        if (!f.good()) {
          if (error)
            *error = "write failed for bundle manifest";
          return false;
        }
        entries.emplace_back(std::string(k_manifest_name), Entry{off, manifest.size()});
      }
    }
    return write_trailer(binary_path, entries, error);
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
    if (std::memcmp(trailer, k_magic, 8) != 0)
      return;
    const std::uint32_t version = read_u32(trailer + 8);
    if (version != k_version)
      return;
    const std::uint32_t count = read_u32(trailer + 12);
    const std::uint64_t index_offset = read_u64(trailer + 16);

    f.seekg(static_cast<std::streamoff>(index_offset));
    for (std::uint32_t i = 0; i < count; ++i) {
      char hdr[4];
      f.read(hdr, 4);
      if (!f)
        return;
      std::uint32_t name_len = read_u32(hdr);
      std::string name(name_len, '\0');
      f.read(name.data(), name_len);
      char tail[16];
      f.read(tail, 16);
      if (!f)
        return;
      Entry e{read_u64(tail), read_u64(tail + 8)};
      index_.emplace(std::move(name), e);
    }
    valid_ = true;
  }

  std::optional<std::string> Bundle::read(std::string_view name) const {
    if (!valid_)
      return std::nullopt;
    auto it = index_.find(std::string(name));
    if (it == index_.end())
      return std::nullopt;
    std::ifstream f(path_, std::ios::binary);
    if (!f)
      return std::nullopt;
    f.seekg(static_cast<std::streamoff>(it->second.offset));
    std::string body(it->second.size, '\0');
    f.read(body.data(), static_cast<std::streamsize>(it->second.size));
    if (!f)
      return std::nullopt;
    return body;
  }

  std::vector<std::string> Bundle::list() const {
    std::vector<std::string> out;
    out.reserve(index_.size());
    for (const auto& [k, _] : index_)
      out.push_back(k);
    return out;
  }

} // namespace fxe::bundle
