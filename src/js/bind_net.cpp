#include "bind_net.hpp"

#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <array>
#include <string>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    MaybeLocal<Object> require_native_namespace(Isolate* iso, Local<Context> ctx, Local<String> key,
                                                const char* module_name, const char* export_name) {
      if (auto native_value = get_prop<Local<Value>>(ctx, ctx->Global(), "__fxe_native"_v8(iso));
          !native_value || !(*native_value)->IsObject()) {
        (void)throw_error(iso, std::string(module_name) + " requires __fxe_native");
        return MaybeLocal<Object>();
      } else if (auto namespace_value =
                     get_prop<Local<Value>>(ctx, (*native_value).As<Object>(), key);
                 !namespace_value || !(*namespace_value)->IsObject()) {
        (void)throw_error(iso, std::string(module_name) + " requires __fxe_native." + export_name);
        return MaybeLocal<Object>();
      } else {
        return (*namespace_value).As<Object>();
      }
    }

    MaybeLocal<Value> net_module_evaluate(Local<Context> ctx, Local<Module> mod) {
      auto* iso = Isolate::GetCurrent();
      HandleScope hs(iso);

      Local<Object> dns;
      if (!require_native_namespace(iso, ctx, "dns"_v8(iso), "fxe:net", "dns").ToLocal(&dns))
        return MaybeLocal<Value>();
      Local<Object> tcp;
      if (!require_native_namespace(iso, ctx, "net"_v8(iso), "fxe:net", "net").ToLocal(&tcp))
        return MaybeLocal<Value>();
      Local<Object> ipc;
      if (!require_native_namespace(iso, ctx, "ipcsock"_v8(iso), "fxe:net", "ipcsock")
               .ToLocal(&ipc))
        return MaybeLocal<Value>();
      Local<Object> udp;
      if (!require_native_namespace(iso, ctx, "dgram"_v8(iso), "fxe:net", "dgram").ToLocal(&udp))
        return MaybeLocal<Value>();

      auto set = [&](Local<String> name, Local<Value> value) -> bool {
        auto ok = mod->SetSyntheticModuleExport(iso, name, value);
        return ok.IsJust() && ok.FromJust();
      };
      if (!set("dns"_v8(iso), dns))
        return MaybeLocal<Value>();
      if (!set("tcp"_v8(iso), tcp))
        return MaybeLocal<Value>();
      if (!set("net"_v8(iso), tcp))
        return MaybeLocal<Value>();
      if (!set("ipc"_v8(iso), ipc))
        return MaybeLocal<Value>();
      if (!set("udp"_v8(iso), udp))
        return MaybeLocal<Value>();
      if (!set("dgram"_v8(iso), udp))
        return MaybeLocal<Value>();
      return Local<Value>(True(iso));
    }
  } // namespace

  MaybeLocal<Module> build_net_module(Isolate* iso, Local<Context> /*ctx*/) {
    HandleScope hs(iso);
    std::array<Local<String>, 6> exports{
        "dns"_v8(iso), "tcp"_v8(iso), "net"_v8(iso), "ipc"_v8(iso), "udp"_v8(iso), "dgram"_v8(iso),
    };
    MemorySpan<const Local<String>> span(exports.data(), exports.size());
    return Module::CreateSyntheticModule(iso, "fxe:net"_v8(iso), span, net_module_evaluate);
  }
} // namespace fxe::js
