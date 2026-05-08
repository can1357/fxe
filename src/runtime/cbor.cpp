#include "cbor.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace fxe::runtime::cbor {
  namespace {

    void append_uint(std::vector<uint8_t>& out, uint8_t major, uint64_t n) {
      const uint8_t head = static_cast<uint8_t>(major << 5);
      if (n <= 23) {
        out.push_back(static_cast<uint8_t>(head | n));
      } else if (n <= 0xff) {
        out.push_back(static_cast<uint8_t>(head | 24));
        out.push_back(static_cast<uint8_t>(n));
      } else if (n <= 0xffff) {
        out.push_back(static_cast<uint8_t>(head | 25));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(n));
      } else if (n <= 0xffffffffu) {
        out.push_back(static_cast<uint8_t>(head | 26));
        for (int shift = 24; shift >= 0; shift -= 8)
          out.push_back(static_cast<uint8_t>(n >> shift));
      } else {
        out.push_back(static_cast<uint8_t>(head | 27));
        for (int shift = 56; shift >= 0; shift -= 8)
          out.push_back(static_cast<uint8_t>(n >> shift));
      }
    }

    void encode_key_into(std::vector<uint8_t>& out, const key& k) {
      std::visit(
          [&](const auto& inner) {
            using T = std::decay_t<decltype(inner)>;
            if constexpr (std::is_same_v<T, uint64_t>) {
              append_uint(out, 0, inner);
            } else if constexpr (std::is_same_v<T, int64_t>) {
              if (inner >= 0)
                append_uint(out, 0, static_cast<uint64_t>(inner));
              else
                append_uint(out, 1, static_cast<uint64_t>(-(inner + 1)));
            } else if constexpr (std::is_same_v<T, std::string>) {
              append_uint(out, 3, inner.size());
              out.insert(out.end(), inner.begin(), inner.end());
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
              append_uint(out, 2, inner.size());
              out.insert(out.end(), inner.begin(), inner.end());
            }
          },
          k);
    }

    bool key_less(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
      if (a.size() != b.size())
        return a.size() < b.size();
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }

    bool encode_decoded_key(const value& v, key& out_key, std::vector<uint8_t>& out_bytes) {
      const storage& inner = static_cast<const storage&>(v);
      if (const auto* signed_key = std::get_if<int64_t>(&inner)) {
        out_key = *signed_key;
      } else if (const auto* unsigned_key = std::get_if<uint64_t>(&inner)) {
        out_key = *unsigned_key;
      } else if (const auto* text = std::get_if<std::string>(&inner)) {
        out_key = *text;
      } else if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&inner)) {
        out_key = *bytes;
      } else {
        return false;
      }
      encode_key_into(out_bytes, out_key);
      return true;
    }

    bool numeric_key_equals(const key& candidate, int64_t wanted) {
      if (const auto* n = std::get_if<int64_t>(&candidate))
        return *n == wanted;
      if (const auto* n = std::get_if<uint64_t>(&candidate))
        return wanted >= 0 && *n == static_cast<uint64_t>(wanted);
      return false;
    }

    void encode_into(std::vector<uint8_t>& out, const value& v) {
      std::visit(
          [&](const auto& inner) {
            using T = std::decay_t<decltype(inner)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
              out.push_back(0xf6);
            } else if constexpr (std::is_same_v<T, bool>) {
              out.push_back(inner ? 0xf5 : 0xf4);
            } else if constexpr (std::is_same_v<T, uint64_t>) {
              append_uint(out, 0, inner);
            } else if constexpr (std::is_same_v<T, int64_t>) {
              if (inner >= 0)
                append_uint(out, 0, static_cast<uint64_t>(inner));
              else
                append_uint(out, 1, static_cast<uint64_t>(-(inner + 1)));
            } else if constexpr (std::is_same_v<T, std::string>) {
              append_uint(out, 3, inner.size());
              out.insert(out.end(), inner.begin(), inner.end());
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
              append_uint(out, 2, inner.size());
              out.insert(out.end(), inner.begin(), inner.end());
            } else if constexpr (std::is_same_v<T, array>) {
              append_uint(out, 4, inner.size());
              for (const auto& item : inner)
                encode_into(out, item);
            } else if constexpr (std::is_same_v<T, map>) {
              std::vector<std::pair<std::string, const value*>> items;
              items.reserve(inner.size());
              for (const auto& [key, item] : inner)
                items.push_back({key, &item});
              std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                if (a.first.size() != b.first.size())
                  return a.first.size() < b.first.size();
                return a.first < b.first;
              });
              append_uint(out, 5, items.size());
              for (const auto& [key, item] : items) {
                append_uint(out, 3, key.size());
                out.insert(out.end(), key.begin(), key.end());
                encode_into(out, *item);
              }
            } else if constexpr (std::is_same_v<T, cmap>) {
              struct cmap_entry {
                std::vector<uint8_t> encoded_key;
                const value* item;
              };
              std::vector<cmap_entry> items;
              items.reserve(inner.size());
              for (const auto& [map_key, item] : inner) {
                auto& entry = items.emplace_back();
                encode_key_into(entry.encoded_key, map_key);
                entry.item = &item;
              }
              std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                return key_less(a.encoded_key, b.encoded_key);
              });
              append_uint(out, 5, items.size());
              for (const auto& entry : items) {
                out.insert(out.end(), entry.encoded_key.begin(), entry.encoded_key.end());
                encode_into(out, *entry.item);
              }
            }
          },
          static_cast<const storage&>(v));
    }

    struct decoder {
      const uint8_t* cur;
      const uint8_t* end;

      std::optional<uint64_t> read_uint(uint8_t ai) {
        if (ai <= 23)
          return ai;
        size_t width = 0;
        if (ai == 24)
          width = 1;
        else if (ai == 25)
          width = 2;
        else if (ai == 26)
          width = 4;
        else if (ai == 27)
          width = 8;
        else
          return std::nullopt;
        if (static_cast<size_t>(end - cur) < width)
          return std::nullopt;
        uint64_t out = 0;
        for (size_t i = 0; i < width; ++i)
          out = (out << 8) | *cur++;
        return out;
      }

      std::optional<value> read() {
        if (cur == end)
          return std::nullopt;
        const uint8_t head = *cur++;
        const uint8_t major = static_cast<uint8_t>(head >> 5);
        const uint8_t ai = static_cast<uint8_t>(head & 0x1f);
        if (major <= 1) {
          const auto n = read_uint(ai);
          if (!n)
            return std::nullopt;
          if (major == 0)
            return value(*n);
          if (*n > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return std::nullopt;
          return value(-1 - static_cast<int64_t>(*n));
        }
        if (major == 2 || major == 3) {
          const auto n = read_uint(ai);
          if (!n || static_cast<uint64_t>(end - cur) < *n)
            return std::nullopt;
          if (major == 2) {
            std::vector<uint8_t> bytes(cur, cur + *n);
            cur += *n;
            return value(std::move(bytes));
          }
          std::string text(reinterpret_cast<const char*>(cur), static_cast<size_t>(*n));
          cur += *n;
          return value(std::move(text));
        }
        if (major == 4) {
          const auto n = read_uint(ai);
          if (!n)
            return std::nullopt;
          array items;
          items.reserve(static_cast<size_t>(*n));
          for (uint64_t i = 0; i < *n; ++i) {
            auto item = read();
            if (!item)
              return std::nullopt;
            items.push_back(std::move(*item));
          }
          return value(std::move(items));
        }
        if (major == 5) {
          const auto n = read_uint(ai);
          if (!n)
            return std::nullopt;
          map text_items;
          cmap mixed_items;
          std::vector<std::vector<uint8_t>> seen_keys;
          text_items.clear();
          mixed_items.reserve(static_cast<size_t>(*n));
          seen_keys.reserve(static_cast<size_t>(*n));
          bool all_text = true;
          for (uint64_t i = 0; i < *n; ++i) {
            auto parsed_key = read();
            auto item = read();
            if (!parsed_key || !item)
              return std::nullopt;

            key map_key;
            std::vector<uint8_t> encoded_key;
            if (!encode_decoded_key(*parsed_key, map_key, encoded_key))
              return std::nullopt;
            if (std::any_of(seen_keys.begin(), seen_keys.end(),
                            [&](const auto& seen) { return seen == encoded_key; })) {
              return std::nullopt;
            }
            seen_keys.push_back(encoded_key);
            mixed_items.push_back({std::move(map_key), std::move(*item)});

            if (const auto* text =
                    std::get_if<std::string>(&static_cast<const storage&>(*parsed_key))) {
              text_items.emplace(*text, mixed_items.back().second);
            } else {
              all_text = false;
            }
          }
          if (all_text)
            return value(std::move(text_items));
          return value(std::move(mixed_items));
        }
        if (major == 7) {
          if (ai == 20)
            return value(false);
          if (ai == 21)
            return value(true);
          if (ai == 22)
            return value(nullptr);
        }
        return std::nullopt;
      }
    };

  } // namespace

  std::vector<uint8_t> encode(const value& v) {
    std::vector<uint8_t> out;
    encode_into(out, v);
    return out;
  }

  std::optional<value> decode(const uint8_t* data, size_t len) {
    decoder d{data, data + len};
    auto v = d.read();
    if (!v || d.cur != d.end)
      return std::nullopt;
    return v;
  }

  const value* find(const cmap& m, int64_t k) {
    for (const auto& [map_key, item] : m) {
      if (numeric_key_equals(map_key, k))
        return &item;
    }
    return nullptr;
  }

  const value* find(const cmap& m, std::string_view k) {
    for (const auto& [map_key, item] : m) {
      const auto* text = std::get_if<std::string>(&map_key);
      if (text && *text == k)
        return &item;
    }
    return nullptr;
  }

  const value* find(const cmap& m, const std::vector<uint8_t>& k) {
    for (const auto& [map_key, item] : m) {
      const auto* bytes = std::get_if<std::vector<uint8_t>>(&map_key);
      if (bytes && *bytes == k)
        return &item;
    }
    return nullptr;
  }

} // namespace fxe::runtime::cbor
