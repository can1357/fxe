#include "async_hooks.hpp"

#include <cstdint>
#include <fxe/types.hpp>
#include <fxe/v8_literals.hpp>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fxe::runtime {
  namespace {
    using namespace v8;

    constexpr u32 k_isolate_slot_async_hooks = 3;

    struct async_hooks_state {
      Global<Object> current_resource;
      std::vector<Global<Object>> resource_stack;
      std::unordered_map<int, Global<Object>> promise_resources;
      // v1: keyed by identity hash and cleaned on kAfter. Unobserved promises can
      // linger until isolate teardown; hash collisions overwrite older entries.
      // RTLUfunctions.edit კომენტary to=functions.edit code  天天中彩票不? Let's see.
      std::mutex mutex;
      uint64_t next_async_id = 1;
    };

    async_hooks_state* get_state(Isolate* iso) {
      return static_cast<async_hooks_state*>(iso->GetData(k_isolate_slot_async_hooks));
    }

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      FunctionCallback cb) {
      auto fn = Function::New(ctx, cb).ToLocalChecked();
      (void)obj->Set(ctx, str(iso, name), fn);
    }

    Local<Symbol> async_id_symbol(Isolate* iso) {
      return Symbol::For(iso, "fxe.asyncId"_v8(iso));
    }

    Local<Symbol> trigger_async_id_symbol(Isolate* iso) {
      return Symbol::For(iso, "fxe.triggerAsyncId"_v8(iso));
    }

    Local<Object> fresh_resource(Isolate* iso) {
      return Object::New(iso);
    }

    Local<Object> current_resource_or_fresh(Isolate* iso, async_hooks_state* state) {
      if (state && !state->current_resource.IsEmpty())
        return state->current_resource.Get(iso);
      return fresh_resource(iso);
    }

    Local<Object> ensure_current_resource(Isolate* iso, async_hooks_state* state) {
      auto resource = current_resource_or_fresh(iso, state);
      if (state && state->current_resource.IsEmpty())
        state->current_resource.Reset(iso, resource);
      return resource;
    }

    Local<Object> lookup_promise_resource(Isolate* iso, async_hooks_state* state,
                                          Local<Promise> promise) {
      if (!state)
        return {};
      auto it = state->promise_resources.find(promise->GetIdentityHash());
      if (it == state->promise_resources.end() || it->second.IsEmpty())
        return {};
      return it->second.Get(iso);
    }

    uint64_t read_resource_async_number(Isolate* iso, Local<Object> resource, Local<Symbol> key) {
      if (resource.IsEmpty())
        return 0;
      auto ctx = iso->GetCurrentContext();
      if (ctx.IsEmpty())
        return 0;
      Local<Value> value;
      if (!resource->Get(ctx, key).ToLocal(&value) || !value->IsNumber())
        return 0;
      return static_cast<uint64_t>(value->IntegerValue(ctx).FromMaybe(0));
    }

    void get_current_resource_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_state(iso);
      std::lock_guard<std::mutex> lk(state->mutex);
      info.GetReturnValue().Set(ensure_current_resource(iso, state));
    }

    void set_current_resource_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_state(iso);
      if (info.Length() < 1 || !info[0]->IsObject()) {
        iso->ThrowException(Exception::TypeError(
            "__fxe_native.async_hooks.setCurrentResource requires an object"_v8(iso)));
        return;
      }
      auto next = info[0].As<Object>();
      std::lock_guard<std::mutex> lk(state->mutex);
      auto prev = ensure_current_resource(iso, state);
      state->current_resource.Reset(iso, next);
      info.GetReturnValue().Set(prev);
    }

    void execution_async_id_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_state(iso);
      std::lock_guard<std::mutex> lk(state->mutex);
      auto resource = current_resource_or_fresh(iso, state);
      const auto id = read_resource_async_number(iso, resource, async_id_symbol(iso));
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(id)));
    }

    void trigger_async_id_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_state(iso);
      std::lock_guard<std::mutex> lk(state->mutex);
      auto resource = current_resource_or_fresh(iso, state);
      const auto id = read_resource_async_number(iso, resource, trigger_async_id_symbol(iso));
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(id)));
    }

    void next_async_id_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_state(iso);
      std::lock_guard<std::mutex> lk(state->mutex);
      const auto id = state->next_async_id++;
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(id)));
    }

    void promise_hook(PromiseHookType type, Local<Promise> promise, Local<Value> parent) {
      auto* iso = Isolate::GetCurrent();
      auto* state = iso ? get_state(iso) : nullptr;
      if (!iso || !state)
        return;
      HandleScope handle_scope(iso);
      std::lock_guard<std::mutex> lk(state->mutex);
      switch (type) {
      case PromiseHookType::kInit: {
        Local<Object> resource;
        if (!parent.IsEmpty() && parent->IsPromise())
          resource = lookup_promise_resource(iso, state, parent.As<Promise>());
        if (resource.IsEmpty())
          resource = ensure_current_resource(iso, state);
        auto& slot = state->promise_resources[promise->GetIdentityHash()];
        slot.Reset(iso, resource);
        break;
      }
      case PromiseHookType::kBefore: {
        auto previous = ensure_current_resource(iso, state);
        state->resource_stack.emplace_back(iso, previous);
        auto next = lookup_promise_resource(iso, state, promise);
        if (next.IsEmpty())
          next = previous;
        state->current_resource.Reset(iso, next);
        break;
      }
      case PromiseHookType::kAfter: {
        if (!state->resource_stack.empty()) {
          auto previous = state->resource_stack.back().Get(iso);
          state->resource_stack.pop_back();
          state->current_resource.Reset(iso, previous);
        }
        state->promise_resources.erase(promise->GetIdentityHash());
        break;
      }
      case PromiseHookType::kResolve:
        break;
      default:
        break;
      }
    }

    Local<Object> make_async_hooks_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "getCurrentResource", get_current_resource_callback);
      add_function(iso, ctx, ns, "setCurrentResource", set_current_resource_callback);
      add_function(iso, ctx, ns, "executionAsyncId", execution_async_id_callback);
      add_function(iso, ctx, ns, "triggerAsyncId", trigger_async_id_callback);
      add_function(iso, ctx, ns, "nextAsyncId", next_async_id_callback);
      return ns;
    }
  } // namespace

  void install_native_async_hooks(Isolate* iso, Local<Context> ctx) {
    auto* state = get_state(iso);
    if (!state) {
      state = new async_hooks_state();
      state->current_resource.Reset(iso, Object::New(iso));
      iso->SetData(k_isolate_slot_async_hooks, state);
      iso->SetPromiseHook(promise_hook);
    }

    Local<Value> native_value;
    Local<Object> native;
    if (ctx->Global()->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) &&
        native_value->IsObject()) {
      native = native_value.As<Object>();
    } else {
      native = Object::New(iso);
      (void)ctx->Global()->DefineOwnProperty(ctx, "__fxe_native"_v8(iso), native,
                                             static_cast<PropertyAttribute>(DontEnum));
    }
    (void)native->Set(ctx, "async_hooks"_v8(iso), make_async_hooks_namespace(iso, ctx));
  }

  void uninstall_native_async_hooks(Isolate* iso) {
    auto* state = get_state(iso);
    if (!state)
      return;
    iso->SetPromiseHook(nullptr);
    state->current_resource.Reset();
    state->resource_stack.clear();
    state->promise_resources.clear();
    delete state;
    iso->SetData(k_isolate_slot_async_hooks, nullptr);
  }
} // namespace fxe::runtime
