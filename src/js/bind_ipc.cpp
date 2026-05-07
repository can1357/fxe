// Match brew V8 ABI: pointer compression + sandbox-off in libv8.dylib.
#define V8_COMPRESS_POINTERS 1

// Same-isolate IPC bindings implementing the `fxe:ipc` synthetic ES module.

#include "bind_ipc.hpp"

#include <fxe/js_bindings.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    using FnGlobal = Global<Function>;
    using ValueGlobal = Global<Value>;
    using ContextGlobal = Global<Context>;
    using ResolverGlobal = Global<Promise::Resolver>;
    using TplGlobal = Global<FunctionTemplate>;

    Local<String> s(Isolate* iso, std::string_view sv) {
      return String::NewFromUtf8(iso, sv.data(), NewStringType::kNormal,
                                 static_cast<int>(sv.size()))
          .ToLocalChecked();
    }

    Local<String> s(Isolate* iso, const char* z) {
      return String::NewFromUtf8(iso, z ? z : "", NewStringType::kNormal).ToLocalChecked();
    }

    std::string to_string(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      if (*utf8)
        return std::string(*utf8, utf8.length());
      return {};
    }

    bool require_channel(Isolate* iso, const FunctionCallbackInfo<Value>& info, int index,
                         const char* signature, std::string& out) {
      if (info.Length() <= index || !info[index]->IsString()) {
        iso->ThrowException(Exception::TypeError(s(iso, signature)));
        return false;
      }
      out = to_string(iso, info[index]);
      return true;
    }

    Local<Value> type_error(Isolate* iso, const char* message) {
      return Exception::TypeError(s(iso, message));
    }

    Local<Value> no_handler_error(Isolate* iso, Local<Context> ctx, std::string_view channel) {
      std::string msg = "No handler registered for IPC channel '";
      msg += channel;
      msg += "'";
      auto err = Exception::Error(s(iso, msg)).As<Object>();
      (void)err->Set(ctx, s(iso, "name"), s(iso, "NoHandler"));
      return err;
    }

    Local<Promise> rejected([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> err) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Reject(ctx, err);
      return resolver->GetPromise();
    }

    struct invoke_task {
      ContextGlobal ctx;
      ResolverGlobal resolver;
      FnGlobal handler;
      ValueGlobal payload;

      void reset() {
        ctx.Reset();
        resolver.Reset();
        handler.Reset();
        payload.Reset();
      }
    };

    struct disposer_data {
      std::string channel;
      FnGlobal listener;
      bool disposed = false;

      void reset() {
        listener.Reset();
      }
    };

    struct ipc_state {
      std::unordered_map<std::string, FnGlobal> handlers;
      std::unordered_map<std::string, std::vector<FnGlobal>> listeners;
      std::vector<std::unique_ptr<invoke_task>> invoke_tasks;
      std::vector<std::unique_ptr<disposer_data>> disposers;
    };

    struct ipc_templates {
      TplGlobal handle;
      TplGlobal remove_handler;
      TplGlobal invoke;
      TplGlobal on;
      TplGlobal off;
      TplGlobal remove_all_listeners;
      TplGlobal send;
      TplGlobal debug;
    };

    std::unordered_map<Isolate*, ipc_state>& ipc_states() {
      static std::unordered_map<Isolate*, ipc_state> states;
      return states;
    }

    std::unordered_map<Isolate*, ipc_templates>& ipc_template_table() {
      static std::unordered_map<Isolate*, ipc_templates> table;
      return table;
    }

    ipc_state& state_for(Isolate* iso) {
      return ipc_states()[iso];
    }

    void reset_state(ipc_state& st) {
      for (auto& [_, fn] : st.handlers)
        fn.Reset();
      st.handlers.clear();
      for (auto& [_, list] : st.listeners) {
        for (auto& fn : list)
          fn.Reset();
      }
      st.listeners.clear();
      for (auto& task : st.invoke_tasks) {
        if (task)
          task->reset();
      }
      st.invoke_tasks.clear();
      for (auto& disposer : st.disposers) {
        if (disposer)
          disposer->reset();
      }
      st.disposers.clear();
    }

    bool remove_listener(Isolate* iso, ipc_state& st, std::string_view channel,
                         Local<Function> listener) {
      auto it = st.listeners.find(std::string(channel));
      if (it == st.listeners.end())
        return false;
      auto& list = it->second;
      auto found = std::find_if(list.begin(), list.end(), [&](FnGlobal& candidate) {
        auto current = candidate.Get(iso);
        return current->StrictEquals(listener);
      });
      if (found == list.end())
        return false;
      found->Reset();
      list.erase(found);
      if (list.empty())
        st.listeners.erase(it);
      return true;
    }

    void log_listener_error(Isolate* iso, Local<Context> ctx, Local<Value> err) {
      TryCatch tc(iso);
      Local<Value> console_value;
      if (!ctx->Global()->Get(ctx, s(iso, "console")).ToLocal(&console_value) ||
          !console_value->IsObject())
        return;
      auto console_obj = console_value.As<Object>();
      Local<Value> error_value;
      if (!console_obj->Get(ctx, s(iso, "error")).ToLocal(&error_value) ||
          !error_value->IsFunction())
        return;
      Local<Value> argv[2] = {s(iso, "fxe:ipc listener error"), err};
      Local<Value> ignored;
      (void)error_value.As<Function>()->Call(ctx, console_obj, 2, argv).ToLocal(&ignored);
    }

    void ipc_handle(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      std::string channel;
      if (!require_channel(iso, info, 0, "ipc.handle(channel, fn)", channel))
        return;
      if (info.Length() < 2 || !info[1]->IsFunction()) {
        iso->ThrowException(Exception::TypeError(s(iso, "ipc.handle(channel, fn)")));
        return;
      }
      auto& handlers = state_for(iso).handlers;
      auto it = handlers.find(channel);
      if (it != handlers.end()) {
        it->second.Reset();
        it->second.Reset(iso, info[1].As<Function>());
      } else {
        handlers.emplace(std::move(channel), FnGlobal(iso, info[1].As<Function>()));
      }
    }

    void ipc_remove_handler(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      std::string channel;
      if (!require_channel(iso, info, 0, "ipc.removeHandler(channel)", channel))
        return;
      auto& handlers = state_for(iso).handlers;
      auto it = handlers.find(channel);
      if (it == handlers.end()) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      it->second.Reset();
      handlers.erase(it);
      info.GetReturnValue().Set(True(iso));
    }

    void invoke_task_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (!info.Data()->IsExternal())
        return;
      auto* task = static_cast<invoke_task*>(
          info.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
      if (!task)
        return;

      auto ctx = task->ctx.Get(iso);
      Context::Scope cs(ctx);
      auto resolver = task->resolver.Get(iso);
      auto handler = task->handler.Get(iso);
      auto payload = task->payload.Get(iso);

      TryCatch tc(iso);
      Local<Value> result;
      Local<Value> argv[1] = {payload};
      if (!handler->Call(ctx, Undefined(iso), 1, argv).ToLocal(&result)) {
        Local<Value> err =
            tc.HasCaught() ? tc.Exception() : Exception::Error(s(iso, "ipc handler failed"));
        tc.Reset();
        (void)resolver->Reject(ctx, err);
      } else {
        (void)resolver->Resolve(ctx, result);
      }

      task->reset();
      auto& tasks = state_for(iso).invoke_tasks;
      auto it = std::find_if(tasks.begin(), tasks.end(),
                             [&](const auto& ptr) { return ptr.get() == task; });
      if (it != tasks.end())
        tasks.erase(it);
    }

    void ipc_invoke(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      std::string channel;
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, type_error(iso, "ipc.invoke(channel, payload)")));
        return;
      }
      channel = to_string(iso, info[0]);
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());

      auto& st = state_for(iso);
      auto it = st.handlers.find(channel);
      if (it == st.handlers.end()) {
        (void)resolver->Reject(ctx, no_handler_error(iso, ctx, channel));
        return;
      }

      auto task = std::make_unique<invoke_task>();
      task->ctx.Reset(iso, ctx);
      task->resolver.Reset(iso, resolver);
      task->handler.Reset(iso, it->second.Get(iso));
      task->payload.Reset(iso, info.Length() > 1 ? info[1] : Local<Value>(Undefined(iso)));
      auto* raw = task.get();
      st.invoke_tasks.push_back(std::move(task));
      auto data = External::New(iso, raw, v8::kExternalPointerTypeTagDefault);
      Local<Function> microtask;
      if (!Function::New(ctx, invoke_task_cb, data).ToLocal(&microtask)) {
        raw->reset();
        st.invoke_tasks.pop_back();
        (void)resolver->Reject(ctx, Exception::Error(s(iso, "ipc invoke scheduling failed")));
        return;
      }
      iso->EnqueueMicrotask(microtask);
    }

    void disposer_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (!info.Data()->IsExternal())
        return;
      auto* data = static_cast<disposer_data*>(
          info.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
      if (!data || data->disposed) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      auto listener = data->listener.Get(iso);
      bool removed = remove_listener(iso, state_for(iso), data->channel, listener);
      data->reset();
      data->disposed = true;
      info.GetReturnValue().Set(Boolean::New(iso, removed));
    }

    void ipc_on(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      std::string channel;
      if (!require_channel(iso, info, 0, "ipc.on(channel, listener)", channel))
        return;
      if (info.Length() < 2 || !info[1]->IsFunction()) {
        iso->ThrowException(Exception::TypeError(s(iso, "ipc.on(channel, listener)")));
        return;
      }
      auto listener = info[1].As<Function>();
      auto& st = state_for(iso);
      st.listeners[channel].emplace_back(iso, listener);

      auto disposer = std::make_unique<disposer_data>();
      disposer->channel = channel;
      disposer->listener.Reset(iso, listener);
      auto* raw = disposer.get();
      st.disposers.push_back(std::move(disposer));
      auto data = External::New(iso, raw, v8::kExternalPointerTypeTagDefault);
      Local<Function> fn;
      if (!Function::New(ctx, disposer_cb, data).ToLocal(&fn)) {
        raw->reset();
        raw->disposed = true;
        iso->ThrowException(Exception::Error(s(iso, "ipc disposer creation failed")));
        return;
      }
      info.GetReturnValue().Set(fn);
    }

    void ipc_off(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      std::string channel;
      if (!require_channel(iso, info, 0, "ipc.off(channel, listener)", channel))
        return;
      if (info.Length() < 2 || !info[1]->IsFunction()) {
        iso->ThrowException(Exception::TypeError(s(iso, "ipc.off(channel, listener)")));
        return;
      }
      (void)remove_listener(iso, state_for(iso), channel, info[1].As<Function>());
    }

    void ipc_remove_all_listeners(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto& listeners = state_for(iso).listeners;
      if (info.Length() == 0 || info[0]->IsUndefined()) {
        for (auto& [_, list] : listeners) {
          for (auto& fn : list)
            fn.Reset();
        }
        listeners.clear();
        return;
      }
      if (!info[0]->IsString()) {
        iso->ThrowException(Exception::TypeError(s(iso, "ipc.removeAllListeners(channel?)")));
        return;
      }
      std::string channel = to_string(iso, info[0]);
      auto it = listeners.find(channel);
      if (it == listeners.end())
        return;
      for (auto& fn : it->second)
        fn.Reset();
      listeners.erase(it);
    }

    void ipc_send(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      std::string channel;
      if (!require_channel(iso, info, 0, "ipc.send(channel, payload)", channel))
        return;
      // v1 routing is same-isolate only. Cross-isolate delivery to worker_threads
      // is intentionally deferred; keep the public surface stable for that router.
      auto& st = state_for(iso);
      auto it = st.listeners.find(channel);
      if (it == st.listeners.end())
        return;
      std::vector<Local<Function>> snapshot;
      snapshot.reserve(it->second.size());
      for (auto& fn : it->second)
        snapshot.push_back(fn.Get(iso));
      Local<Value> argv[1] = {info.Length() > 1 ? info[1] : Local<Value>(Undefined(iso))};
      for (auto fn : snapshot) {
        TryCatch tc(iso);
        Local<Value> ignored;
        if (!fn->Call(ctx, Undefined(iso), 1, argv).ToLocal(&ignored) || tc.HasCaught()) {
          Local<Value> err =
              tc.HasCaught() ? tc.Exception() : Exception::Error(s(iso, "ipc listener failed"));
          tc.Reset();
          log_listener_error(iso, ctx, err);
        }
      }
    }

    void ipc_debug(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto out = Object::New(iso);
      auto& st = state_for(iso);
      for (auto& [channel, handler] : st.handlers) {
        auto entry = Object::New(iso);
        (void)entry->Set(ctx, s(iso, "handler"), Boolean::New(iso, !handler.IsEmpty()));
        auto listeners_it = st.listeners.find(channel);
        int count =
            listeners_it == st.listeners.end() ? 0 : static_cast<int>(listeners_it->second.size());
        (void)entry->Set(ctx, s(iso, "listeners"), Integer::New(iso, count));
        (void)out->Set(ctx, s(iso, channel), entry);
      }
      for (auto& [channel, list] : st.listeners) {
        if (st.handlers.find(channel) != st.handlers.end())
          continue;
        auto entry = Object::New(iso);
        (void)entry->Set(ctx, s(iso, "handler"), False(iso));
        (void)entry->Set(ctx, s(iso, "listeners"),
                         Integer::New(iso, static_cast<int>(list.size())));
        (void)out->Set(ctx, s(iso, channel), entry);
      }
      info.GetReturnValue().Set(out);
    }

    TplGlobal& ensure_tpl(Isolate* iso, TplGlobal& slot, FunctionCallback cb) {
      if (slot.IsEmpty())
        slot.Reset(iso, FunctionTemplate::New(iso, cb));
      return slot;
    }

    ipc_templates& ensure_templates(Isolate* iso) {
      auto& t = ipc_template_table()[iso];
      ensure_tpl(iso, t.handle, ipc_handle);
      ensure_tpl(iso, t.remove_handler, ipc_remove_handler);
      ensure_tpl(iso, t.invoke, ipc_invoke);
      ensure_tpl(iso, t.on, ipc_on);
      ensure_tpl(iso, t.off, ipc_off);
      ensure_tpl(iso, t.remove_all_listeners, ipc_remove_all_listeners);
      ensure_tpl(iso, t.send, ipc_send);
      ensure_tpl(iso, t.debug, ipc_debug);
      return t;
    }

    void ipc_reset_for_isolate(Isolate* iso) {
      if (auto it = ipc_template_table().find(iso); it != ipc_template_table().end()) {
        it->second.handle.Reset();
        it->second.remove_handler.Reset();
        it->second.invoke.Reset();
        it->second.on.Reset();
        it->second.off.Reset();
        it->second.remove_all_listeners.Reset();
        it->second.send.Reset();
        it->second.debug.Reset();
        ipc_template_table().erase(it);
      }
      if (auto it = ipc_states().find(iso); it != ipc_states().end()) {
        reset_state(it->second);
        ipc_states().erase(it);
      }
    }

    struct ipc_resetter_register {
      ipc_resetter_register() {
        register_template_resetter(&ipc_reset_for_isolate);
      }
    };
    static ipc_resetter_register s_ipc_resetter_register;

    MaybeLocal<Value> ipc_module_evaluate(Local<Context> ctx, Local<Module> mod) {
      auto* iso = Isolate::GetCurrent();
      HandleScope hs(iso);
      auto& t = ensure_templates(iso);

      auto set_fn = [&](const char* name, TplGlobal& tpl) -> bool {
        Local<Function> fn;
        if (!tpl.Get(iso)->GetFunction(ctx).ToLocal(&fn))
          return false;
        auto ok = mod->SetSyntheticModuleExport(iso, s(iso, name), fn);
        return ok.IsJust() && ok.FromJust();
      };

      if (!set_fn("handle", t.handle))
        return MaybeLocal<Value>();
      if (!set_fn("removeHandler", t.remove_handler))
        return MaybeLocal<Value>();
      if (!set_fn("invoke", t.invoke))
        return MaybeLocal<Value>();
      if (!set_fn("on", t.on))
        return MaybeLocal<Value>();
      if (!set_fn("off", t.off))
        return MaybeLocal<Value>();
      if (!set_fn("removeAllListeners", t.remove_all_listeners))
        return MaybeLocal<Value>();
      if (!set_fn("send", t.send))
        return MaybeLocal<Value>();
      if (!set_fn("debug", t.debug))
        return MaybeLocal<Value>();
      return Local<Value>(True(iso));
    }
  } // namespace

  void install_ipc_bindings(Isolate* iso, Local<ObjectTemplate> /*global*/) {
    HandleScope hs(iso);
    (void)ensure_templates(iso);
  }

  MaybeLocal<Module> build_ipc_module(Isolate* iso, Local<Context> /*ctx*/) {
    HandleScope hs(iso);
    std::array<Local<String>, 8> exports{
        String::NewFromUtf8Literal(iso, "handle"),
        String::NewFromUtf8Literal(iso, "removeHandler"),
        String::NewFromUtf8Literal(iso, "invoke"),
        String::NewFromUtf8Literal(iso, "on"),
        String::NewFromUtf8Literal(iso, "off"),
        String::NewFromUtf8Literal(iso, "removeAllListeners"),
        String::NewFromUtf8Literal(iso, "send"),
        String::NewFromUtf8Literal(iso, "debug"),
    };
    MemorySpan<const Local<String>> span(exports.data(), exports.size());
    auto module_name = String::NewFromUtf8Literal(iso, "fxe:ipc");
    return Module::CreateSyntheticModule(iso, module_name, span, ipc_module_evaluate);
  }
} // namespace fxe::js
