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
