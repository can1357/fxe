#pragma once

// Common scalar type aliases used across fxe. Defined in the global namespace
// so callers may write `u32`/`usize`/`f32` without qualification regardless of
// the surrounding namespace. Project code MUST use these aliases instead of
// `std::uint32_t`, `uint32_t`, `std::size_t`, `size_t`, `float`, `double`, etc.
//
// See AGENTS.md ("Code Conventions & Common Patterns") for the rule.

#include <cstddef>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using usize = std::size_t;
using isize = std::ptrdiff_t;

using f32 = float;
using f64 = double;

// V8 internalized-string literal. Use as `"frameCount"_v8(iso)` to materialize
// a per-isolate cached, internalized v8::Local<v8::String>. The UDL itself
// only captures `{const char*, usize}`; the call operator is defined in
// <fxe/v8_strings.hpp>, which any V8-using TU (every src/js/bind_*.cpp) must
// include before invoking it. types.hpp deliberately stays free of v8.h so
// fxe_core (which does not link V8) can keep including it.
//
// Cache is keyed by the literal's `const char*` address; string literals have
// static storage duration and stable addresses, and V8 deduplicates identical
// internalized strings across cache entries, so pointer-keying is safe.
struct v8_string_literal {
  const char* data;
  usize size;

  // Defined in <fxe/v8_strings.hpp>. Templated on the V8 isolate type so this
  // header does not need to know about v8::Isolate.
  template <typename Iso>
  auto operator()(Iso* iso) const;
};

constexpr v8_string_literal operator""_v8(const char* s, usize n) noexcept {
  return {s, n};
}
