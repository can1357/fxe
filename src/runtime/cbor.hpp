#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fxe::runtime::cbor {

  struct value;
  using array = std::vector<value>;
  using map = std::map<std::string, value>;
  using storage = std::variant<std::nullptr_t, bool, int64_t, uint64_t, std::string,
                               std::vector<uint8_t>, array, map>;

  struct value : storage {
    using storage::storage;
    using storage::operator=;
  };

  std::vector<uint8_t> encode(const value& v);
  std::optional<value> decode(const uint8_t* data, size_t len);

} // namespace fxe::runtime::cbor
