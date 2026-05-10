#include "vm.hpp"

#include "../../../js/weak_holder.hpp"
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::runtime {
  namespace {
    using namespace v8;

    using namespace fxe::js;
    constexpr uint32_t kContextTag = 0x564d4358; // VMCX
    constexpr uint32_t kScriptTag = 0x564d5343;  // VMSC

    struct script_options {
      std::string filename = "evalmachine.<anonymous>";
      int line_offset = 0;
      int column_offset = 0;
    };

    struct script_state : fxe::js::weak_holder<script_state> {
      Global<UnboundScript> script;

      void on_finalize(Isolate*) {
        script.Reset();
      }
    };

    struct context_state {
      Global<Context> context;
    };

    void context_state_finalize(const WeakCallbackInfo<context_state>& info) {
      auto* state = info.GetParameter();
      if (!state)
        return;
      state->context.Reset();
      delete state;
    }

    bool is_tagged_object(Local<Value> value, uint32_t tag) {
      if (value.IsEmpty() || !value->IsObject())
        return false;
      auto obj = value.As<Object>();
      if (obj->InternalFieldCount() < 2)
        return false;
      auto tag_value = obj->GetInternalField(1).As<Value>();
      return tag_value->IsUint32() && tag_value.As<Uint32>()->Value() == tag;
    }

    script_state* script_state_from(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 2)
        return nullptr;
      auto field = obj->GetInternalField(0).As<Value>();
      if (!field->IsExternal())
        return nullptr;
      return fxe::js::external_ptr<script_state>(field);
    }

    context_state* context_state_from(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 2)
        return nullptr;
      auto field = obj->GetInternalField(0).As<Value>();
      if (!field->IsExternal())
        return nullptr;
      return fxe::js::external_ptr<context_state>(field);
    }

    Maybe<bool> validate_runtime_opts(Isolate* iso, Local<Context> ctx, Local<Value> opts_value) {
      if (opts_value.IsEmpty() || opts_value->IsUndefined() || opts_value->IsNull())
        return Just(true);
      if (!opts_value->IsObject()) {
        throw_type_error(iso, "vm options must be an object");
        return Nothing<bool>();
      }
      auto opts = opts_value.As<Object>();
      if (auto microtask_mode = get_prop<Local<Value>>(ctx, opts, "microtaskMode")) {
        if (!(*microtask_mode)->IsUndefined() && !(*microtask_mode)->IsNull()) {
          const std::string mode = to_std_string(iso, *microtask_mode);
          if (mode == "afterEvaluate") {
            throw_error(iso, "node:vm microtaskMode 'afterEvaluate' is not supported in FXE");
            return Nothing<bool>();
          }
        }
      }
      return Just(true);
    }

    Maybe<bool> parse_script_options(Isolate* iso, Local<Context> ctx, Local<Value> opts_value,
                                     script_options& out) {
      if (!validate_runtime_opts(iso, ctx, opts_value).FromMaybe(false))
        return Nothing<bool>();
      if (opts_value.IsEmpty() || opts_value->IsUndefined() || opts_value->IsNull())
        return Just(true);
      auto opts = opts_value.As<Object>();

      if (auto filename_value = get_prop<Local<Value>>(ctx, opts, "filename")) {
        if (!(*filename_value)->IsUndefined() && !(*filename_value)->IsNull())
          out.filename = to_std_string(iso, *filename_value);
      }

      if (auto line_value = get_prop<Local<Value>>(ctx, opts, "lineOffset")) {
        if (!(*line_value)->IsUndefined() && !(*line_value)->IsNull())
          out.line_offset = (*line_value)->Int32Value(ctx).FromMaybe(0);
      }

      if (auto column_value = get_prop<Local<Value>>(ctx, opts, "columnOffset")) {
        if (!(*column_value)->IsUndefined() && !(*column_value)->IsNull())
          out.column_offset = (*column_value)->Int32Value(ctx).FromMaybe(0);
      }

      return Just(true);
    }

    ScriptOrigin make_origin(Isolate* iso, const script_options& opts) {
      return ScriptOrigin(to_v8_string(iso, opts.filename), opts.line_offset, opts.column_offset,
                          /*is_shared_cross_origin*/ false, /*script_id*/ -1, Local<Value>());
    }

    Maybe<bool> property_is_enumerable(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                       Local<Value> key) {
      Context::Scope scope(ctx);
      if (!key->IsName())
        return Just(false);
      Local<Value> desc_value;
      if (!obj->GetOwnPropertyDescriptor(ctx, key.As<Name>()).ToLocal(&desc_value))
        return Nothing<bool>();
      if (desc_value.IsEmpty() || !desc_value->IsObject())
        return Just(false);
      auto desc = desc_value.As<Object>();
      auto enumerable = get_prop<Local<Value>>(ctx, desc, "enumerable");
      if (!enumerable.has_value())
        return Nothing<bool>();
      return Just((*enumerable)->BooleanValue(iso));
    }

    struct prop_entry {
      Local<Value> key;
      Local<Value> value;
    };

    Maybe<bool> collect_enumerable_props(Isolate* iso, Local<Context> source_ctx,
                                         Local<Object> from, std::vector<prop_entry>& out) {
      Context::Scope scope(source_ctx);
      Local<Array> names;
      if (!from->GetOwnPropertyNames(source_ctx).ToLocal(&names))
        return Nothing<bool>();
      for (uint32_t i = 0; i < names->Length(); ++i) {
        Local<Value> key;
        if (!names->Get(source_ctx, i).ToLocal(&key))
          return Nothing<bool>();
        auto enumerable = property_is_enumerable(iso, source_ctx, from, key);
        if (enumerable.IsNothing())
          return Nothing<bool>();
        if (!enumerable.FromJust())
          continue;
        Local<Value> value;
        if (!from->Get(source_ctx, key).ToLocal(&value))
          return Nothing<bool>();
        out.push_back(prop_entry{key, value});
      }
      return Just(true);
    }

    Maybe<bool> apply_props(Isolate* iso, Local<Context> target_ctx, Local<Object> to,
                            const std::vector<prop_entry>& props, bool skip_native_binding) {
      Context::Scope scope(target_ctx);
      for (const auto& prop : props) {
        if (skip_native_binding && prop.key->IsString() &&
            to_std_string(iso, prop.key) == "__fxe_native")
          continue;
        if (!to->Set(target_ctx, prop.key, prop.value).FromMaybe(false))
          return Nothing<bool>();
      }
      return Just(true);
    }

    Maybe<bool> copy_enumerable_props(Isolate* iso, Local<Context> source_ctx, Local<Object> from,
                                      Local<Context> target_ctx, Local<Object> to,
                                      bool skip_native_binding = false) {
      std::vector<prop_entry> props;
      if (!collect_enumerable_props(iso, source_ctx, from, props).FromMaybe(false))
        return Nothing<bool>();
      return apply_props(iso, target_ctx, to, props, skip_native_binding);
    }

    MaybeLocal<Context> context_from_value(Isolate* iso, Local<Value> value) {
      if (!is_tagged_object(value, kContextTag)) {
        throw_type_error(iso, "vm context must be created by vm.createContext()");
        return {};
      }
      auto state = context_state_from(value.As<Object>());
      if (!state || state->context.IsEmpty()) {
        throw_error(iso, "vm context has been disposed");
        return {};
      }
      return state->context.Get(iso);
    }

    MaybeLocal<Script> compile_script(Isolate* iso, Local<Context> ctx, Local<String> code,
                                      const script_options& opts) {
      auto origin = make_origin(iso, opts);
      ScriptCompiler::Source source(code, origin);
      return ScriptCompiler::Compile(ctx, &source);
    }

    MaybeLocal<UnboundScript> compile_unbound(Isolate* iso, Local<String> code,
                                              const script_options& opts) {
      auto origin = make_origin(iso, opts);
      ScriptCompiler::Source source(code, origin);
      return ScriptCompiler::CompileUnboundScript(iso, &source);
    }

    MaybeLocal<Value> run_unbound(Local<Context> target_ctx, Local<UnboundScript> script) {
      Context::Scope scope(target_ctx);
      Local<Script> bound = script->BindToCurrentContext();
      return bound->Run(target_ctx);
    }

    MaybeLocal<Value> run_string_in_context(Local<Context> target_ctx, Local<String> code,
                                            const script_options& opts) {
      Context::Scope scope(target_ctx);
      Local<Script> script;
      if (!compile_script(Isolate::GetCurrent(), target_ctx, code, opts).ToLocal(&script))
        return {};
      return script->Run(target_ctx);
    }

    MaybeLocal<Value> run_in_new_context_impl(Isolate* iso, Local<Context> current_ctx,
                                              Local<String> code, Local<Object> sandbox,
                                              const script_options& opts) {
      auto global_tpl = ObjectTemplate::New(iso);
      global_tpl->SetInternalFieldCount(2);
      auto new_ctx = Context::New(iso, nullptr, global_tpl);
      Context::Scope new_scope(new_ctx);
      auto global = new_ctx->Global();
      if (!copy_enumerable_props(iso, current_ctx, sandbox, new_ctx, global).FromMaybe(false))
        return {};
      new_ctx->SetSecurityToken(sandbox);
      Local<Value> result;
      if (!run_string_in_context(new_ctx, code, opts).ToLocal(&result))
        return {};
      if (!copy_enumerable_props(iso, new_ctx, global, current_ctx, sandbox, true).FromMaybe(false))
        return {};
      return result;
    }

    void script_run_in_this_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto current_ctx = iso->GetCurrentContext();
      auto* state = script_state_from(info.This());
      if (!state || state->script.IsEmpty()) {
        throw_error(iso, "compiled vm.Script has been released");
        return;
      }
      if (!validate_runtime_opts(iso, current_ctx, info.Length() > 0 ? info[0] : Local<Value>())
               .FromMaybe(false))
        return;
      Local<Value> result;
      if (!run_unbound(current_ctx, state->script.Get(iso)).ToLocal(&result))
        return;
      info.GetReturnValue().Set(result);
    }

    void script_run_in_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto current_ctx = iso->GetCurrentContext();
      auto* state = script_state_from(info.This());
      if (!state || state->script.IsEmpty()) {
        throw_error(iso, "compiled vm.Script has been released");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsObject()) {
        throw_type_error(iso,
                         "Script.runInContext(contextifiedObject[, options]) requires a context");
        return;
      }
      if (!validate_runtime_opts(iso, current_ctx, info.Length() > 1 ? info[1] : Local<Value>())
               .FromMaybe(false))
        return;
      Local<Context> target_ctx;
      if (!context_from_value(iso, info[0]).ToLocal(&target_ctx))
        return;
      Local<Value> result;
      if (!run_unbound(target_ctx, state->script.Get(iso)).ToLocal(&result))
        return;
      info.GetReturnValue().Set(result);
    }

    void script_run_in_new_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto current_ctx = iso->GetCurrentContext();
      auto* state = script_state_from(info.This());
      if (!state || state->script.IsEmpty()) {
        throw_error(iso, "compiled vm.Script has been released");
        return;
      }
      Local<Object> sandbox = Object::New(iso);
      if (info.Length() > 0 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (!info[0]->IsObject()) {
          throw_type_error(
              iso, "Script.runInNewContext([sandbox[, options]]) sandbox must be an object");
          return;
        }
        sandbox = info[0].As<Object>();
      }
      if (!validate_runtime_opts(iso, current_ctx, info.Length() > 1 ? info[1] : Local<Value>())
               .FromMaybe(false))
        return;
      Local<Value> result;
      auto global_tpl = ObjectTemplate::New(iso);
      global_tpl->SetInternalFieldCount(2);
      auto new_ctx = Context::New(iso, nullptr, global_tpl);
      Context::Scope new_scope(new_ctx);
      auto global = new_ctx->Global();
      if (!copy_enumerable_props(iso, current_ctx, sandbox, new_ctx, global).FromMaybe(false))
        return;
      new_ctx->SetSecurityToken(sandbox);
      if (!run_unbound(new_ctx, state->script.Get(iso)).ToLocal(&result))
        return;
      if (!copy_enumerable_props(iso, new_ctx, global, current_ctx, sandbox, true).FromMaybe(false))
        return;
      info.GetReturnValue().Set(result);
    }

    MaybeLocal<Object> make_compiled_script_object(Isolate* iso, Local<Context> ctx,
                                                   Local<UnboundScript> script) {
      auto tpl = ObjectTemplate::New(iso);
      tpl->SetInternalFieldCount(2);
      auto holder = tpl->NewInstance(ctx).ToLocalChecked();
      auto* state = new script_state();
      state->script.Reset(iso, script);
      set_native(iso, holder, state, kScriptTag);
      state->bind(iso, holder);
      add_function(ctx, holder, "runInThisContext", script_run_in_this_context);
      add_function(ctx, holder, "runInContext", script_run_in_context);
      add_function(ctx, holder, "runInNewContext", script_run_in_new_context);
      return holder;
    }

    MaybeLocal<Object> make_context_object(Isolate* iso, Local<Context> ctx, Local<Object> seed) {
      auto global_tpl = ObjectTemplate::New(iso);
      global_tpl->SetInternalFieldCount(2);
      auto sandbox_ctx = Context::New(iso, nullptr, global_tpl);
      sandbox_ctx->SetSecurityToken(ctx->GetSecurityToken());
      Context::Scope sandbox_scope(sandbox_ctx);
      auto global = sandbox_ctx->Global();
      if (!copy_enumerable_props(iso, ctx, seed, sandbox_ctx, global).FromMaybe(false))
        return {};
      auto* state = new context_state();
      state->context.Reset(iso, sandbox_ctx);
      state->context.SetWeak(state, context_state_finalize, WeakCallbackType::kParameter);
      set_native(iso, global, state, kContextTag);
      return global;
    }

    void native_run_in_this_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_type_error(iso, "__fxe_native.vm.runInThisContext(code[, options]) requires code");
        return;
      }
      script_options opts;
      if (!parse_script_options(iso, ctx, info.Length() > 1 ? info[1] : Local<Value>(), opts)
               .FromMaybe(false))
        return;
      Local<Value> result;
      if (!run_string_in_context(ctx, info[0].As<String>(), opts).ToLocal(&result))
        return;
      info.GetReturnValue().Set(result);
    }

    void native_run_in_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsObject() || !info[1]->IsString()) {
        throw_type_error(
            iso,
            "__fxe_native.vm.runInContext(context, code[, options]) requires context and code");
        return;
      }
      script_options opts;
      if (!parse_script_options(iso, ctx, info.Length() > 2 ? info[2] : Local<Value>(), opts)
               .FromMaybe(false))
        return;
      Local<Context> target_ctx;
      if (!context_from_value(iso, info[0]).ToLocal(&target_ctx))
        return;
      Local<Value> result;
      if (!run_string_in_context(target_ctx, info[1].As<String>(), opts).ToLocal(&result))
        return;
      info.GetReturnValue().Set(result);
    }

    void native_run_in_new_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_type_error(
            iso, "__fxe_native.vm.runInNewContext(code[, sandbox[, options]]) requires code");
        return;
      }
      Local<Object> sandbox = Object::New(iso);
      if (info.Length() > 1 && !info[1]->IsUndefined() && !info[1]->IsNull()) {
        if (!info[1]->IsObject()) {
          throw_type_error(iso, "vm sandbox must be an object");
          return;
        }
        sandbox = info[1].As<Object>();
      }
      script_options opts;
      if (!parse_script_options(iso, ctx, info.Length() > 2 ? info[2] : Local<Value>(), opts)
               .FromMaybe(false))
        return;
      Local<Value> result;
      if (!run_in_new_context_impl(iso, ctx, info[0].As<String>(), sandbox, opts).ToLocal(&result))
        return;
      info.GetReturnValue().Set(result);
    }

    void native_compile(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_type_error(iso, "__fxe_native.vm.compile(code[, options]) requires code");
        return;
      }
      script_options opts;
      if (!parse_script_options(iso, ctx, info.Length() > 1 ? info[1] : Local<Value>(), opts)
               .FromMaybe(false))
        return;
      Local<UnboundScript> script;
      if (!compile_unbound(iso, info[0].As<String>(), opts).ToLocal(&script))
        return;
      Local<Object> compiled;
      if (!make_compiled_script_object(iso, ctx, script).ToLocal(&compiled))
        return;
      info.GetReturnValue().Set(compiled);
    }

    void native_create_context(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() > 0 && is_tagged_object(info[0], kContextTag)) {
        info.GetReturnValue().Set(info[0]);
        return;
      }
      Local<Object> seed = Object::New(iso);
      if (info.Length() > 0 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (!info[0]->IsObject()) {
          throw_type_error(iso,
                           "__fxe_native.vm.createContext([sandbox]) sandbox must be an object");
          return;
        }
        seed = info[0].As<Object>();
      }
      Local<Object> context_object;
      if (!make_context_object(iso, ctx, seed).ToLocal(&context_object))
        return;
      info.GetReturnValue().Set(context_object);
    }

    void native_is_context(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Boolean::New(
          info.GetIsolate(), info.Length() > 0 && is_tagged_object(info[0], kContextTag)));
    }

    void native_compile_function(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsArray()) {
        throw_type_error(
            iso,
            "__fxe_native.vm.compileFunction(code, params[, options]) requires code and params");
        return;
      }
      script_options opts;
      if (!parse_script_options(iso, ctx, info.Length() > 2 ? info[2] : Local<Value>(), opts)
               .FromMaybe(false))
        return;
      auto params = info[1].As<Array>();
      std::string wrapped = "(function(";
      for (uint32_t i = 0; i < params->Length(); ++i) {
        if (i)
          wrapped.push_back(',');
        Local<Value> param;
        if (!params->Get(ctx, i).ToLocal(&param))
          return;
        wrapped += to_std_string(iso, param);
      }
      wrapped += "){ ";
      wrapped += to_std_string(iso, info[0]);
      wrapped += " })";
      Local<Value> result;
      if (!run_string_in_context(ctx, to_v8_string(iso, wrapped), opts).ToLocal(&result))
        return;
      info.GetReturnValue().Set(result);
    }

    Local<Object> make_vm_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(ctx, ns, "runInThisContext", native_run_in_this_context);
      add_function(ctx, ns, "runInContext", native_run_in_context);
      add_function(ctx, ns, "runInNewContext", native_run_in_new_context);
      add_function(ctx, ns, "compile", native_compile);
      add_function(ctx, ns, "createContext", native_create_context);
      add_function(ctx, ns, "isContext", native_is_context);
      add_function(ctx, ns, "compileFunction", native_compile_function);
      return ns;
    }
  } // namespace

  void install_native_vm(Isolate* iso, Local<Context> ctx) {
    Local<Value> native_value;
    Local<Object> native;
    if (ctx->Global()->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) &&
        native_value->IsObject()) {
      native = native_value.As<Object>();
    } else {
      native = Object::New(iso);
      define_prop(ctx, ctx->Global(), "__fxe_native"_v8, native,
                  static_cast<PropertyAttribute>(DontEnum));
    }
    set_prop(ctx, native, "vm", make_vm_namespace(iso, ctx));
  }
} // namespace fxe::runtime
