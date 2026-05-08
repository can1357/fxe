#include "runtime/cbor.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace {
  namespace cbor = fxe::runtime::cbor;

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  bool key_equals(const cbor::key& a, const cbor::key& b) {
    const auto same_type = std::visit(
        [](const auto& lhs, const auto& rhs) -> bool {
          using L = std::decay_t<decltype(lhs)>;
          using R = std::decay_t<decltype(rhs)>;
          if constexpr (!std::is_same_v<L, R>)
            return false;
          else
            return lhs == rhs;
        },
        a, b);
    return same_type;
  }

  bool value_equals(const cbor::value& a, const cbor::value& b) {
    const auto& lhs = static_cast<const cbor::storage&>(a);
    const auto& rhs = static_cast<const cbor::storage&>(b);
    if (lhs.index() != rhs.index())
      return false;
    return std::visit(
        [&](const auto& left, const auto& right) -> bool {
          using L = std::decay_t<decltype(left)>;
          using R = std::decay_t<decltype(right)>;
          if constexpr (!std::is_same_v<L, R>) {
            return false;
          } else if constexpr (std::is_same_v<L, cbor::array>) {
            if (left.size() != right.size())
              return false;
            for (size_t i = 0; i < left.size(); ++i) {
              if (!value_equals(left[i], right[i]))
                return false;
            }
            return true;
          } else if constexpr (std::is_same_v<L, cbor::map>) {
            if (left.size() != right.size())
              return false;
            auto lit = left.begin();
            auto rit = right.begin();
            for (; lit != left.end(); ++lit, ++rit) {
              if (lit->first != rit->first || !value_equals(lit->second, rit->second))
                return false;
            }
            return true;
          } else if constexpr (std::is_same_v<L, cbor::cmap>) {
            if (left.size() != right.size())
              return false;
            for (size_t i = 0; i < left.size(); ++i) {
              if (!key_equals(left[i].first, right[i].first) ||
                  !value_equals(left[i].second, right[i].second))
                return false;
            }
            return true;
          } else {
            return left == right;
          }
        },
        lhs, rhs);
  }

  std::vector<uint8_t> bytes32(uint8_t seed) {
    std::vector<uint8_t> out(32);
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = static_cast<uint8_t>(seed + i);
    return out;
  }
} // namespace

