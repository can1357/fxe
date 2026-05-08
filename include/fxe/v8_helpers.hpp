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

#include <format>
#include <string>
#include <string_view>
#include <utility>

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

} // namespace fxe::js
