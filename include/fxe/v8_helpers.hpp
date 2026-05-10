// Tiny binding-side helpers that compress the four most-repeated V8 patterns
// across `bind_*.cpp`. Header-only.
//
// Cheat sheet — prefer these over hand-rolled equivalents in new bindings:
//
//   External pointer wrapping
//   -------------------------
//   make_external(iso, ptr)        -> Local<External>
//       `External::New` with the default external-pointer type tag preset.
//
//   external_ptr<T>(value)         -> T*
//   external_ptr<T>(local_external)-> T*
//   external_ptr<T>(local_data)    -> T*
//       Recover `T*` from a `Local<Value>` / `Local<External>` / `Local<Data>`
//       (no checks). Use when you already know the slot holds an External.
//
//   internal_ptr<T>(obj, slot=0)   -> T*
//       Recover `T*` from `obj->GetInternalField(slot)`. Caller is
//       responsible for slot/tag validation; use `js::unwrap` (in
//       `<fxe/js_bindings.hpp>`) when a tag-checked path is required.
//
//   set_native(iso, obj, ptr, tag) -> void
//       Two-line write of internal field 0 = External(ptr) and field 1 =
//       Uint32(tag). Mirrors the body of `js::wrap` for objects created via a
//       custom path (constructor `info.This()`, custom template, etc.).
//
//   Exception helpers
//   -----------------
//   throw_error(iso, msg)          -> bool (always false)
//   throw_type_error(iso, msg)     -> bool (always false)
//   throw_range_error(iso, msg)    -> bool (always false)
//       `iso->ThrowException` fronts. `msg` accepts either a runtime
//       `std::string_view` (literal, `std::string`, `string_view`, …) or a
//       compile-checked `std::format_string<...>` plus arguments. All return
//       `false` so call sites can write
//
//           return throw_type_error(iso, "queue: expected CommandBuffer");
//
//       from `bool` helpers, or
//
//           (void)throw_type_error(iso, "queue: expected CommandBuffer");
//           return;
//
//       from `void` callbacks. Prefer these over the raw
//       `iso->ThrowException(Exception::TypeError(...))` form — they are
//       shorter, accept `std::format` directly, and avoid the boilerplate
//       around `String::NewFromUtf8(...).ToLocalChecked()`.
//
// These wrap the V8 surface in plain inline functions — no allocation, no
// state, no link surface. Keep behavior identical to the inline forms they
// replaced; do not add validation here unless every caller wants it.

#pragma once

#include <fxe/types.hpp>
#include <fxe/v8_literals.hpp>

#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <xstd/type_helpers.hpp>

#include <v8.h>

namespace fxe::js {

  // External pointer wrapping ---------------------------------------------

  template <typename T> inline v8::Local<v8::External> make_external(v8::Isolate* iso, T* ptr) {
    return v8::External::New(iso, const_cast<void*>(static_cast<const void*>(ptr)),
                             v8::kExternalPointerTypeTagDefault);
  }

