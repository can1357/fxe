// Per-isolate internalized-string cache backing the `_v8` user-defined
// literals declared in <fxe/v8_literals.hpp>.

#include <fxe/v8_literals.hpp>
#include <unordered_map>

namespace fxe::js {

  namespace {

    // Slot 0 = host::impl, slot 1 = typescript context. This cache uses slot 2.
    constexpr u32 kIsolateSlotStringCache = 2;

    struct string_cache {
      // Key: pointer into the literal's static-storage payload. Identical
      // literals across TUs are not guaranteed to share an address; in that
      // case we end up with multiple cache entries pointing at the *same*
      // V8 internalized String (V8's string table dedupes content), which is
      // benign — at most O(num distinct addresses) wasted Eternal slots.
      std::unordered_map<const char*, v8::Eternal<v8::String>> map;
    };

  } // namespace

  void install_string_cache(v8::Isolate* iso) {
    iso->SetData(kIsolateSlotStringCache, new string_cache());
  }

  void uninstall_string_cache(v8::Isolate* iso) {
    auto* c = static_cast<string_cache*>(iso->GetData(kIsolateSlotStringCache));
    if (!c)
      return;
    // Eternal::Reset is not exposed; relying on destruction. The Eternal
    // slots are owned by V8's eternal handle table and are reclaimed when
    // the isolate is disposed. We just need to free our hashmap before that.
    delete c;
    iso->SetData(kIsolateSlotStringCache, nullptr);
  }

  v8::Local<v8::String> intern_literal(v8::Isolate* iso, v8_string_literal lit) {
    auto* c = static_cast<string_cache*>(iso->GetData(kIsolateSlotStringCache));
    // Defensive: if the cache is missing (e.g. literal evaluated before
    // install on a freshly-spun isolate), fall back to an uncached internalized
    // string so we never return an empty handle.
    if (!c) {
      return v8::String::NewFromUtf8(iso, lit.data, v8::NewStringType::kInternalized,
                                     static_cast<int>(lit.size))
          .ToLocalChecked();
    }
    auto& slot = c->map[lit.data];
    if (slot.IsEmpty()) {
      auto s = v8::String::NewFromUtf8(iso, lit.data, v8::NewStringType::kInternalized,
                                       static_cast<int>(lit.size))
                   .ToLocalChecked();
      slot.Set(iso, s);
    }
    return slot.Get(iso);
  }

} // namespace fxe::js
