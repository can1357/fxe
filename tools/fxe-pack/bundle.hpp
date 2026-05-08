// fxe bundle: zip-like archive appended to a host executable.
//
// On-disk layout of the trailer (last 32 bytes of a packed binary):
//   char    magic[8]   = "FXEBNDL\0"
//   uint32  version    = 1
//   uint32  index_count
//   uint64  index_offset    (from start of file)
//   uint64  payload_offset  (from start of file; first byte of file blobs)
//
// Index entries (at index_offset, repeated index_count times):
//   uint32  path_len
//   char    path[path_len]
//   uint64  offset   (from start of file)
//   uint64  size
//
// All integers little-endian.
#pragma once

#include <cstdint>
#include <fxe/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fxe::bundle {

  inline constexpr char k_magic[8] = {'F', 'X', 'E', 'B', 'N', 'D', 'L', '\0'};
  inline constexpr u32 k_version = 1;
  inline constexpr std::string_view k_manifest_name = "__fxe_manifest.json";
  inline constexpr usize k_trailer_size = 32;

  struct Entry {
    u64 offset;
    u64 size;
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
  };

  // Append a bundle (index + trailer) to file at `binary_path`. The file blobs
  // must already have been concatenated to the binary at the offsets recorded
  // in `entries`. Caller-friendly helper `pack_files` does both steps.
  bool write_trailer(const std::string& binary_path,
                     const std::vector<std::pair<std::string, Entry>>& entries,
                     std::string* error = nullptr);

  // Append `files` (path-on-disk -> archive-name) to `binary_path`. Returns
  // true on success. The binary is appended to in-place. When `metadata` is
  // supplied, a JSON manifest is stored as `k_manifest_name`.
  bool pack_files(const std::string& binary_path,
                  const std::vector<std::pair<std::string, std::string>>& files,
                  const ManifestMetadata* metadata = nullptr, std::string* error = nullptr);

  class Bundle {
  public:
    explicit Bundle(std::string path);

    // True iff the binary at `path` carries a valid trailer.
    bool valid() const noexcept {
      return valid_;
    }

    // Read a file by archive name. Returns nullopt if absent.
    std::optional<std::string> read(std::string_view name) const;

    // List archive names.
    std::vector<std::string> list() const;

  private:
    std::string path_;
    bool valid_ = false;
    std::unordered_map<std::string, Entry> index_;
  };

} // namespace fxe::bundle
