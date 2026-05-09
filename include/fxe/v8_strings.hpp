#pragma once

// Per-isolate internalized-string cache for `_v8` user-defined literals.
//
// Background: every call to `v8::String::NewFromUtf8Literal(iso, "x")` allocates
// a fresh v8::String. For property keys reused on every frame / every option
// decode (e.g. "frameCount" in the audio callback, "width"/"height" in
// offscreen options), this is wasted work. This header lets you write
//
//     obj->Get(ctx, "frameCount"_v8(iso))
//
// and string equality as
//
//     if (s == "flex"_v8)
//
// (`operator==` uses `v8::Isolate::GetCurrent()`, materialises the cached
// internalized literal, then `v8::String::StringEquals`.)
//
// The first hit interns the string and stores a
// v8::Eternal<v8::String> in a per-isolate hashmap keyed by the literal's
// `const char*` address. Subsequent hits resolve to a single pointer load and
// V8 fast-paths property lookups by internalized-string identity.
//
// Lifecycle:
//   - install_string_cache(iso) MUST be called once per isolate, before any
//     `_v8` literal is materialised. v8_host wires this in host::impl::impl().
//   - uninstall_string_cache(iso) MUST be called inside an Isolate::Scope +
//     HandleScope before the isolate is disposed (Eternal needs the isolate
//     alive to release its slot).

#include <fxe/types.hpp>
#include <type_traits>
#include <v8.h>

namespace fxe::js {

  // Returns a per-isolate cached, internalized v8::String for `lit`. Allocates
  // (and interns into V8's string table) on first use; O(1) hashmap lookup +
  // Eternal::Get on subsequent calls.
  v8::Local<v8::String> intern_literal(v8::Isolate* iso, v8_string_literal lit);

  // Allocates the per-isolate cache and installs it on the isolate's data
  // slot. Must run before any `_v8` literal is invoked on this isolate.
  void install_string_cache(v8::Isolate* iso);

  // Releases every Eternal in the cache and frees the backing store. Must run
  // before isolate->Dispose() and inside an Isolate::Scope + HandleScope.
  void uninstall_string_cache(v8::Isolate* iso);

} // namespace fxe::js

template <typename Iso> auto v8_string_literal::operator()(Iso* iso) const {
  static_assert(std::is_same_v<Iso, v8::Isolate>,
                "_v8 literal must be invoked with a v8::Isolate*");
  return ::fxe::js::intern_literal(iso, *this);
}

inline bool operator==(v8::Local<v8::String> a, v8_string_literal lit) {
  auto* iso = v8::Isolate::GetCurrent();
  if (a.IsEmpty() || iso == nullptr)
    return false;
  return a->StringEquals(lit(iso));
}

inline bool operator!=(v8::Local<v8::String> a, v8_string_literal lit) {
  return !(a == lit);
}

inline bool operator==(v8_string_literal lit, v8::Local<v8::String> a) {
  return a == lit;
}

inline bool operator!=(v8_string_literal lit, v8::Local<v8::String> a) {
  return !(a == lit);
}