  template <typename T> inline T* external_ptr(v8::Local<v8::Value> value) {
    return static_cast<T*>(value.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  }

  template <typename T> inline T* external_ptr(v8::Local<v8::External> ext) {
    return static_cast<T*>(ext->Value(v8::kExternalPointerTypeTagDefault));
  }

  template <typename T> inline T* external_ptr(v8::Local<v8::Data> data) {
    return external_ptr<T>(data.template As<v8::Value>());
  }

  template <typename T> inline T* internal_ptr(v8::Local<v8::Object> obj, int slot = 0) {
    return external_ptr<T>(obj->GetInternalField(slot).template As<v8::Value>());
  }

  // Set internal field 0 = External(native), field 1 = Uint32(type_tag).
  // Mirrors the body of `js::wrap` for objects that were already created via
  // a custom path (constructor `info.This()`, custom template, etc.).
  template <typename T>
  inline void set_native(v8::Isolate* iso, v8::Local<v8::Object> obj, T* native, u32 type_tag) {
    obj->SetInternalField(0, make_external(iso, native));
    obj->SetInternalField(1, v8::Integer::NewFromUnsigned(iso, type_tag));
  }

  // Exception helpers -----------------------------------------------------

  namespace detail {
    inline v8::Local<v8::String> v8_msg(v8::Isolate* iso, std::string_view msg) {
      return v8::String::NewFromUtf8(iso, msg.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(msg.size()))
          .ToLocalChecked();
    }

    using thrower = v8::Local<v8::Value> (*)(v8::Local<v8::String>, v8::Local<v8::Value>);

    inline bool throw_with(v8::Isolate* iso, thrower make, std::string_view msg) {
      iso->ThrowException(make(v8_msg(iso, msg), {}));
      return false;
    }

    template <typename... Args>
    inline bool throw_fmt(v8::Isolate* iso, thrower make, std::format_string<Args...> fmt,
                          Args&&... args) {
      std::string msg = std::format(fmt, std::forward<Args>(args)...);
      iso->ThrowException(make(v8_msg(iso, msg), {}));
      return false;
    }
  } // namespace detail

  inline bool throw_error(v8::Isolate* iso, std::string_view msg) {
    return detail::throw_with(iso, &v8::Exception::Error, msg);
  }
  inline bool throw_type_error(v8::Isolate* iso, std::string_view msg) {
    return detail::throw_with(iso, &v8::Exception::TypeError, msg);
  }
  inline bool throw_range_error(v8::Isolate* iso, std::string_view msg) {
    return detail::throw_with(iso, &v8::Exception::RangeError, msg);
  }

  // Format-string overloads. SFINAE-gated to `sizeof...(Args) >= 1` so plain
  // string literals continue to bind to the `string_view` overload above and
  // avoid the (compile-time but non-zero) cost of running the format parser.
  template <typename... Args>
    requires(sizeof...(Args) >= 1)
  inline bool throw_error(v8::Isolate* iso, std::format_string<Args...> fmt, Args&&... args) {
    return detail::throw_fmt(iso, &v8::Exception::Error, fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
    requires(sizeof...(Args) >= 1)
  inline bool throw_type_error(v8::Isolate* iso, std::format_string<Args...> fmt, Args&&... args) {
    return detail::throw_fmt(iso, &v8::Exception::TypeError, fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
    requires(sizeof...(Args) >= 1)
  inline bool throw_range_error(v8::Isolate* iso, std::format_string<Args...> fmt, Args&&... args) {
    return detail::throw_fmt(iso, &v8::Exception::RangeError, fmt, std::forward<Args>(args)...);
  }

  // Forward declarations — bodies live further down with the other string
  // construction helpers, but `throw_coded` / `throw_named` need them now.
  inline v8::Local<v8::String> to_v8_string(v8::Isolate*, std::string_view);
  inline v8::Local<v8::String> to_v8_string_internalized(v8::Isolate*, const char*, int);
  template <usize N>
  inline v8::Local<v8::String> to_v8_string_internalized(v8::Isolate*, const char (&)[N]);

  // Throw an Error with a `code` and/or `name` property — Node-style.
  // Each shape mirrors `throw_error`: a plain message, or a `std::format`
  // template + args that materialises the message lazily.

  namespace detail {
    inline void throw_coded(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view code,
                            std::string_view message) {
      auto err = v8::Exception::Error(to_v8_string(iso, message));
      if (err->IsObject()) {
        auto obj = err.As<v8::Object>();
        (void)obj->Set(ctx, to_v8_string_internalized(iso, "code"), to_v8_string(iso, code));
      }
      iso->ThrowException(err);
    }

    inline void throw_named(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view name,
                            std::string_view message) {
      auto err = v8::Exception::Error(to_v8_string(iso, message));
      if (err->IsObject()) {
        auto obj = err.As<v8::Object>();
        (void)obj->Set(ctx, to_v8_string_internalized(iso, "name"), to_v8_string(iso, name));
      }
      iso->ThrowException(err);
    }

    inline void throw_exception(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view code,
                                std::string_view name, std::string_view message) {
      auto err = v8::Exception::Error(to_v8_string(iso, message));
      if (err->IsObject()) {
        auto obj = err.As<v8::Object>();
        (void)obj->Set(ctx, to_v8_string_internalized(iso, "code"), to_v8_string(iso, code));
        (void)obj->Set(ctx, to_v8_string_internalized(iso, "name"), to_v8_string(iso, name));
      }
      iso->ThrowException(err);
    }
  } // namespace detail

  inline void throw_coded_error(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view code,
                                std::string_view message) {
    detail::throw_coded(iso, ctx, code, message);
  }

  template <typename... Args>
    requires(sizeof...(Args) >= 1)
  inline void throw_coded_error(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view code,
                                std::format_string<Args...> fmt, Args&&... args) {
    detail::throw_coded(iso, ctx, code, std::format(fmt, std::forward<Args>(args)...));
  }

  inline void throw_named(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view name,
                          std::string_view message) {
    detail::throw_named(iso, ctx, name, message);
  }

  template <typename... Args>
    requires(sizeof...(Args) >= 1)
  inline void throw_named(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view name,
                          std::format_string<Args...> fmt, Args&&... args) {
    detail::throw_named(iso, ctx, name, std::format(fmt, std::forward<Args>(args)...));
  }

  inline void throw_exception(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view code,
                              std::string_view name, std::string_view message) {
    detail::throw_exception(iso, ctx, code, name, message);
  }

  template <typename... Args>
    requires(sizeof...(Args) >= 1)
  inline void throw_exception(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view code,
                              std::string_view name, std::format_string<Args...> fmt,
                              Args&&... args) {
    detail::throw_exception(iso, ctx, code, name, std::format(fmt, std::forward<Args>(args)...));
  }
  // Undefined / Null helpers ---------------------------------------------

  inline v8::Local<v8::Primitive> to_v8_undefined(v8::Isolate* iso) {
    return v8::Undefined(iso);
  }
  inline v8::Local<v8::Primitive> to_v8_null(v8::Isolate* iso) {
    return v8::Null(iso);
  }

  // Value conversions ------------------------------------------------------
  //
  // Customization point: specialize `v8_value_of<T>` with a static
  // `to(Isolate*, T) -> Local<X>` to plug a new C++ type into `to_v8(...)`,
  // `set_prop(...)`, and any future helper that wants a uniform "make a
  // Local from a C++ value" entry point.
  //
  //     template <> struct v8_value_of<my_handle> {
  //       static v8::Local<v8::Value> to(v8::Isolate* iso, const my_handle& h) {
  //         return wrap_handle(iso, h);
  //       }
  //     };
  //
  // Built-ins:
  //   - bool                        -> Boolean
  //   - signed integral (≤ 32 bit)  -> Integer (Int32)
  //   - unsigned integral (≤ 32 bit)-> Integer (Uint32)
  //   - i64 / u64                   -> Number (double; precision loss above 2^53)
  //   - f32 / f64                   -> Number
  //   - nullptr_t / nullopt_t       -> Null
  //   - std::optional<T>            -> Null when empty, else `to_v8(*opt)`
  //   - std::string{,_view}, char*, const char*, char[N]
  //                                 -> String (UTF-8, NewFromUtf8)
  //   - std::u16string{,_view}      -> String (UTF-16, NewFromTwoByte)
  //   - std::span<T> / std::vector<T> / std::array<T, N> / T[N]
  //                                 -> Array (each element through `to_v8`)
  //   - v8::Local<T>                -> passthrough
  //   - v8::Global<T> / v8::Eternal<T>
  //                                 -> handle.Get(iso)

  template <typename T> struct v8_value_of;

  // Passthrough for any `v8::Local<T>` (Value, String, Number, Object, …).
  template <typename T> struct v8_value_of<v8::Local<T>> {
    static v8::Local<T> to(v8::Isolate*, v8::Local<T> v) {
      return v;
    }
  };

  // Lift `v8::Global<T>` / `v8::Eternal<T>` into a `Local<T>` automatically.
  template <typename T> struct v8_value_of<v8::Global<T>> {
    static v8::Local<T> to(v8::Isolate* iso, const v8::Global<T>& g) {
      return g.Get(iso);
    }
  };
  template <typename T> struct v8_value_of<v8::Eternal<T>> {
    static v8::Local<T> to(v8::Isolate* iso, const v8::Eternal<T>& e) {
      return e.Get(iso);
    }
  };

  // `_v8` user-defined literals — `"key"_v8`, `42_v8`, `1.5_v8`. Each
  // literal struct exposes `operator()(Isolate*)` returning a `Local<...>`,
  // so flow them through that.
  template <> struct v8_value_of<v8_string_literal> {
    static v8::Local<v8::String> to(v8::Isolate* iso, v8_string_literal lit) {
      return lit(iso);
    }
  };
  template <> struct v8_value_of<v8_integer_literal> {
    static v8::Local<v8::Number> to(v8::Isolate* iso, v8_integer_literal lit) {
      return lit(iso);
    }
  };
  template <> struct v8_value_of<v8_float_literal> {
    static v8::Local<v8::Number> to(v8::Isolate* iso, v8_float_literal lit) {
      return lit(iso);
    }
  };

  template <> struct v8_value_of<bool> {
    static v8::Local<v8::Boolean> to(v8::Isolate* iso, bool v) {
      return v8::Boolean::New(iso, v);
    }
  };

  // Signed integers up to 32 bits → Integer (Int32). `bool` is excluded so the
  // dedicated overload above is preferred.
  template <typename T>
    requires std::is_integral_v<T> && std::is_signed_v<T> && (!std::is_same_v<T, bool>) &&
             (sizeof(T) <= 4)
  struct v8_value_of<T> {
    static v8::Local<v8::Integer> to(v8::Isolate* iso, T v) {
      return v8::Integer::New(iso, static_cast<i32>(v));
    }
  };

  // Unsigned integers up to 32 bits → Integer (Uint32).
  template <typename T>
    requires std::is_integral_v<T> && std::is_unsigned_v<T> && (!std::is_same_v<T, bool>) &&
             (sizeof(T) <= 4)
  struct v8_value_of<T> {
    static v8::Local<v8::Integer> to(v8::Isolate* iso, T v) {
      return v8::Integer::NewFromUnsigned(iso, static_cast<u32>(v));
    }
  };

  // 64-bit integers → Number. Values > 2^53 lose precision; reach for
  // `v8::BigInt::New{,FromUnsigned}` directly when you need exact 64-bit.
  template <typename T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, bool>) && (sizeof(T) > 4)
  struct v8_value_of<T> {
    static v8::Local<v8::Number> to(v8::Isolate* iso, T v) {
      return v8::Number::New(iso, static_cast<f64>(v));
    }
  };

  template <typename T>
    requires std::is_floating_point_v<T>
  struct v8_value_of<T> {
    static v8::Local<v8::Number> to(v8::Isolate* iso, T v) {
      return v8::Number::New(iso, static_cast<f64>(v));
    }
  };

  // null / undefined --------------------------------------------------------

  template <> struct v8_value_of<std::nullptr_t> {
    static v8::Local<v8::Primitive> to(v8::Isolate* iso, std::nullptr_t) {
      return v8::Null(iso);
    }
  };
  template <> struct v8_value_of<std::nullopt_t> {
    static v8::Local<v8::Primitive> to(v8::Isolate* iso, std::nullopt_t) {
      return v8::Null(iso);
    }
  };

  // Strings -----------------------------------------------------------------

  // Build a `Local<String>` from a dynamic `std::string_view`. For
  // compile-time literals, prefer `"foo"_v8(iso)` from `<fxe/v8_literals.hpp>`
  // — that path is cached per isolate and skips the `String::NewFromUtf8` cost.
  inline v8::Local<v8::String> to_v8_string(v8::Isolate* iso, std::string_view s) {
    return v8::String::NewFromUtf8(iso, s.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(s.size()))
        .ToLocalChecked();
  }

  // Build an internalized `Local<String>` from a C string. Suitable for
  // string literals and other long-lived `const char*` keys / values: V8
  // dedupes against its string table so subsequent property lookups can
  // fast-path on identity. Pays for one strlen on first use; pure win when
  // the same address recurs (literals, static buffers).
  inline v8::Local<v8::String> to_v8_string_internalized(v8::Isolate* iso, const char* s,
                                                         int length = -1) {
    return v8::String::NewFromUtf8(iso, s, v8::NewStringType::kInternalized, length)
        .ToLocalChecked();
  }

  // Char-array overload: trims the trailing NUL via `N - 1` so callers writing
  // `to_v8_string_internalized(iso, "code")` skip the strlen the `const char*`
  // overload would otherwise pay on first use.
  template <usize N>
  inline v8::Local<v8::String> to_v8_string_internalized(v8::Isolate* iso, const char (&s)[N]) {
    return v8::String::NewFromUtf8(iso, s, v8::NewStringType::kInternalized,
                                   static_cast<int>(N - 1))
        .ToLocalChecked();
  }

  inline v8::Local<v8::String> to_v8_string(v8::Isolate* iso, std::u16string_view s) {
    static_assert(sizeof(char16_t) == sizeof(uint16_t));
    return v8::String::NewFromTwoByte(iso, reinterpret_cast<const uint16_t*>(s.data()),
                                      v8::NewStringType::kNormal, static_cast<int>(s.size()))
        .ToLocalChecked();
  }

  template <> struct v8_value_of<std::string_view> {
    static v8::Local<v8::String> to(v8::Isolate* iso, std::string_view s) {
      return to_v8_string(iso, s);
    }
  };
  template <> struct v8_value_of<std::string> : v8_value_of<std::string_view> {};
  // `const char*` and `char[N]` are almost always pointing at static storage
  // (string literals, embedded `constexpr` arrays). Route them through
  // `kInternalized` so identical strings collapse to a single V8 entry. For
  // hot keys reused every frame, prefer `"key"_v8(iso)` from
  // `<fxe/v8_literals.hpp>` — same internalized result, but cached per
  // isolate to skip the string-table probe entirely.
  template <> struct v8_value_of<const char*> {
    static v8::Local<v8::String> to(v8::Isolate* iso, const char* s) {
      return to_v8_string_internalized(iso, s);
    }
  };
  template <usize N> struct v8_value_of<char[N]> {
    static v8::Local<v8::String> to(v8::Isolate* iso, const char (&s)[N]) {
      // Dispatches to the `const char(&)[N]` overload — `length = N - 1` is
      // baked in, so no strlen on first use.
      return to_v8_string_internalized(iso, s);
    }
  };
  // `char*` (non-const) is typically a runtime buffer; keep it on the
  // non-internalized path.
  template <> struct v8_value_of<char*> : v8_value_of<std::string_view> {};

  template <> struct v8_value_of<std::u16string_view> {
    static v8::Local<v8::String> to(v8::Isolate* iso, std::u16string_view s) {
      return to_v8_string(iso, s);
    }
  };
  template <> struct v8_value_of<std::u16string> : v8_value_of<std::u16string_view> {};
  template <> struct v8_value_of<const char16_t*> : v8_value_of<std::u16string_view> {};
  template <> struct v8_value_of<char16_t*> : v8_value_of<std::u16string_view> {};
  template <usize N> struct v8_value_of<char16_t[N]> : v8_value_of<std::u16string_view> {};

  // Single entry point: convert any T with a `v8_value_of<T>` specialization
  // into a V8 handle. Array types (`const char[N]`, `int[N]`, …) keep their
  // bound `N` through dispatch so `v8_value_of<T[N]>` specializations see the
  // compile-time length; everything else decays cv-ref before lookup.
  template <typename T> inline auto to_v8(v8::Isolate* iso, T&& value) {
    using R = std::remove_reference_t<T>;
    if constexpr (std::is_array_v<R>) {
      return v8_value_of<std::remove_cv_t<R>>::to(iso, value);
    } else {
      return v8_value_of<std::decay_t<T>>::to(iso, std::forward<T>(value));
    }
  }

  // optional ----------------------------------------------------------------
  //
  // `optional<T>` flows through `to_v8` so any inner `T` with a specialization
  // (including nested `optional<U>`, vectors, etc.) works.

  template <typename T> struct v8_value_of<std::optional<T>> {
    static v8::Local<v8::Value> to(v8::Isolate* iso, const std::optional<T>& v) {
      if (!v.has_value())
        return v8::Null(iso);
      return to_v8(iso, *v);
    }
  };

  // Containers → Array ------------------------------------------------------
  //
  // Each element is converted through `to_v8`, so element types must
  // themselves have a `v8_value_of` specialization. Empty inputs produce an
  // empty `v8::Array` (length 0).

  namespace detail {
    template <typename Range>
    inline v8::Local<v8::Array> range_to_array(v8::Isolate* iso, const Range& r) {
      auto ctx = iso->GetCurrentContext();
      const auto n = static_cast<int>(std::size(r));
      auto arr = v8::Array::New(iso, n);
      int i = 0;
      for (const auto& item : r) {
        (void)arr->Set(ctx, static_cast<u32>(i++), to_v8(iso, item));
      }
      return arr;
    }
  } // namespace detail

  template <typename T, usize Extent> struct v8_value_of<std::span<T, Extent>> {
    static v8::Local<v8::Array> to(v8::Isolate* iso, std::span<T, Extent> s) {
      return detail::range_to_array(iso, s);
    }
  };
  template <typename T, typename Alloc> struct v8_value_of<std::vector<T, Alloc>> {
    static v8::Local<v8::Array> to(v8::Isolate* iso, const std::vector<T, Alloc>& v) {
      return detail::range_to_array(iso, v);
    }
  };
  template <typename T, usize N> struct v8_value_of<std::array<T, N>> {
    static v8::Local<v8::Array> to(v8::Isolate* iso, const std::array<T, N>& a) {
      return detail::range_to_array(iso, a);
    }
  };
  // Raw C array `T[N]` (excluding the char[N] string-literal cases handled above).
  template <typename T, usize N>
    requires(!std::is_same_v<T, char> && !std::is_same_v<T, char16_t>)
  struct v8_value_of<T[N]> {
    static v8::Local<v8::Array> to(v8::Isolate* iso, const T (&a)[N]) {
      return detail::range_to_array(iso, std::span<const T, N>(a));
    }
  };

  // Object property setter -------------------------------------------------
  //
  // `key` and `value` both flow through `to_v8`, so any type with a
  // `v8_value_of<T>` specialization works on either side. In practice that
  // means string literals (`"foo"_v8`), `const char*` (internalized),
  // `std::string{,_view}`, raw `v8::Local<Name>` / `Local<String>` (which
  // pass through unchanged), and `v8::Global<Name>` / `Eternal<Name>` (auto
  // `.Get(iso)`) all dispatch through one signature. Same for the value
  // side: numbers, booleans, strings, optionals, vectors, raw `Local<T>`,
  // etc.
  template <typename K, typename V>
  inline void set_prop(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj, K&& key, V&& value) {
    auto* iso = v8::Isolate::GetCurrent();
    (void)obj->Set(ctx, to_v8(iso, std::forward<K>(key)), to_v8(iso, std::forward<V>(value)));
  }

  // Indexed setter for arrays.
  template <typename V>
  inline void set_index(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj, u32 index,
                        V&& value) {
    (void)obj->Set(ctx, index, to_v8(v8::Isolate::GetCurrent(), std::forward<V>(value)));
  }

  // `Object::DefineOwnProperty` — `set_prop` with explicit `PropertyAttribute`
  // bits. Use when you need `DontEnum` (hide a slot from `for…in` /
  // `Object.keys`), `ReadOnly`, or `DontDelete`. Default `None` makes this a
  // direct stand-in for `set_prop` when the attribute set differs from `Set`'s
  // (which always adds enumerable). Key flows through `to_v8`; `to_v8(key)` is
  // expected to produce a `Local<Name>`-compatible handle (string/symbol).
  template <typename K, typename V>
  inline void define_prop(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj, K&& key, V&& value,
                          v8::PropertyAttribute attrs = v8::None) {
    auto* iso = v8::Isolate::GetCurrent();
    (void)obj->DefineOwnProperty(ctx, to_v8(iso, std::forward<K>(key)),
                                 to_v8(iso, std::forward<V>(value)), attrs);
  }

  // Inverse conversions ----------------------------------------------------
  //
  // `from_v8<T>(ctx, value) -> std::optional<T>` is the strict mirror of
  // `to_v8`. It returns `nullopt` when `value` is the wrong shape (e.g.
  // asking for `int` on a non-numeric value, `std::string` on a non-String,
  // `vector<U>` on a non-Array). `optional<T>` and any `Local<T>` overload
  // treat `null`/`undefined` as a present-but-empty value where applicable.
  //
  // Customization point: specialize `v8_value_from<T>` with
  //
  //     template <> struct v8_value_from<my_handle> {
  //       static std::optional<my_handle> from(
  //           v8::Local<v8::Context> ctx, v8::Local<v8::Value> v);
  //     };
  //
  // Built-ins:
  //   - bool                        — strict (only `IsBoolean()`)
  //   - signed integral (≤ 32 bit)  — `Int32Value`,  Number-or-coercible required
  //   - unsigned integral (≤ 32 bit)— `Uint32Value`, Number-or-coercible required
  //   - i64 / u64                   — `IntegerValue` / cast (precision lost > 2^53)
  //   - f32 / f64                   — `NumberValue`
  //   - std::string                 — UTF-8 (`String::Utf8Value`)
  //   - std::u16string              — UTF-16 (`String::WriteV2`)
  //   - std::optional<T>            — null/undefined → empty, else `from_v8<T>`
  //   - std::vector<T>              — Array, each element through `from_v8<T>`
  //   - v8::Local<Value>            — passthrough (always succeeds)
  //   - v8::Local<Object|String|Number|Integer|Boolean|Array|Function|
  //                ArrayBuffer|Uint8Array|Map|Set|Promise|Date|RegExp|
  //                Symbol|BigInt|External>
  //                                 — strict, gated by the matching `IsX()`

  template <typename T> struct v8_value_from;

  template <typename T>
  inline std::optional<T> from_v8(v8::Local<v8::Context> ctx, v8::Local<v8::Value> value) {
    return v8_value_from<T>::from(ctx, value);
  }

  // Local<T> recovery -------------------------------------------------------

  template <> struct v8_value_from<v8::Local<v8::Value>> {
    static std::optional<v8::Local<v8::Value>> from(v8::Local<v8::Context>,
                                                    v8::Local<v8::Value> v) {
      return v;
    }
  };

#define FXE_V8_LOCAL_FROM(KIND)                                                                    \
  template <> struct v8_value_from<v8::Local<v8::KIND>> {                                          \
    static std::optional<v8::Local<v8::KIND>> from(v8::Local<v8::Context>,                         \
                                                   v8::Local<v8::Value> v) {                       \
      if (!v->Is##KIND())                                                                          \
        return std::nullopt;                                                                       \
      return v.As<v8::KIND>();                                                                     \
    }                                                                                              \
  }
  FXE_V8_LOCAL_FROM(Object);
  FXE_V8_LOCAL_FROM(String);
  FXE_V8_LOCAL_FROM(Number);
  FXE_V8_LOCAL_FROM(Boolean);
  FXE_V8_LOCAL_FROM(Array);
  FXE_V8_LOCAL_FROM(Function);
  FXE_V8_LOCAL_FROM(ArrayBuffer);
  FXE_V8_LOCAL_FROM(Uint8Array);
  FXE_V8_LOCAL_FROM(Map);
  FXE_V8_LOCAL_FROM(Set);
  FXE_V8_LOCAL_FROM(Promise);
  FXE_V8_LOCAL_FROM(Date);
  FXE_V8_LOCAL_FROM(RegExp);
  FXE_V8_LOCAL_FROM(Symbol);
  FXE_V8_LOCAL_FROM(BigInt);
  FXE_V8_LOCAL_FROM(External);
#undef FXE_V8_LOCAL_FROM

  // `IsInt32` is the predicate that pairs with `Local<Integer>`. (`IsInteger`
  // exists but matches any whole-valued Number; `IsInt32` is the canonical
  // "this slot really is an Integer" check.)
  template <> struct v8_value_from<v8::Local<v8::Integer>> {
    static std::optional<v8::Local<v8::Integer>> from(v8::Local<v8::Context>,
                                                      v8::Local<v8::Value> v) {
      if (!v->IsInt32() && !v->IsUint32())
        return std::nullopt;
      return v.As<v8::Integer>();
    }
  };

  // Booleans / numbers ------------------------------------------------------

  template <> struct v8_value_from<bool> {
    static std::optional<bool> from(v8::Local<v8::Context>, v8::Local<v8::Value> v) {
      if (!v->IsBoolean())
        return std::nullopt;
      return v.As<v8::Boolean>()->Value();
    }
  };

  template <typename T>
    requires std::is_integral_v<T> && std::is_signed_v<T> && (!std::is_same_v<T, bool>) &&
             (sizeof(T) <= 4)
  struct v8_value_from<T> {
    static std::optional<T> from(v8::Local<v8::Context> ctx, v8::Local<v8::Value> v) {
      if (!v->IsNumber())
        return std::nullopt;
      i32 out;
      if (!v->Int32Value(ctx).To(&out))
        return std::nullopt;
      return static_cast<T>(out);
    }
  };

  template <typename T>
    requires std::is_integral_v<T> && std::is_unsigned_v<T> && (!std::is_same_v<T, bool>) &&
             (sizeof(T) <= 4)
  struct v8_value_from<T> {
    static std::optional<T> from(v8::Local<v8::Context> ctx, v8::Local<v8::Value> v) {
      if (!v->IsNumber())
        return std::nullopt;
      u32 out;
      if (!v->Uint32Value(ctx).To(&out))
        return std::nullopt;
      return static_cast<T>(out);
    }
  };

  template <typename T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, bool>) && (sizeof(T) > 4)
  struct v8_value_from<T> {
    static std::optional<T> from(v8::Local<v8::Context> ctx, v8::Local<v8::Value> v) {
      if (!v->IsNumber())
        return std::nullopt;
      i64 out;
      if (!v->IntegerValue(ctx).To(&out))
        return std::nullopt;
      return static_cast<T>(out);
    }
  };

  template <typename T>
    requires std::is_floating_point_v<T>
  struct v8_value_from<T> {
    static std::optional<T> from(v8::Local<v8::Context> ctx, v8::Local<v8::Value> v) {
      if (!v->IsNumber())
        return std::nullopt;
      f64 out;
      if (!v->NumberValue(ctx).To(&out))
        return std::nullopt;
      return static_cast<T>(out);
    }
  };

  // Strings -----------------------------------------------------------------

  template <> struct v8_value_from<std::string> {
    static std::optional<std::string> from([[maybe_unused]] v8::Local<v8::Context> ctx,
                                           v8::Local<v8::Value> v) {
      if (!v->IsString())
        return std::nullopt;
      v8::String::Utf8Value u(v8::Isolate::GetCurrent(), v);
      if (*u == nullptr)
        return std::nullopt;
      return std::string(*u, static_cast<usize>(u.length()));
    }
  };

  template <> struct v8_value_from<std::u16string> {
    static std::optional<std::u16string> from([[maybe_unused]] v8::Local<v8::Context> ctx,
                                              v8::Local<v8::Value> v) {
      if (!v->IsString())
        return std::nullopt;
      static_assert(sizeof(char16_t) == sizeof(uint16_t));
      auto* iso = v8::Isolate::GetCurrent();
      auto s = v.As<v8::String>();
      const auto len = static_cast<u32>(s->Length());
      std::u16string out(len, u'\0');
      s->WriteV2(iso, 0, len, reinterpret_cast<uint16_t*>(out.data()));
      return out;
    }
  };

  // optional<T>: null/undefined → empty, otherwise recurse.
  template <typename T> struct v8_value_from<std::optional<T>> {
    static std::optional<std::optional<T>> from(v8::Local<v8::Context> ctx,
                                                v8::Local<v8::Value> v) {
      if (v->IsNullOrUndefined())
        return std::optional<T>{};
      auto inner = v8_value_from<T>::from(ctx, v);
      if (!inner.has_value())
        return std::nullopt;
      return std::optional<T>{std::move(*inner)};
    }
  };

  // vector<T>: Array → each element through from_v8<T>. Allocator defaulted
  // to std::allocator<T>; callers wanting a custom allocator should write the
  // loop themselves.
  template <typename T, typename Alloc> struct v8_value_from<std::vector<T, Alloc>> {
    static std::optional<std::vector<T, Alloc>> from(v8::Local<v8::Context> ctx,
                                                     v8::Local<v8::Value> v) {
      if (!v->IsArray())
        return std::nullopt;
      auto arr = v.As<v8::Array>();
      const auto n = arr->Length();
      std::vector<T, Alloc> out;
      out.reserve(n);
      for (u32 i = 0; i < n; ++i) {
        v8::Local<v8::Value> elem;
        if (!arr->Get(ctx, i).ToLocal(&elem))
          return std::nullopt;
        auto e = v8_value_from<T>::from(ctx, elem);
        if (!e.has_value())
          return std::nullopt;
        out.push_back(std::move(*e));
      }
      return out;
    }
  };

  // Object property reader -------------------------------------------------
  //
  // Returns `nullopt` when the property is missing, the read throws (caller
  // must handle the pending exception), or `from_v8<T>` rejects the shape.
  // For "did the slot exist at all?" use `obj->Has(ctx, key)` directly —
  // these helpers conflate "missing" with "wrong type" by design.

  // Key flows through `to_v8` so every type accepted by `set_prop`
  // (`"foo"_v8`, `const char*`, `string_view`, `Local<Name>`, `Global<Name>`,
  // …) is accepted here too.
  template <typename T = v8::Local<v8::Value>, typename K>
  inline std::optional<T> get_prop(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj, K&& key) {
    auto* iso = v8::Isolate::GetCurrent();
    v8::Local<v8::Value> v;
    if (!obj->Get(ctx, to_v8(iso, std::forward<K>(key))).ToLocal(&v))
      return std::nullopt;
    return from_v8<T>(ctx, v);
  }

  // `.value_or(fallback)` shorthand for `get_prop` — collapses every
  // `int_option` / `bool_option` / `string_option` / `object_*_prop` /
  // `get_optional_*` helper scattered across bindings into a single call.
  template <typename T, typename K, typename U>
  inline T get_prop_or(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj, K&& key,
                       U&& fallback) {
    return get_prop<T>(ctx, obj, std::forward<K>(key))
        .value_or(static_cast<T>(std::forward<U>(fallback)));
  }

  // Indexed reader, mirror of `set_index`.
  template <typename T = v8::Local<v8::Value>>
  inline std::optional<T> get_index(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj,
                                    u32 index) {
    v8::Local<v8::Value> v;
    if (!obj->Get(ctx, index).ToLocal(&v))
      return std::nullopt;
    return from_v8<T>(ctx, v);
  }

  // String extraction ------------------------------------------------------
  //
  // Default (`to_std_string`): loose — invokes `Utf8Value`, which calls
  // JS-side `ToString` on anything that isn't already a string. Matches the
  // legacy `utf8(...)` / `to_str(...)` helpers scattered through bindings.
  // Strict variant returns empty unless `v` is already a `v8::String`.

  inline std::string to_std_string(v8::Isolate* iso, v8::Local<v8::Value> v) {
    if (v.IsEmpty())
      return {};
    v8::String::Utf8Value u(iso, v);
    return *u ? std::string(*u, static_cast<usize>(u.length())) : std::string{};
  }

  inline std::string to_std_string_strict(v8::Isolate* iso, v8::Local<v8::Value> v) {
    if (v.IsEmpty() || !v->IsString())
      return {};
    return to_std_string(iso, v);
  }

  // Function-property installer. `name` accepts anything `set_prop` accepts
  // (`"x"_v8`, `const char*`, `string_view`, `Local<Name>`, …). Optional
  // `data` is forwarded to `Function::New` for callbacks that need a closure
  // pointer (typically a `make_external(iso, native)` handle). Returns the
  // installed function so callers can stash it in a `Global<Function>` for
  // later invocation if needed.
  template <typename K>
  inline v8::Local<v8::Function> add_function(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj,
                                              K&& name, v8::FunctionCallback cb,
                                              v8::Local<v8::Value> data = v8::Local<v8::Value>()) {
    auto fn = v8::Function::New(ctx, cb, data).ToLocalChecked();
    set_prop(ctx, obj, std::forward<K>(name), fn);
    return fn;
  }

  // Compile-time unqualified-name extraction for a function-pointer NTTP.
  // `xstd::const_tag<Fn>::to_string()` produces `&fxe::js::my_cb` (or the
  // MSVC equivalent); we strip the leading `&` and the namespace prefix so
  // V8 sees just `my_cb`. Returns a `string_view` that points at the
  // statically-stored buffer xstd allocates per NTTP.
  template <auto Fn> inline constexpr std::string_view function_name() {
    constexpr std::string_view full = xstd::const_tag<Fn>::to_string();
    constexpr std::string_view trimmed = full.starts_with('&') ? full.substr(1) : full;
    constexpr auto colon = trimmed.rfind("::");
    return colon == std::string_view::npos ? trimmed : trimmed.substr(colon + 2);
  }

  // Auto-named function installer. Uses the C++ identifier as the JS
  // property name — saves typing the string at every call site:
  //
  //     add_function<&power_inhibit_sleep>(ctx, power);    // → power.power_inhibit_sleep
  //
  // When the JS name should differ from the C++ identifier (camelCase vs
  // snake_case, override of an `__fxe_*` symbol, etc.), use the explicit
  // string overload above.
  template <auto Fn>
  inline v8::Local<v8::Function> add_function(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj,
                                              v8::Local<v8::Value> data = v8::Local<v8::Value>()) {
    return add_function(ctx, obj, function_name<Fn>(), Fn, data);
  }

} // namespace fxe::js
