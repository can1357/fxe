// Node builtin compatibility status:
// - Host-adapted: assert/strict, async_hooks (limited), buffer, child_process, console,
//   crypto, dgram, dns, dns/promises, events, fs, fs/promises, http2, https, inspector,
//   inspector/promises, net, os, path, path/posix, path/win32, process, querystring,
//   readline, readline/promises, repl, stream, stream/promises, timers,
//   timers/promises, tls, tty, url, util, util/types, v8, vm, wasi, worker_threads, zlib.
// - unenv fallback: assert, cluster, constants, diagnostics_channel, domain, http,
//   module, perf_hooks, punycode, stream/consumers, stream/web, string_decoder, sys,
//   test, test/reporters, trace_events, _http_agent, _http_client, _http_common,
//   _http_incoming, _http_outgoing, _http_server, _stream_duplex, _stream_passthrough,
//   _stream_readable, _stream_transform, _stream_wrap, _stream_writable.
// - Deferred on unenv until native parity work lands: none.

#include "node_compat.hpp"
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
#include "native/http2.hpp"
#include "native/https.hpp"
#include "native/tls.hpp"
#endif

#ifdef FXE_ENABLE_NODE_COMPAT
#include <fxe/generated/unenv_assets.hpp>
#endif
#include <algorithm>
#include <array>
#include <fxe/generated/node_compat/async_hooks_adapter.hpp>
#include <fxe/generated/node_compat/buffer_adapter.hpp>
#include <fxe/generated/node_compat/child_process_adapter.hpp>
#include <fxe/generated/node_compat/console_adapter.hpp>
#include <fxe/generated/node_compat/crypto_adapter.hpp>
#include <fxe/generated/node_compat/dgram_adapter.hpp>
#include <fxe/generated/node_compat/dns_adapter.hpp>
#include <fxe/generated/node_compat/dns_promises_adapter.hpp>
#include <fxe/generated/node_compat/events_adapter.hpp>
#include <fxe/generated/node_compat/fs_adapter.hpp>
#include <fxe/generated/node_compat/fs_promises_adapter.hpp>
#include <fxe/generated/node_compat/http2_adapter.hpp>
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
#include <fxe/generated/node_compat/http2_native_adapter.hpp>
#include <fxe/generated/node_compat/https_native_adapter.hpp>
#endif
#include <fxe/generated/node_compat/https_adapter.hpp>
#include <fxe/generated/node_compat/inspector_adapter.hpp>
#include <fxe/generated/node_compat/inspector_promises_adapter.hpp>
#include <fxe/generated/node_compat/net_adapter.hpp>
#include <fxe/generated/node_compat/os_adapter.hpp>
#include <fxe/generated/node_compat/path_adapter.hpp>
#include <fxe/generated/node_compat/path_posix_adapter.hpp>
#include <fxe/generated/node_compat/path_win32_adapter.hpp>
#include <fxe/generated/node_compat/prelude.hpp>
#include <fxe/generated/node_compat/process_adapter.hpp>
#include <fxe/generated/node_compat/querystring_adapter.hpp>
#include <fxe/generated/node_compat/readline_adapter.hpp>
#include <fxe/generated/node_compat/readline_promises_adapter.hpp>
#include <fxe/generated/node_compat/repl_adapter.hpp>
#include <fxe/generated/node_compat/stream_adapter.hpp>
#include <fxe/generated/node_compat/stream_promises_adapter.hpp>
#include <fxe/generated/node_compat/timers_adapter.hpp>
#include <fxe/generated/node_compat/timers_promises_adapter.hpp>
#include <fxe/generated/node_compat/tls_adapter.hpp>
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
#include <fxe/generated/node_compat/tls_native_adapter.hpp>
#endif
#include <fxe/generated/node_compat/tty_adapter.hpp>
#include <fxe/generated/node_compat/url_adapter.hpp>
#include <fxe/generated/node_compat/util_adapter.hpp>
#include <fxe/generated/node_compat/util_types_adapter.hpp>
#include <fxe/generated/node_compat/v8_adapter.hpp>
#include <fxe/generated/node_compat/vm_adapter.hpp>
#include <fxe/generated/node_compat/wasi_adapter.hpp>
#include <fxe/generated/node_compat/worker_threads_adapter.hpp>
#include <fxe/generated/node_compat/zlib_adapter.hpp>
#include <fxe/v8_literals.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <v8.h>

namespace fxe::runtime {
  namespace {
    using namespace v8;

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string_view without_node_prefix(std::string_view specifier) {
      constexpr std::string_view prefix = "node:";
      if (specifier.starts_with(prefix))
        return specifier.substr(prefix.size());
      return specifier;
    }

