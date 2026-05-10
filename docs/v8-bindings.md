# V8 Bindings — Literals, Weak Callbacks, Template Caches

## V8 literals (`<fxe/v8_literals.hpp>`)

Use the user-defined literals for values passed across the V8 boundary:

```cpp
obj->Get(ctx, "width"_v8(iso));
obj->Set(ctx, "name"_v8(iso), value);
obj->Set(ctx, "fd"_v8(iso), 0_v8(iso));
```

String literals return a per-isolate internalized `v8::Local<v8::String>` cached in an `Eternal` slot, so repeated calls are a hash lookup, not a fresh `String::NewFromUtf8`. Don't write `String::NewFromUtf8(iso, "width").ToLocalChecked()` in bindings — it's slower and noisier.

The cache is installed/uninstalled per isolate via `install_string_cache` / `uninstall_string_cache`; new isolates spun up outside the host must call `install_string_cache` before any binding code runs (a missing cache falls back to an uncached internalized string, never an empty handle).

### String equality

Compare a dynamic `v8::Local<v8::String>` to a literal with `s == "flex"_v8` (also `"flex"_v8 == s`). Do not reintroduce `s->StringEquals("flex"_v8(iso))` for routine checks — the `operator==` / `operator!=` in `<fxe/v8_literals.hpp>` materialises the same cached internalized literal via `v8::Isolate::GetCurrent()` and delegates to `StringEquals`. If `GetCurrent()` is not valid for the call site (not on the entered isolate), use the explicit `s->StringEquals("…"_v8(iso))` form instead.

## Per-isolate template caches (`<fxe/v8_template_cache.hpp>`)

Bindings that memoize a `Global<FunctionTemplate>` per isolate **MUST** use `fxe::js::template_isolate_cache<Tag>` instead of hand-rolling the `xxx_tpl_table()` map / `xxx_reset_for_isolate` thunk / `xxx_resetter_register` + static instance quartet. Each `Tag` instantiation auto-registers its resetter on first table use, so isolate teardown stays correct without per-binding boilerplate.

```cpp
// bind_foo.cpp
namespace {
  struct foo_tag {};
  using foo_tpl_cache = template_isolate_cache<foo_tag>;
}

void install_foo_template(Isolate* iso, Local<ObjectTemplate> global) {
  auto tpl = FunctionTemplate::New(iso, foo_ctor);
  /* … configure tpl … */
  global->Set(iso, "Foo", tpl);
  foo_tpl_cache::install(iso, tpl);          // stash for this isolate
}

Local<Object> make_foo_object(Isolate* iso, Local<Context> ctx, foo* native) {
  return wrap(iso, ctx, foo_tpl_cache::resolve(iso), native, TAG_FOO);
}
```

Use `::install(iso, tpl)` and `::resolve(iso)` for the common write/read paths; reach for `::table()` only when you need the underlying `unordered_map` (rare — diagnostic dumps, multi-template fetches).

## V8 weak callbacks (`Global<T>::SetWeak`)

V8 requires the first-pass weak callback to either `Reset()` the persistent that triggered it or call `SetSecondPassCallback()`. Doing neither aborts the process with `Handle not reset in first callback` during the next GC. The repo convention: store the persistent on the holder so the finalizer can reset and free it.

```cpp
struct foo_holder {
  /* … real fields … */
  v8::Global<v8::Object>* persistent = nullptr;
};

void foo_finalizer(const v8::WeakCallbackInfo<foo_holder>& info) {
  auto* h = info.GetParameter();
  if (h && h->persistent) {
    h->persistent->Reset();
    delete h->persistent;
  }
  delete h;
}

// wrap site
auto* persistent = new v8::Global<v8::Object>(iso, obj);
h->persistent = persistent;
persistent->SetWeak(h, foo_finalizer, v8::WeakCallbackType::kParameter);
```

Equivalent alternative used in a few bindings: store `v8::Global<v8::Object> self;` directly on the holder, call `h->self.SetWeak(h, finalizer, kParameter)`, and `h->self.Reset()` in the finalizer. Either pattern is fine; never `delete info.GetParameter()` on its own without resetting the persistent.
