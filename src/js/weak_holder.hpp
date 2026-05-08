// Lifetime helper for the `holder + Global<Object>* persistent` GC pattern
// used by most JS bindings.
//
// Holders that pair with `bind_weak_holder` MUST expose a public field:
//   v8::Global<v8::Object>* persistent = nullptr;
//
// `bind_weak_holder(iso, obj, h)`:
//   * heap-allocates a Global<Object> referencing `obj`,
//   * stashes the pointer in `h->persistent`,
//   * arms a weak callback that, on collection, runs the optional cleanup
//     hook, resets+deletes the persistent, and `delete`s `h`.
//
// For holders that need extra teardown (closing a native handle, removing
// from a registry, etc.) pass a function pointer as the second template
// argument:
//
//   static void rend_cleanup(rend_holder* h, v8::Isolate* iso) {
//     if (h->owned) unregister_renderer_for_isolate(iso, h->owned.get());
//   }
//   bind_weak_holder<rend_holder, &rend_cleanup>(iso, obj, h);
//
// The hook runs BEFORE the persistent is reset and BEFORE `h` is deleted,
// so it may freely access `h`'s members. Do not `delete h` from a hook —
// the helper owns that.

#pragma once

#include <v8.h>

namespace fxe::js {

  template <typename Holder> inline void weak_holder_no_cleanup(Holder*, v8::Isolate*) {}

  template <typename Holder,
            void (*Cleanup)(Holder*, v8::Isolate*) = &weak_holder_no_cleanup<Holder>>
  void weak_holder_finalizer(const v8::WeakCallbackInfo<Holder>& info) {
    auto* h = info.GetParameter();
    if (!h)
      return;
    Cleanup(h, info.GetIsolate());
    if (h->persistent) {
      h->persistent->Reset();
      delete h->persistent;
      h->persistent = nullptr;
    }
    delete h;
  }

  // Heap-allocate the Global<Object>, store it in `h->persistent`, and arm
  // the weak callback. Returns the persistent (rarely needed by callers).
  template <typename Holder,
            void (*Cleanup)(Holder*, v8::Isolate*) = &weak_holder_no_cleanup<Holder>>
  v8::Global<v8::Object>* bind_weak_holder(v8::Isolate* iso, v8::Local<v8::Object> obj, Holder* h) {
    auto* p = new v8::Global<v8::Object>(iso, obj);
    h->persistent = p;
    p->SetWeak(h, &weak_holder_finalizer<Holder, Cleanup>, v8::WeakCallbackType::kParameter);
    return p;
  }

} // namespace fxe::js