int main() {
  {
    const auto encoded = cbor::encode(cbor::value(cbor::cmap{}));
    CHECK(encoded == std::vector<uint8_t>({0xa0}));
  }

  {
    const cbor::cmap input = {
        {uint64_t(5), cbor::value(nullptr)},
        {uint64_t(4), cbor::value(nullptr)},
        {int64_t(-1), cbor::value(nullptr)},
        {std::string("alg"), cbor::value(nullptr)},
        {std::vector<uint8_t>{}, cbor::value(nullptr)},
        {std::string("a"), cbor::value(nullptr)},
    };
    const auto encoded = cbor::encode(cbor::value(input));
    const std::vector<uint8_t> expected = {
        0xa6, 0x04, 0xf6, 0x05, 0xf6, 0x20, 0xf6, 0x40, 0xf6,
        0x61, 0x61, 0xf6, 0x63, 0x61, 0x6c, 0x67, 0xf6,
    };
    CHECK(encoded == expected);
  }

  {
    const cbor::cmap nested = {
        {std::string("z"), cbor::value(cbor::cmap{{uint64_t(5), cbor::value(nullptr)},
                                                  {uint64_t(4), cbor::value(nullptr)}})},
        {uint64_t(1), cbor::value(uint64_t(9))},
    };
    const auto encoded = cbor::encode(cbor::value(nested));
    const std::vector<uint8_t> expected = {
        0xa2, 0x01, 0x09, 0x61, 0x7a, 0xa2, 0x04, 0xf6, 0x05, 0xf6,
    };
    CHECK(encoded == expected);
  }

  {
    const cbor::cmap input = {
        {uint64_t(1), cbor::value(uint64_t(2))},   {uint64_t(3), cbor::value(int64_t(-7))},
        {int64_t(-1), cbor::value(uint64_t(1))},   {int64_t(-2), cbor::value(bytes32(0x10))},
        {int64_t(-3), cbor::value(bytes32(0x80))},
    };
    const auto encoded = cbor::encode(cbor::value(input));
    CHECK(!encoded.empty());
    CHECK(encoded[0] == 0xa5);
    CHECK(encoded.size() > 1);
    CHECK(encoded[1] == 0x01);
    const auto decoded = cbor::decode(encoded.data(), encoded.size());
    CHECK(decoded.has_value());
    if (decoded) {
      const auto* cmap = std::get_if<cbor::cmap>(&static_cast<const cbor::storage&>(*decoded));
      CHECK(cmap != nullptr);
      if (cmap)
        CHECK(value_equals(*decoded, cbor::value(input)));
    }
  }

  {
    const std::vector<uint8_t> encoded = {0xa2, 0x01, 0x02, 0x63, 0x61, 0x6c, 0x67, 0x26};
    const auto decoded = cbor::decode(encoded.data(), encoded.size());
    CHECK(decoded.has_value());
    if (decoded) {
      const auto* cmap = std::get_if<cbor::cmap>(&static_cast<const cbor::storage&>(*decoded));
      CHECK(cmap != nullptr);
      if (cmap) {
        const auto* one = cbor::find(*cmap, int64_t(1));
        const auto* alg = cbor::find(*cmap, "alg");
        CHECK(one != nullptr);
        CHECK(alg != nullptr);
        CHECK(one && std::get_if<uint64_t>(&static_cast<const cbor::storage&>(*one)) &&
              *std::get_if<uint64_t>(&static_cast<const cbor::storage&>(*one)) == 2);
        CHECK(alg && std::get_if<int64_t>(&static_cast<const cbor::storage&>(*alg)) &&
              *std::get_if<int64_t>(&static_cast<const cbor::storage&>(*alg)) == -7);
      }
    }
  }

  {
    const std::vector<uint8_t> encoded = {0xa1, 0x61, 0x61, 0x01};
    const auto decoded = cbor::decode(encoded.data(), encoded.size());
    CHECK(decoded.has_value());
    if (decoded) {
      CHECK(std::get_if<cbor::map>(&static_cast<const cbor::storage&>(*decoded)) != nullptr);
      CHECK(std::get_if<cbor::cmap>(&static_cast<const cbor::storage&>(*decoded)) == nullptr);
    }
  }

  {
    const cbor::cmap input = {{std::vector<uint8_t>{0xde, 0xad}, cbor::value(uint64_t(7))}};
    const auto encoded = cbor::encode(cbor::value(input));
    CHECK(encoded == std::vector<uint8_t>({0xa1, 0x42, 0xde, 0xad, 0x07}));
    const auto decoded = cbor::decode(encoded.data(), encoded.size());
    CHECK(decoded.has_value());
    if (decoded) {
      const auto* cmap = std::get_if<cbor::cmap>(&static_cast<const cbor::storage&>(*decoded));
      CHECK(cmap != nullptr);
      if (cmap) {
        const auto* value = cbor::find(*cmap, std::vector<uint8_t>{0xde, 0xad});
        CHECK(value != nullptr);
        CHECK(value && std::get_if<uint64_t>(&static_cast<const cbor::storage&>(*value)) &&
              *std::get_if<uint64_t>(&static_cast<const cbor::storage&>(*value)) == 7);
      }
    }
  }

  {
    struct case_t {
      int64_t key;
      std::vector<uint8_t> expected;
    };
    const std::array<case_t, 5> cases = {{{-1, {0xa1, 0x20, 0xf6}},
                                          {-24, {0xa1, 0x37, 0xf6}},
                                          {-25, {0xa1, 0x38, 0x18, 0xf6}},
                                          {-256, {0xa1, 0x38, 0xff, 0xf6}},
                                          {-257, {0xa1, 0x39, 0x01, 0x00, 0xf6}}}};
    for (const auto& test : cases) {
      const auto encoded = cbor::encode(cbor::value(cbor::cmap{{test.key, cbor::value(nullptr)}}));
      CHECK(encoded == test.expected);
    }
  }

  {
    const std::vector<uint8_t> duplicate = {0xa2, 0x01, 0xf6, 0x18, 0x01, 0xf6};
    CHECK(!cbor::decode(duplicate.data(), duplicate.size()).has_value());
  }

  {
    const cbor::cmap input = {
        {std::string("alg"), cbor::value(int64_t(-7))},
        {uint64_t(5), cbor::value(nullptr)},
        {std::vector<uint8_t>{0x01},
         cbor::value(cbor::cmap{{int64_t(-1), cbor::value(uint64_t(1))},
                                {uint64_t(1), cbor::value(uint64_t(2))}})},
        {int64_t(-1), cbor::value(uint64_t(3))},
    };
    const auto encoded = cbor::encode(cbor::value(input));
    const auto decoded = cbor::decode(encoded.data(), encoded.size());
    CHECK(decoded.has_value());
    if (decoded)
      CHECK(cbor::encode(*decoded) == encoded);
  }

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
