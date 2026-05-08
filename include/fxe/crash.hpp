#pragma once

#include <cstddef>
#include <fxe/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::os {
  struct crash_options {
    std::string product_name;
    std::string product_version;
    std::string submit_url; // empty = no upload
    std::string crash_dir;  // empty = userData/Crashes
    bool upload_to_server = false;
  };

  struct crash_self_test_result {
    bool ok = false;
    std::string dump_path;
    std::string error;
  };

  bool crash_start(const crash_options&);
  std::vector<std::string> crash_list_uploaded();
  std::optional<std::string> crash_get_last_dump_path();
  crash_self_test_result crash_self_test();

  namespace crash_detail {
    bool platform_install_handlers(const crash_options&);

    std::string current_crash_dir();
    crash_options current_options();
    std::string next_dump_path(const char* extension);
    void record_last_dump_path(std::string path);

    bool write_dump_bytes(const char* extension, const void* data, usize size);
    bool write_dump_text(const char* extension, std::string_view text);
    bool upload_last_dump_if_requested(const std::string& path);
    std::vector<std::string> list_dump_paths();

#ifndef _WIN32
    bool signal_next_dump_path(const char* extension, char* out, usize out_size) noexcept;
    void write_signal_dump(int signal_number, const void* address, const char* extension) noexcept;
#endif
    bool platform_self_test() noexcept;
  } // namespace crash_detail
} // namespace fxe::os
