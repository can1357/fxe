#include "bind_os.hpp"

#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <array>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    MaybeLocal<Object> require_os_namespace(Isolate* iso, Local<Context> ctx) {
      if (auto native_value = get_prop<Local<Value>>(ctx, ctx->Global(), "__fxe_native"_v8(iso));
          !native_value || !(*native_value)->IsObject()) {
        (void)throw_error(iso, "fxe:os requires __fxe_native");
        return MaybeLocal<Object>();
      } else if (auto os_value =
                     get_prop<Local<Value>>(ctx, (*native_value).As<Object>(), "os"_v8(iso));
                 !os_value || !(*os_value)->IsObject()) {
        (void)throw_error(iso, "fxe:os requires __fxe_native.os");
        return MaybeLocal<Object>();
      } else {
        return (*os_value).As<Object>();
      }
    }

    MaybeLocal<Value> os_module_evaluate(Local<Context> ctx, Local<Module> mod) {
      auto* iso = Isolate::GetCurrent();
      HandleScope hs(iso);

      Local<Object> os;
      if (!require_os_namespace(iso, ctx).ToLocal(&os))
        return MaybeLocal<Value>();

      auto export_fn = [&](Local<String> name) -> bool {
        auto value = get_prop<Local<Value>>(ctx, os, name);
        if (!value || !(*value)->IsFunction()) {
          (void)throw_error(iso, "fxe:os export is unavailable");
          return false;
        }
        auto ok = mod->SetSyntheticModuleExport(iso, name, *value);
        return ok.IsJust() && ok.FromJust();
      };

      for (Local<String> name : std::array<Local<String>, 14>{
               "platform"_v8(iso),
               "arch"_v8(iso),
               "release"_v8(iso),
               "type"_v8(iso),
               "endianness"_v8(iso),
               "homedir"_v8(iso),
               "tmpdir"_v8(iso),
               "hostname"_v8(iso),
               "uptime"_v8(iso),
               "totalmem"_v8(iso),
               "freemem"_v8(iso),
               "cpus"_v8(iso),
               "networkInterfaces"_v8(iso),
               "userInfo"_v8(iso),
           }) {
        if (!export_fn(name))
          return MaybeLocal<Value>();
      }
      if (!export_fn("installSystemChangeObserver"_v8(iso)))
        return MaybeLocal<Value>();
      return Local<Value>(True(iso));
    }
  } // namespace

  MaybeLocal<Module> build_os_module(Isolate* iso, Local<Context> /*ctx*/) {
    HandleScope hs(iso);
    std::array<Local<String>, 15> exports{
        "platform"_v8(iso),
        "arch"_v8(iso),
        "release"_v8(iso),
        "type"_v8(iso),
        "endianness"_v8(iso),
        "homedir"_v8(iso),
        "tmpdir"_v8(iso),
        "hostname"_v8(iso),
        "uptime"_v8(iso),
        "totalmem"_v8(iso),
        "freemem"_v8(iso),
        "cpus"_v8(iso),
        "networkInterfaces"_v8(iso),
        "userInfo"_v8(iso),
        "installSystemChangeObserver"_v8(iso),
    };
    MemorySpan<const Local<String>> span(exports.data(), exports.size());
    return Module::CreateSyntheticModule(iso, "fxe:os"_v8(iso), span, os_module_evaluate);
  }
} // namespace fxe::js
