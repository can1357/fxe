# V8 Binding Helpers (`<fxe/v8_helpers.hpp>`)

Bindings **MUST** route through these instead of hand-rolled equivalents. The header is the single source of truth for every "convert / throw / wrap / set / get" V8 idiom. Adding a local `utf8(...)` / `to_str(...)` / `s(iso, ...)` / `set_fn(...)` / `int_option(...)` / `throw_msg(...)` helper to a binding is rejected on review — extend `v8_helpers.hpp` (or a `v8_value_of<T>` specialisation) instead.

## External / native wrapping

| Signature | Purpose |
|---|---|
| `make_external(iso, ptr) -> Local<External>` | Wrap any `T*` into an `External` with the default external-pointer tag. |
| `external_ptr<T>(value\|ext\|data) -> T*` | Recover `T*` from a `Local<Value>` / `Local<External>` / `Local<Data>` (no checks). |
| `internal_ptr<T>(obj, slot=0) -> T*` | Recover `T*` from `obj->GetInternalField(slot)`; caller validates the slot/tag. |
| `set_native(iso, obj, ptr, tag) -> void` | Assign internal field 0 = `External(ptr)`, field 1 = `Uint32(tag)` in one call. |

## Throwers (return `false`; format-string overloads accept `std::format_string<…>` + args)

| Signature | Purpose |
|---|---|
| `throw_error(iso, msg \| fmt, args…) -> bool` | Throw `new Error(msg)`; always returns `false`. |
| `throw_type_error(iso, msg \| fmt, args…) -> bool` | Throw `new TypeError(msg)`; always returns `false`. |
| `throw_range_error(iso, msg \| fmt, args…) -> bool` | Throw `new RangeError(msg)`; always returns `false`. |
| `throw_coded_error(iso, ctx, code, msg \| fmt, args…) -> void` | Throw `Error` with `err.code = code` (Node-style `{code, …}`). |
| `throw_named(iso, ctx, name, msg \| fmt, args…) -> void` | Throw `Error` with `err.name = name` (DOM-style `AbortError`, `NoHandler`, …). |
| `throw_exception(iso, ctx, code, name, msg \| fmt, args…) -> void` | Throw `Error` with both `code` and `name` set. |

## Primitives

| Signature | Purpose |
|---|---|
| `to_v8_undefined(iso) -> Local<Primitive>` | `v8::Undefined(iso)`. |
| `to_v8_null(iso) -> Local<Primitive>` | `v8::Null(iso)`. |

## C++ → V8 (`to_v8`)

Single entry point dispatched via `v8_value_of<T>`.

| Signature | Purpose |
|---|---|
| `to_v8(iso, value) -> Local<…>` | Convert any `T` with a `v8_value_of<T>` specialisation; result type is whatever the specialisation returns. |
| `template<> struct v8_value_of<T>` | Customisation point — define `static Local<X> to(Isolate*, T)` to plug a new C++ type into `to_v8`, `set_prop`, `set_index`, `add_function`, container element conversion, and `optional` wrapping. |
| `to_v8_string(iso, string_view) -> Local<String>` | UTF-8 `String::NewFromUtf8` with `kNormal` for dynamic strings. |
| `to_v8_string(iso, u16string_view) -> Local<String>` | UTF-16 `String::NewFromTwoByte` for `std::u16string{,_view}` / `char16_t*`. |
| `to_v8_string_internalized(iso, const char*, length=-1) -> Local<String>` | UTF-8 with `kInternalized`; `const char(&)[N]` overload bakes in `N - 1` so literals skip the strlen. |

Built-in `v8_value_of` specialisations (use directly via `to_v8` — do not duplicate):

- `bool` → `Boolean`
- signed integral ≤ 32-bit → `Integer` (`Integer::New`)
- unsigned integral ≤ 32-bit → `Integer` (`Integer::NewFromUnsigned`)
- `i64` / `u64` → `Number` (precision lost above 2^53; reach for `BigInt::New{,FromUnsigned}` directly when 64-bit precision is required)
- `f32` / `f64` → `Number`
- `nullptr_t`, `nullopt_t` → `Null`
- `std::optional<T>` → `Null` when empty, else `to_v8(*opt)`
- `std::string`, `std::string_view`, `char*` → `String` (`kNormal`)
- `const char*`, `char[N]` → `String` (`kInternalized`; pure win for literals)
- `std::u16string{,_view}`, `const char16_t*`, `char16_t[N]` → `String` (UTF-16)
- `std::span<T>` / `std::vector<T>` / `std::array<T,N>` / raw `T[N]` → `Array` (each element re-enters `to_v8`)
- `v8::Local<T>` → passthrough
- `v8::Global<T>`, `v8::Eternal<T>` → auto `.Get(iso)`
- `"foo"_v8`, `42_v8`, `1.5_v8` (literals from `<fxe/v8_literals.hpp>`) → cached `Eternal` lookup

## V8 → C++ (`from_v8`)

Strict mirror of `to_v8`, returns `std::optional<T>`.