    constexpr std::array<std::string_view, 68> k_node_builtins = {
        "assert",
        "assert/strict",
        "async_hooks",
        "buffer",
        "child_process",
        "cluster",
        "console",
        "constants",
        "crypto",
        "dgram",
        "diagnostics_channel",
        "dns",
        "dns/promises",
        "domain",
        "events",
        "fs",
        "fs/promises",
        "http",
        "http2",
        "https",
        "inspector",
        "inspector/promises",
        "module",
        "net",
        "os",
        "path",
        "path/posix",
        "path/win32",
        "perf_hooks",
        "process",
        "punycode",
        "querystring",
        "readline",
        "readline/promises",
        "repl",
        "stream",
        "stream/consumers",
        "stream/promises",
        "stream/web",
        "string_decoder",
        "sys",
        "test",
        "test/reporters",
        "timers",
        "timers/promises",
        "tls",
        "trace_events",
        "tty",
        "url",
        "util",
        "util/types",
        "v8",
        "vm",
        "wasi",
        "worker_threads",
        "zlib",
        "_http_agent",
        "_http_client",
        "_http_common",
        "_http_incoming",
        "_http_outgoing",
        "_http_server",
        "_stream_duplex",
        "_stream_passthrough",
        "_stream_readable",
        "_stream_transform",
        "_stream_wrap",
        "_stream_writable",
    };

    std::string canonical_node_specifier(std::string_view bare) {
      std::string canonical{"node:"};
      canonical.append(bare);
      return canonical;
    }

    constexpr std::string_view k_assert_strict_adapter_source = R"JS(
import assertDefault, { strict } from 'node:assert';

export const AssertionError = assertDefault.AssertionError;
export const CallTracker = assertDefault.CallTracker;
export const fail = assertDefault.fail;
export const ok = strict;
export const throws = assertDefault.throws;
export const rejects = assertDefault.rejects;
export const doesNotThrow = assertDefault.doesNotThrow;
export const doesNotReject = assertDefault.doesNotReject;
export const ifError = assertDefault.ifError;
export const match = assertDefault.match;
export const doesNotMatch = assertDefault.doesNotMatch;
export const deepStrictEqual = assertDefault.deepStrictEqual;
export const deepEqual = assertDefault.deepStrictEqual;
export const notDeepStrictEqual = assertDefault.notDeepStrictEqual;
export const notDeepEqual = assertDefault.notDeepStrictEqual;
export const strictEqual = assertDefault.strictEqual;
export const equal = assertDefault.strictEqual;
export const notStrictEqual = assertDefault.notStrictEqual;
export const notEqual = assertDefault.notStrictEqual;
export const partialDeepStrictEqual = assertDefault.partialDeepStrictEqual;
export { strict };
export default strict;
)JS";

    std::string node_asset_path_for(std::string_view bare) {
      if (bare == "path/posix" || bare == "path/win32")
        bare = "path";
      std::string path{"src/runtime/node/"};
      path.append(bare);
      path.append(".ts");
      return path;
    }

    std::string unenv_display_path(std::string_view lookup_path) {
      std::string path{"vendor/unenv/"};
      path.append(lookup_path);
      return path;
    }

    std::string_view unenv_lookup_path(std::string_view asset_path) {
      constexpr std::string_view prefix = "vendor/unenv/";
      if (asset_path.starts_with(prefix))
        return asset_path.substr(prefix.size());
      return asset_path;
    }

#ifdef FXE_ENABLE_NODE_COMPAT
    std::optional<std::string_view> find_unenv_asset_source(std::string_view asset_path) {
      auto it = std::find_if(k_unenv_assets.begin(), k_unenv_assets.end(),
                             [&](const auto& asset) { return asset.path == asset_path; });
      if (it == k_unenv_assets.end())
        return std::nullopt;
      return it->source;
    }
#endif

