// Per-isolate `Global<FunctionTemplate>` cache. Most `bind_*.cpp` files
// memoize one function template per isolate so wrapping a native object is
// cheap, and need to drop those Globals before isolate disposal so V8 doesn't
// trip on dangling handles. The boilerplate (a `tpl_table()` map, an
// `*_reset_for_isolate` function, a registrar struct, and a static instance
// thereof) was copy-pasted across ~10 bindings. This helper collapses all of
// that into a single tagged class:
//
//     namespace { struct cb_tag {}; }
//     using cb_tpl_cache = ::fxe::js::template_isolate_cache<cb_tag>;
//
//     auto& t = cb_tpl_cache::table();           // same map as before
//     // (no more `cb_reset_for_isolate` / `s_cb_resetter_register`)
//
// The first instantiation of each `Tag` registers a per-instantiation
// `reset_for_isolate` thunk via `register_template_resetter`. The host calls
// every registrar in `~host()` before isolate disposal.

#pragma once

#include <fxe/js_bindings.hpp>

#include <unordered_map>

#include <v8.h>

namespace fxe::js {

  template <typename Tag> struct template_isolate_cache {
    using table_type = std::unordered_map<v8::Isolate*, v8::Global<v8::FunctionTemplate>>;

    static table_type& table() {
      // ODR-use the registrar so its (inline) constructor runs and
      // `register_template_resetter` is called for this Tag exactly once.
      [[maybe_unused]] static auto* anchor = &registrar_;
      static table_type t;
      return t;
    }

    // Stash the per-isolate FunctionTemplate. Mirrors `table()[iso].Reset(iso, tpl)`
    // so install_*_template paths read as a single statement.
    static void install(v8::Isolate* iso, v8::Local<v8::FunctionTemplate> tpl) {
      table()[iso].Reset(iso, tpl);
    }

    // Recover the cached template for `iso`. Mirrors `table()[iso].Get(iso)`.
    static v8::Local<v8::FunctionTemplate> resolve(v8::Isolate* iso) {
      return table()[iso].Get(iso);
    }

    static void reset_for_isolate(v8::Isolate* iso) {
      auto& t = table();
      if (auto it = t.find(iso); it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }

    struct registrar {
      registrar() noexcept {
        register_template_resetter(&reset_for_isolate);
      }
    };
    inline static registrar registrar_{};
  };

} // namespace fxe::js