| Signature | Purpose |
|---|---|
| `from_v8<T>(ctx, value) -> optional<T>` | Convert a `Local<Value>` to `T` via `v8_value_from<T>`; returns `nullopt` when shape is wrong. |
| `template<> struct v8_value_from<T>` | Customisation point — define `static optional<T> from(Local<Context>, Local<Value>)`. |
| `to_std_string(iso, v) -> string` | **Lossy.** Runs `Utf8Value` (calls JS `ToString` on non-strings); empty on failure. Replaces every per-TU `utf8()` / `to_str()` / `string_arg()` helper. |
| `to_std_string_strict(iso, v) -> string` | Returns empty unless `v->IsString()`; use when you reject coercion. |

Built-in `v8_value_from` specialisations:

- `bool` (strict, `IsBoolean()` only)
- signed integral ≤ 32-bit (`Int32Value`)
- unsigned integral ≤ 32-bit (`Uint32Value`)
- `i64` / `u64` (`IntegerValue`, cast)
- `f32` / `f64` (`NumberValue`)
- `std::string` (UTF-8 via `Utf8Value`)
- `std::u16string` (UTF-16 via `WriteV2`)
- `std::optional<T>` (null/undefined → empty, else recurse)
- `std::vector<T>` (Array; each element re-enters `from_v8`)
- `Local<Value>` (passthrough)
- `Local<Object|String|Number|Integer|Boolean|Array|Function|ArrayBuffer|Uint8Array|Map|Set|Promise|Date|RegExp|Symbol|BigInt|External>` (strict, gated by the matching `IsX()`)

## Property I/O

Keys flow through `to_v8`, so `"foo"_v8`, `const char*`, `string_view`, `Local<Name>`, `Global<Name>` all work.

| Signature | Purpose |
|---|---|
| `set_prop(ctx, obj, key, value)` | `obj->Set(ctx, to_v8(key), to_v8(value))`. Replaces every raw `obj->Set(ctx, "X"_v8(iso), …)`. |
| `set_index(ctx, obj, u32, value)` | Indexed `obj->Set(ctx, idx, to_v8(value))`. |
| `define_prop(ctx, obj, key, value, attrs=None)` | `Object::DefineOwnProperty` — `set_prop` with `PropertyAttribute` bits (`v8::DontEnum`, `v8::ReadOnly`, `v8::DontDelete`). Use for `__fxe_native`-style hidden slots and read-only constants. |
| `get_prop<T>(ctx, obj, key) -> optional<T>` | `Get` + `from_v8<T>`; `nullopt` on missing or wrong shape. |
| `get_prop_or<T>(ctx, obj, key, fallback) -> T` | `get_prop<T>(...).value_or(fallback)`. Replaces every `int_option` / `bool_option` / `string_option` / `object_*_prop` / `get_optional_*` helper. |
| `get_index<T>(ctx, obj, u32) -> optional<T>` | Indexed mirror of `get_prop`. |

## Function installers

| Signature | Purpose |
|---|---|
| `add_function(ctx, obj, name, cb, data={}) -> Local<Function>` | `Function::New(ctx, cb, data).ToLocalChecked()` + `set_prop(ctx, obj, name, fn)` in one call. |
| `add_function<&Fn>(ctx, obj, data={}) -> Local<Function>` | Auto-named: derives the JS property name from the C++ identifier of `&Fn` via `xstd::const_tag` (e.g. `&power_inhibit_sleep` → `"power_inhibit_sleep"`). Use the string overload above when JS name should differ. |
| `function_name<&Fn>() -> string_view` | Compile-time unqualified name of `Fn` (strips `&` + namespace prefix); building block for `add_function<&Fn>`. |

## Idioms enforced on review

- `get_prop` / `get_index` default `T = v8::Local<v8::Value>`, so `if (auto v = get_prop(ctx, obj, "k"_v8))` works without angle brackets when you just want the raw value.
- **Inside helpers (`set_prop`, `get_prop`, `set_index`, `get_index`, `add_function`), drop `(iso)` from `_v8` literals.** The literal types (`v8_string_literal`, `v8_integer_literal`, `v8_float_literal`) have `v8_value_of` specialisations, so the helper's `to_v8` call materialises the cached `Local<String>` itself. Write `set_prop(ctx, obj, "k"_v8, v)`, **not** `set_prop(ctx, obj, "k"_v8(iso), v)`. The `(iso)` form is still required at the rare raw-V8 boundary (`Exception::Error("msg"_v8(iso))`, `iso->ThrowError("..."_v8(iso))`, `obj->StringEquals("k"_v8(iso))` when `Isolate::GetCurrent()` isn't valid).
- Use `to_v8(iso, x)` for any value crossing into V8 — bindings should never call `Integer::New` / `Number::New` / `Boolean::New` / `String::NewFromUtf8` directly. The only legitimate exception is `BigInt::New{,FromUnsigned}` for exact 64-bit values.
- Argument coercion at call entry (`info[i]->Int32Value(ctx).FromMaybe(0)`) stays raw — it's intentionally lax JS semantics. Option-bag reads (`obj.foo`) go through `get_prop_or<T>`.
- Never declare a local `utf8 / to_str / to_string / string_arg / utf8_arg / s / str / js_string / s8 / set_fn / add_native_fn / int_option / bool_option / string_option / get_optional_string / get_optional_bool / int_prop / bool_prop / string_prop / object_*_prop / throw_msg / throw_js_error / throw_native_error` helper. Each of these has a header equivalent above; if behaviour genuinely differs, extend the header.
