#include "bind_crash.hpp"

#include <fxe/crash.hpp>
#include <fxe/log.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <string>
#include <v8.h>
#include <vector>

namespace fxe::js {
  using namespace v8;

  namespace {
    [[maybe_unused]] std::atomic_bool g_auto_self_test_ran{false};

    void crash_start_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "App.crashReporter.start requires an options object");
        return;
      }

      auto opts_obj = info[0].As<Object>();
      fxe::os::crash_options opts;
      opts.product_name = get_prop<std::string>(ctx, opts_obj, "productName").value_or("");
      if (opts.product_name.empty()) {
        (void)throw_type_error(iso, "App.crashReporter.start requires options.productName");
        return;
      }
      opts.product_version = get_prop<std::string>(ctx, opts_obj, "productVersion").value_or("");
      opts.submit_url = get_prop<std::string>(ctx, opts_obj, "submitURL").value_or("");
      opts.crash_dir = get_prop<std::string>(ctx, opts_obj, "crashDir").value_or("");
      opts.upload_to_server = get_prop_or<bool>(ctx, opts_obj, "uploadToServer", false);
      opts.include_full_memory_dump =
          get_prop_or<bool>(ctx, opts_obj, "includeFullMemoryDump", false);
      opts.include_stack_memory = get_prop_or<bool>(ctx, opts_obj, "includeStackMemory", true);
      if (auto scrub_keys = get_prop<Local<Value>>(ctx, opts_obj, "scrubAnnotationKeys");
          scrub_keys.has_value() && (*scrub_keys)->IsArray()) {
        auto array = (*scrub_keys).As<Array>();
        u32 length = array->Length();
        opts.scrub_annotation_keys.reserve(length);
        for (u32 i = 0; i < length; ++i) {
          if (auto entry = get_index<Local<Value>>(ctx, array, i);
              entry.has_value() && (*entry)->IsString())
            opts.scrub_annotation_keys.push_back(to_std_string_strict(iso, *entry));
        }
      }

      info.GetReturnValue().Set(to_v8(iso, fxe::os::crash_start(opts)));
    }

    void crash_list_dumps_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::vector<std::string> dumps = fxe::os::crash_detail::list_dump_paths();
      auto arr = Array::New(iso, static_cast<int>(dumps.size()));
      for (usize i = 0; i < dumps.size(); ++i) {
        set_index(ctx, arr, static_cast<u32>(i), dumps[i]);
      }
      info.GetReturnValue().Set(arr);
    }

    void crash_get_last_dump_path_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto last = fxe::os::crash_get_last_dump_path();
      if (!last) {
        info.GetReturnValue().Set(to_v8_null(iso));
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, *last));
    }

    void crash_self_test_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto result = fxe::os::crash_self_test();
      if (!result.ok && !result.error.empty()) {
        (void)throw_error(iso, result.error.c_str());
        return;
      }
      info.GetReturnValue().Set(to_v8(iso, result.ok));
    }
  } // namespace

  void install_crash_reporter_to(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
    HandleScope hs(iso);
    auto reporter = Object::New(iso);
    add_function(ctx, reporter, "start", crash_start_cb);
    add_function(ctx, reporter, "listDumps", crash_list_dumps_cb);
    add_function(ctx, reporter, "getLastDumpPath", crash_get_last_dump_path_cb);
    add_function(ctx, reporter, "selfTest", crash_self_test_cb);
    set_prop(ctx, appObj, "crashReporter", reporter);
    set_prop(ctx, appObj, "crashReport", reporter);
#ifndef NDEBUG
    if (const char* enabled = std::getenv("FXE_CRASH_SELF_TEST");
        enabled && std::string(enabled) == "1") {
      bool expected = false;
      if (g_auto_self_test_ran.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        auto result = fxe::os::crash_self_test();
        if (!result.ok) {
          FXE_ERROR("crash.selftest", "crash self-test failed: {}", result.error);
          std::exit(1);
        }
      }
    }
#endif
  }
} // namespace fxe::js
