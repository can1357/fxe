#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace fxe {

  // Returns `in` with leading and trailing ASCII whitespace (` `, `\t`, `\r`, `\n`)
  // stripped. Non-allocating; the returned view points into the same storage as
  // the input. Construct a `std::string` at the call site if owned storage is
  // needed.
  constexpr std::string_view trim(std::string_view in) noexcept {
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!in.empty() && is_ws(in.front()))
      in.remove_prefix(1);
    while (!in.empty() && is_ws(in.back()))
      in.remove_suffix(1);
    return in;
  }

  // Returns a lowercased copy of `in`, transforming only ASCII A-Z. Does not
  // touch bytes >= 0x80, so it is safe on UTF-8 inputs (no locale dependency,
  // no `std::tolower` undefined behaviour for negative `char` values).
  inline std::string ascii_lower(std::string_view in) {
    std::string out;
    out.resize(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(in[i]);
      out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    }
    return out;
  }

  // Returns an uppercased copy of `in`. ASCII-only; non-ASCII bytes are
  // unchanged. See `ascii_lower` for rationale.
  inline std::string ascii_upper(std::string_view in) {
    std::string out;
    out.resize(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(in[i]);
      out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : static_cast<char>(c);
    }
    return out;
  }

  // Lowercases `s` in place. Same ASCII-only semantics as `ascii_lower`.
  inline void ascii_lower_inplace(std::string& s) noexcept {
    for (char& ch : s) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (c >= 'A' && c <= 'Z')
        ch = static_cast<char>(c + ('a' - 'A'));
    }
  }

} // namespace fxe
