// CRTP base for the "wrapped C++ object + weak GC callback" pattern most JS
// bindings use. A holder that inherits from `weak_holder<Self>` gains:
//
//   * a `persistent` field (heap-owned `v8::Global<v8::Object>*`),
//   * `bind(iso, obj)` which heap-allocates the persistent and arms the
//     weak callback so the holder is freed when `obj` is collected,
//   * automatic teardown: on collection the persistent is reset+deleted
//     and the holder itself is `delete`d.
//
// Derived MAY define `void on_finalize(v8::Isolate*)`. When present, the
// hook runs BEFORE the persistent is reset and BEFORE the holder is
// deleted, so it can freely touch derived state (close handles, remove
// from registries, etc.). Do NOT `delete this` from the hook — the base
// owns that.
//
//   struct sheet_holder : fxe::js::weak_holder<sheet_holder> {
//     spritesheet sheet;
//   };
//
//   auto* h = new sheet_holder{};
//   obj->SetInternalField(0, External::New(iso, h, ...));
//   obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_FOO));
//   h->bind(iso, obj);

#pragma once

#include <v8.h>

namespace fxe::js {

  template <typename Derived> struct weak_holder {
    v8::Global<v8::Object>* persistent = nullptr;

    void bind(v8::Isolate* iso, v8::Local<v8::Object> obj) {
      auto* p = new v8::Global<v8::Object>(iso, obj);
      persistent = p;
      p->SetWeak(static_cast<Derived*>(this), &weak_holder::finalize,
                 v8::WeakCallbackType::kParameter);
    }

  private:
    static void finalize(const v8::WeakCallbackInfo<Derived>& info) {
      auto* h = info.GetParameter();
      if (!h)
        return;
      if constexpr (requires(Derived* d, v8::Isolate* i) { d->on_finalize(i); }) {
        h->on_finalize(info.GetIsolate());
      }
      if (h->persistent) {
        h->persistent->Reset();
        delete h->persistent;
        h->persistent = nullptr;
      }
      delete h;
    }
  };

} // namespace fxe::js