    std::optional<node_compat_asset> make_host_adapter_asset(std::string canonical_specifier,
                                                             std::string_view bare) {
      if (bare == "assert/strict")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/assert/strict.fxe.ts",
                                 k_assert_strict_adapter_source};
      if (bare == "async_hooks")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/async_hooks.fxe.ts",
                                 node_js::k_async_hooks_adapter_source};
      if (bare == "events")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/events.fxe.ts",
                                 node_js::k_events_adapter_source};
      if (bare == "buffer")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/buffer.fxe.ts",
                                 node_js::k_buffer_adapter_source};
      if (bare == "process")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/process.fxe.ts",
                                 node_js::k_process_adapter_source};
      if (bare == "path")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/path.fxe.ts",
                                 node_js::k_path_adapter_source};
      if (bare == "path/posix")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/path/posix.fxe.ts",
                                 node_js::k_path_posix_adapter_source};
      if (bare == "path/win32")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/path/win32.fxe.ts",
                                 node_js::k_path_win32_adapter_source};
      if (bare == "url")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/url.fxe.ts",
                                 node_js::k_url_adapter_source};
      if (bare == "querystring")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/querystring.fxe.ts",
                                 node_js::k_querystring_adapter_source};
      if (bare == "util")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/util.fxe.ts",
                                 node_js::k_util_adapter_source};
      if (bare == "util/types")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/util/types.fxe.ts",
                                 node_js::k_util_types_adapter_source};
      if (bare == "console")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/console.fxe.ts",
                                 node_js::k_console_adapter_source};
      if (bare == "timers")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/timers.fxe.ts",
                                 node_js::k_timers_adapter_source};
      if (bare == "timers/promises")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/timers/promises.fxe.ts",
                                 node_js::k_timers_promises_adapter_source};
      if (bare == "stream")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/stream.fxe.ts",
                                 node_js::k_stream_adapter_source};
      if (bare == "stream/promises")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/stream/promises.fxe.ts",
                                 node_js::k_stream_promises_adapter_source};
      if (bare == "readline")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/readline.fxe.ts",
                                 node_js::k_readline_adapter_source};
      if (bare == "readline/promises")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/readline/promises.fxe.ts",
                                 node_js::k_readline_promises_adapter_source};
      if (bare == "repl")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/repl.fxe.ts",
                                 node_js::k_repl_adapter_source};
      if (bare == "os")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/os.fxe.ts",
                                 node_js::k_os_adapter_source};
      if (bare == "tty")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/tty.fxe.ts",
                                 node_js::k_tty_adapter_source};
      if (bare == "crypto")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/crypto.fxe.ts",
                                 node_js::k_crypto_adapter_source};
      if (bare == "child_process")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/child_process.fxe.ts",
                                 node_js::k_child_process_adapter_source};
      if (bare == "fs")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/fs.fxe.ts",
                                 node_js::k_fs_adapter_source};
      if (bare == "fs/promises")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/fs/promises.fxe.ts",
                                 node_js::k_fs_promises_adapter_source};
      if (bare == "worker_threads")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/worker_threads.fxe.ts",
                                 node_js::k_worker_threads_adapter_source};
      if (bare == "net")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/net.fxe.ts",
                                 node_js::k_net_adapter_source};
      if (bare == "dgram")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/dgram.fxe.ts",
                                 node_js::k_dgram_adapter_source};
      if (bare == "dns")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/dns.fxe.ts",
                                 node_js::k_dns_adapter_source};
      if (bare == "dns/promises")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/dns/promises.fxe.ts",
                                 node_js::k_dns_promises_adapter_source};
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
      if (bare == "tls")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/tls.fxe.ts",
                                 node_js::k_tls_native_adapter_source};
#endif
      if (bare == "vm")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/vm.fxe.ts",
                                 node_js::k_vm_adapter_source};
      if (bare == "v8")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/v8.fxe.ts",
                                 node_js::k_v8_adapter_source};
      if (bare == "wasi")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/wasi.fxe.ts",
                                 node_js::k_wasi_adapter_source};
      if (bare == "inspector")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/inspector.fxe.ts",
                                 node_js::k_inspector_adapter_source};
      if (bare == "inspector/promises")
        return node_compat_asset{std::move(canonical_specifier),
                                 "src/runtime/node/inspector/promises.fxe.ts",
                                 node_js::k_inspector_promises_adapter_source};
      if (bare == "zlib")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/zlib.fxe.ts",
                                 node_js::k_zlib_adapter_source};
      if (bare == "tls")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/tls.fxe.ts",
                                 node_js::k_tls_adapter_source};
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
      if (bare == "https")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/https.fxe.ts",
                                 node_js::k_https_native_adapter_source};
#endif
      if (bare == "https")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/https.fxe.ts",
                                 node_js::k_https_adapter_source};
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
      if (bare == "http2")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/http2.fxe.ts",
                                 node_js::k_http2_native_adapter_source};
