#pragma once

#include <cstddef>
#include <fxe/types.hpp>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::js {
  struct source_mapped_position {
    std::string source;
    int line = 0;   // 1-based
    int column = 0; // 1-based
    std::string name;
  };

  class source_map_decoder {
  public:
    source_map_decoder() = default;

    [[nodiscard]] static std::optional<source_map_decoder> parse(std::string_view module_url,
                                                                 std::string_view source_map_json,
                                                                 int generated_line_offset = 0);

    [[nodiscard]] std::optional<source_mapped_position>
    original_position(int generated_line, int generated_column) const;

    [[nodiscard]] bool empty() const noexcept;

  private:
    struct segment {
      int generated_column = 0;
      int source_index = -1;
      int source_line = -1;
      int source_column = -1;
      int name_index = -1;
    };

    std::string module_url_;
    int generated_line_offset_ = 0;
    std::vector<std::vector<segment>> mappings_;
    std::vector<std::string> sources_;
    std::vector<std::string> names_;
  };

  class source_map_cache {
  public:
    explicit source_map_cache(usize capacity = 64);

    void put(std::string_view module_url, source_map_decoder decoder);
    void put_json(std::string_view module_url, std::string_view source_map_json,
                  int generated_line_offset = 0);
    void erase(std::string_view module_url);
    void clear();

    [[nodiscard]] std::optional<source_mapped_position>
    original_position(std::string_view module_url, int generated_line, int generated_column);

    [[nodiscard]] usize capacity() const noexcept {
      return capacity_;
    }
    [[nodiscard]] usize size() const noexcept;

  private:
    struct entry {
      std::string module_url;
      source_map_decoder decoder;
    };

    usize capacity_ = 64;
    std::list<entry> entries_;
  };

  source_map_cache& source_maps();
  std::string normalize_source_map_url(std::string_view url);
} // namespace fxe::js
