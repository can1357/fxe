#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fxe::runtime::cbor {

  struct value;
  using array = std::vector<value>;
  using map = std::map<std::string, value>;
  using key = std::variant<int64_t, uint64_t, std::string, std::vector<uint8_t>>;
  using cmap = std::vector<std::pair<key, value>>;
  using storage = std::variant<std::nullptr_t, bool, int64_t, uint64_t, std::string,
                               std::vector<uint8_t>, array, map, cmap>;

  struct value : storage {
    using storage::storage;
    using storage::operator=;
  };

  std::vector<uint8_t> encode(const value& v);
  std::optional<value> decode(const uint8_t* data, size_t len);
  const value* find(const cmap& m, int64_t k);
  const value* find(const cmap& m, std::string_view k);
  const value* find(const cmap& m, const std::vector<uint8_t>& k);

} // namespace fxe::runtime::cbor
