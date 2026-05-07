#define V8_COMPRESS_POINTERS 1

#include "bind_crash.hpp"

#include <fxe/crash.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <v8.h>
#include <vector>

namespace fxe::js {
  using namespace v8;

  namespace {
    std::atomic_bool g_auto_self_test_ran{false};
    Local<String> s(Isolate* iso, const char* str) {
      return String::NewFromUtf8(iso, str, NewStringType::kNormal).ToLocalChecked();
    }

    std::string to_str(Isolate* iso, Local<Value> value) {
      if (value.IsEmpty() || !value->IsString())
        return {};
      String::Utf8Value utf8(iso, value);
      return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
    }

    std::string get_optional_string(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                    const char* name) {
      Local<Value> value;
      if (!obj->Get(ctx, s(iso, name)).ToLocal(&value) || !value->IsString())
        return {};
      return to_str(iso, value);
    }

    bool get_optional_bool(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                           bool fallback) {
      Local<Value> value;
      if (!obj->Get(ctx, s(iso, name)).ToLocal(&value) || !value->IsBoolean())
        return fallback;
      return value->BooleanValue(iso);
    }

    void crash_start_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.crashReporter.start requires an options object")));
        return;
      }

      auto opts_obj = info[0].As<Object>();
      fxe::os::crash_options opts;
      opts.product_name = get_optional_string(iso, ctx, opts_obj, "productName");
      if (opts.product_name.empty()) {
        iso->ThrowException(
            Exception::TypeError(s(iso, "App.crashReporter.start requires options.productName")));
        return;
      }
      opts.product_version = get_optional_string(iso, ctx, opts_obj, "productVersion");
      opts.submit_url = get_optional_string(iso, ctx, opts_obj, "submitURL");
      opts.crash_dir = get_optional_string(iso, ctx, opts_obj, "crashDir");
      opts.upload_to_server = get_optional_bool(iso, ctx, opts_obj, "uploadToServer", false);

      info.GetReturnValue().Set(Boolean::New(iso, fxe::os::crash_start(opts)));
    }

    void crash_list_dumps_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::vector<std::string> dumps = fxe::os::crash_detail::list_dump_paths();
      auto arr = Array::New(iso, static_cast<int>(dumps.size()));
      for (std::size_t i = 0; i < dumps.size(); ++i) {
        (void)arr->Set(ctx, static_cast<uint32_t>(i), s(iso, dumps[i].c_str()));
      }
      info.GetReturnValue().Set(arr);
    }

    void crash_get_last_dump_path_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto last = fxe::os::crash_get_last_dump_path();
      if (!last) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      info.GetReturnValue().Set(s(iso, last->c_str()));
    }

    void crash_self_test_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto result = fxe::os::crash_self_test();
      if (!result.ok && !result.error.empty()) {
        iso->ThrowException(Exception::Error(s(iso, result.error.c_str())));
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, result.ok));
    }
  } // namespace

  void install_crash_reporter_to(Isolate* iso, Local<Context> ctx, Local<Object> appObj) {
    HandleScope hs(iso);
    auto reporter = Object::New(iso);
    (void)reporter->Set(ctx, s(iso, "start"), Function::New(ctx, crash_start_cb).ToLocalChecked());
    (void)reporter->Set(ctx, s(iso, "listDumps"),
                        Function::New(ctx, crash_list_dumps_cb).ToLocalChecked());
    (void)reporter->Set(ctx, s(iso, "getLastDumpPath"),
                        Function::New(ctx, crash_get_last_dump_path_cb).ToLocalChecked());
    (void)reporter->Set(ctx, s(iso, "selfTest"),
                        Function::New(ctx, crash_self_test_cb).ToLocalChecked());
    (void)appObj->Set(ctx, s(iso, "crashReporter"), reporter);
    (void)appObj->Set(ctx, s(iso, "crashReport"), reporter);
#ifndef NDEBUG
    if (const char* enabled = std::getenv("FXE_CRASH_SELF_TEST");
        enabled && std::string(enabled) == "1") {
      bool expected = false;
      if (g_auto_self_test_ran.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        auto result = fxe::os::crash_self_test();
        if (!result.ok) {
          std::fprintf(stderr, "[fxe] crash self-test failed: %s\n", result.error.c_str());
          std::exit(1);
        }
      }
    }
#endif
  }
} // namespace fxe::js
