#include "transpile_cache.hpp"

#include "v8_code_cache.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace fxe::js {
  namespace {
    // On-disk file format mirrors v8_code_cache:
    //   "FXTC" magic | uint32 version | uint64 src_h | uint64 src_n
    //   | uint32 origin_n | origin bytes | uint32 emitted_n | emitted bytes
    //   | int32 source_map_line_offset
    constexpr uint32_t k_format_version = 1;
    constexpr char k_magic[4] = {'F', 'X', 'T', 'C'};

    struct cache_entry {
      std::string origin;
      std::string source;
      std::string emitted;
      int line_offset = 0;
    };

    struct cache_state {
      std::mutex mutex;
      std::unordered_multimap<uint64_t, cache_entry> entries;
    };

    cache_state& state() {
      static cache_state s;
      return s;
    }

    uint64_t fnv1a_append(uint64_t hash, std::string_view text) {
      for (char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
      }
      return hash;
    }

    uint64_t cache_key(std::string_view origin, std::string_view source) {
      uint64_t hash = 0xcbf29ce484222325ULL;
      hash = fnv1a_append(hash, origin);
      hash ^= 0x1fULL;
      hash *= 0x100000001b3ULL;
      hash = fnv1a_append(hash, source);
      return hash;
    }

    std::string hex64(uint64_t v) {
      char buf[17];
      std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
      return std::string(buf, 16);
    }

    std::filesystem::path disk_path(uint64_t key) {
      auto root = v8_code_cache::cache_dir();
      if (root.empty())
        return {};
      auto dir = root / "transpile";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec)
        return {};
      return dir / (hex64(key) + ".tsc");
    }

    bool read_disk(const std::filesystem::path& path, std::string_view origin,
                   std::string_view source, std::string& emitted, int& line_offset) {
      std::ifstream f(path, std::ios::binary);
      if (!f)
        return false;

      auto rd = [&](void* dst, size_t n) -> bool {
        return static_cast<bool>(f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
      };

      char magic[4];
      uint32_t version = 0;
      uint64_t src_hash = 0;
      uint64_t src_len = 0;
      uint32_t origin_n = 0;
      uint32_t emitted_n = 0;
      int32_t line_off = 0;
      if (!rd(magic, 4) || std::memcmp(magic, k_magic, 4) != 0)
        return false;
      if (!rd(&version, 4) || version != k_format_version)
        return false;
      if (!rd(&src_hash, 8) || !rd(&src_len, 8))
        return false;
      if (src_len != source.size() || src_hash != fnv1a_append(0xcbf29ce484222325ULL, source))
        return false;
      if (!rd(&origin_n, 4) || origin_n > (16u * 1024u))
        return false;
      std::string disk_origin(origin_n, '\0');
      if (origin_n && !rd(disk_origin.data(), origin_n))
        return false;
      if (disk_origin != origin)
        return false;
      if (!rd(&emitted_n, 4) || emitted_n > (256u * 1024u * 1024u))
        return false;
      std::string disk_emitted(emitted_n, '\0');
      if (emitted_n && !rd(disk_emitted.data(), emitted_n))
        return false;
      if (!rd(&line_off, 4))
        return false;
      emitted = std::move(disk_emitted);
      line_offset = static_cast<int>(line_off);
      return true;
    }

    void write_disk(const std::filesystem::path& path, std::string_view origin,
                    std::string_view source, std::string_view emitted, int line_offset) {
      if (path.empty())
        return;
      auto tmp = path;
      tmp += ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
          return;
        auto wr = [&](const void* p, size_t n) {
          f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
        };
        uint32_t version = k_format_version;
        uint64_t src_hash = fnv1a_append(0xcbf29ce484222325ULL, source);
        uint64_t src_len = source.size();
        uint32_t origin_n = static_cast<uint32_t>(origin.size());
        uint32_t emitted_n = static_cast<uint32_t>(emitted.size());
        int32_t line_off = static_cast<int32_t>(line_offset);
        wr(k_magic, 4);
        wr(&version, 4);
        wr(&src_hash, 8);
        wr(&src_len, 8);
        wr(&origin_n, 4);
        wr(origin.data(), origin.size());
        wr(&emitted_n, 4);
        wr(emitted.data(), emitted.size());
        wr(&line_off, 4);
        f.flush();
        if (!f) {
          std::error_code ec;
          std::filesystem::remove(tmp, ec);
          return;
        }
      }
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
      if (ec)
        std::filesystem::remove(tmp, ec);
    }
  } // namespace

  bool transpile_cache_lookup(std::string_view origin, std::string_view source,
                              std::string& emitted, int& source_map_line_offset) {
    auto& cache = state();
    const uint64_t key = cache_key(origin, source);
    {
      std::lock_guard lock(cache.mutex);
      const auto [first, last] = cache.entries.equal_range(key);
      for (auto it = first; it != last; ++it) {
        const cache_entry& entry = it->second;
        if (entry.origin == origin && entry.source == source) {
          emitted = entry.emitted;
          source_map_line_offset = entry.line_offset;
          return true;
        }
      }
    }

    // Miss in memory. Check disk.
    auto path = disk_path(key);
    if (path.empty())
      return false;
    std::string disk_emitted;
    int disk_line = 0;
    if (!read_disk(path, origin, source, disk_emitted, disk_line))
      return false;

    // Promote to in-memory cache.
    {
      std::lock_guard lock(cache.mutex);
      cache.entries.emplace(
          key, cache_entry{std::string(origin), std::string(source), disk_emitted, disk_line});
    }
    emitted = std::move(disk_emitted);
    source_map_line_offset = disk_line;
    return true;
  }

  void transpile_cache_store(std::string_view origin, std::string_view source, std::string emitted,
                             int source_map_line_offset) {
    auto& cache = state();
    const uint64_t key = cache_key(origin, source);
    {
      std::lock_guard lock(cache.mutex);
      const auto [first, last] = cache.entries.equal_range(key);
      for (auto it = first; it != last; ++it) {
        cache_entry& entry = it->second;
        if (entry.origin == origin && entry.source == source) {
          entry.emitted = emitted;
          entry.line_offset = source_map_line_offset;
          auto path = disk_path(key);
          if (!path.empty())
            write_disk(path, origin, source, entry.emitted, entry.line_offset);
          return;
        }
      }
      cache.entries.emplace(key, cache_entry{std::string(origin), std::string(source), emitted,
                                             source_map_line_offset});
    }
    auto path = disk_path(key);
    if (!path.empty())
      write_disk(path, origin, source, emitted, source_map_line_offset);
  }

  void transpile_cache_clear() {
    auto& cache = state();
    std::lock_guard lock(cache.mutex);
    cache.entries.clear();
  }
} // namespace fxe::js