#endif
      if (bare == "http2")
        return node_compat_asset{std::move(canonical_specifier), "src/runtime/node/http2.fxe.ts",
                                 node_js::k_http2_adapter_source};
      return std::nullopt;
    }

    std::optional<node_compat_asset> make_unenv_asset(std::string canonical_specifier,
                                                      std::string_view lookup_path) {
#ifdef FXE_ENABLE_NODE_COMPAT
      auto source = find_unenv_asset_source(lookup_path);
      if (!source)
        return std::nullopt;
      return node_compat_asset{std::move(canonical_specifier), unenv_display_path(lookup_path),
                               *source};
#else
      (void)canonical_specifier;
      (void)lookup_path;
      return std::nullopt;
#endif
    }
  } // namespace

  void install_node_compat(Isolate* iso, Local<Context> ctx) {
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    install_native_tls(iso, ctx);
    install_native_http2(iso, ctx);
    install_native_https(iso, ctx);
#endif
    auto global = ctx->Global();
    auto status_fn =
        Function::New(ctx, [](const FunctionCallbackInfo<Value>& info) {
          auto* iso = info.GetIsolate();
          if (info.Length() < 1 || !info[0]->IsString())
            return;
          String::Utf8Value spec(iso, info[0]);
          std::string_view sv = *spec ? std::string_view(*spec, spec.length()) : std::string_view{};
          auto json = node_compat_module_status_json(sv);
          info.GetReturnValue().Set(String::NewFromUtf8(iso, json.c_str(), NewStringType::kNormal,
                                                        static_cast<int>(json.size()))
                                        .ToLocalChecked());
        }).ToLocalChecked();
    (void)global->Set(ctx, "__fxe_node_compat_status"_v8(iso), status_fn);
    v8::TryCatch tc(iso);
    v8::ScriptOrigin origin("<fxe-node-compat-prelude>"_v8(iso));
    Local<Script> script;
    if (!Script::Compile(ctx, str(iso, node_js::k_prelude_source), &origin).ToLocal(&script))
      return;
    Local<Value> ignored;
    (void)script->Run(ctx).ToLocal(&ignored);
  }

  bool is_node_builtin_specifier(std::string_view specifier) {
    auto bare = without_node_prefix(specifier);
    return std::find(k_node_builtins.begin(), k_node_builtins.end(), bare) != k_node_builtins.end();
  }

  std::optional<node_compat_asset> resolve_node_compat_asset(std::string_view specifier) {
    auto bare = without_node_prefix(specifier);
    if (!is_node_builtin_specifier(specifier))
      return std::nullopt;

    auto canonical = canonical_node_specifier(bare);
    if (auto adapter = make_host_adapter_asset(canonical, bare))
      return adapter;

    auto lookup_path = node_asset_path_for(bare);
    return make_unenv_asset(std::move(canonical), lookup_path);
  }

  std::optional<node_compat_asset> resolve_node_compat_asset_path(std::string_view asset_path) {
    auto lookup_path = unenv_lookup_path(asset_path);
    std::string canonical{"unenv:"};
    canonical.append(lookup_path);
    return make_unenv_asset(std::move(canonical), lookup_path);
  }

  std::optional<node_compat_asset> resolve_unenv_pathe_asset() {
    return make_unenv_asset("vendor:pathe", "node_modules/pathe/dist/index.mjs");
  }

  void throw_node_compat_disabled(Isolate* iso, std::string_view specifier) {
    std::string msg;
    msg.reserve(specifier.size() + sizeof("node compat disabled for specifier ''"));
    msg.append("node compat disabled for specifier '");
    msg.append(specifier);
    msg.append("'");
    iso->ThrowException(Exception::Error(str(iso, msg)));
  }

  std::string node_compat_module_status_json(std::string_view specifier) {
    auto bare = without_node_prefix(specifier);
    auto canonical = canonical_node_specifier(bare);
    std::string source;
    std::string asset_path;
    if (!is_node_builtin_specifier(specifier)) {
      source = "unsupported";
    } else if (auto adapter = make_host_adapter_asset(canonical, bare)) {
      source = "native";
      asset_path = adapter->asset_path;
    } else {
      auto lookup_path = node_asset_path_for(bare);
      auto unenv = make_unenv_asset(std::move(canonical), lookup_path);
      if (unenv) {
        source = "unenv";
        asset_path = unenv->asset_path;
      } else {
        source = "unsupported";
      }
    }
    auto json_escape = [](std::string_view s) {
      std::string out;
      out.reserve(s.size() + 2);
      for (char c : s) {
        if (c == '"' || c == '\\') {
          out.push_back('\\');
          out.push_back(c);
        } else if (c == '\n') {
          out += "\\n";
        } else {
          out.push_back(c);
        }
      }
      return out;
    };
    std::string out = "{\"specifier\":\"";
    out += json_escape(specifier);
    out += "\",\"source\":\"";
    out += source;
    out += "\"";
    if (!asset_path.empty()) {
      out += ",\"assetPath\":\"";
      out += json_escape(asset_path);
      out += "\"";
    }
    out += "}";
    return out;
  }

} // namespace fxe::runtime
