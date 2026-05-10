#include "bind_process.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/log.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fxe/types.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <v8.h>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define fxe_getpid _getpid
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#define fxe_getpid getpid
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#elif !defined(_WIN32)
extern "C" char** environ;
#endif

#ifndef FXE_VERSION
#define FXE_VERSION "0.1.0"
#endif
#ifndef FXE_DAWN_VERSION
#define FXE_DAWN_VERSION "unknown"
#endif

#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32)
#if defined(_MSC_VER)
#pragma message("warning: fxe process.platform is unsupported for this target")
#else
#warning "fxe process.platform is unsupported for this target"
#endif
#endif

#if !defined(__aarch64__) && !defined(_M_ARM64) && !defined(__x86_64__) && !defined(_M_X64) &&     \
    !defined(__i386__) && !defined(_M_IX86)
#if defined(_MSC_VER)
#pragma message("warning: fxe process.arch is unsupported for this target")
#else
#warning "fxe process.arch is unsupported for this target"
#endif
#endif

namespace fxe::js {
  namespace {
    using namespace v8;

    std::mutex g_argv_mu;
    std::vector<std::string> g_argv;

    // Per-isolate event listener registry. Keyed by (isolate, event-name).
    struct listener_registry {
      // event -> list of callbacks
      std::unordered_map<std::string, std::vector<std::unique_ptr<Global<Function>>>> events;
    };
    std::unordered_map<Isolate*, std::unique_ptr<listener_registry>> g_listeners;
    std::mutex g_listeners_mu;

    listener_registry& listeners_for(Isolate* iso) {
      std::lock_guard<std::mutex> lk(g_listeners_mu);
      auto& slot = g_listeners[iso];
      if (!slot)
        slot = std::make_unique<listener_registry>();
      return *slot;
    }

#if defined(_WIN32)
    std::string wide_to_utf8(const wchar_t* value, int length) {
      if (!value || length <= 0)
        return {};
      int needed = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
      if (needed <= 0)
        return {};
      std::string out(static_cast<usize>(needed), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), needed, nullptr, nullptr);
      return out;
    }
#endif

    bool env_value(const std::string& key, std::string& out) {
#if defined(_WIN32)
      char* value = nullptr;
      usize value_len = 0;
      if (_dupenv_s(&value, &value_len, key.c_str()) != 0 || !value)
        return false;
      out.assign(value);
      std::free(value);
      return true;
#else
      const char* value = std::getenv(key.c_str());
      if (!value)
        return false;
      out.assign(value);
      return true;
#endif
    }

    // === env Proxy via NamedPropertyHandler ===

    Intercepted env_getter(Local<Name> name, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!name->IsString())
        return Intercepted::kNo;
      auto key = to_std_string(iso, name);
      std::string val;
      if (!env_value(key, val))
        return Intercepted::kNo;
      info.GetReturnValue().Set(to_v8_string(iso, val));
      return Intercepted::kYes;
    }

    Intercepted env_setter(Local<Name> name, Local<Value> value,
                           const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      if (!name->IsString())
        return Intercepted::kNo;
      auto key = to_std_string(iso, name);
      auto val = to_std_string(iso, value);
#if defined(_WIN32)
      _putenv_s(key.c_str(), val.c_str());
#else
      setenv(key.c_str(), val.c_str(), 1);
#endif
      return Intercepted::kYes;
    }

    Intercepted env_query(Local<Name> name, const PropertyCallbackInfo<Integer>& info) {
      auto* iso = info.GetIsolate();
      if (!name->IsString())
        return Intercepted::kNo;
      auto key = to_std_string(iso, name);
      std::string val;
      if (!env_value(key, val))
        return Intercepted::kNo;
      info.GetReturnValue().Set(Integer::New(iso, PropertyAttribute::None));
      return Intercepted::kYes;
    }

    Intercepted env_deleter(Local<Name> name, const PropertyCallbackInfo<Boolean>& info) {
      auto* iso = info.GetIsolate();
      if (!name->IsString())
        return Intercepted::kNo;
      auto key = to_std_string(iso, name);
#if defined(_WIN32)
      _putenv_s(key.c_str(), "");
#else
      unsetenv(key.c_str());
#endif
      info.GetReturnValue().Set(true);
      return Intercepted::kYes;
    }

    void env_enumerator(const PropertyCallbackInfo<Array>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::vector<std::string> keys;
#if defined(_WIN32)
      // The previous MSVC shim declared `extern char** _environ` to avoid
      // pulling in Win32 UTF-16 environment handling while process.env support
      // was minimal. That exposed the CRT cache directly and skipped the
      // ownership/error semantics of `_dupenv_s`; enumerate the OS block and
      // confirm each key through `_dupenv_s` so values set via `_putenv_s` are
      // reflected without leaking CRT-allocated buffers.
      if (LPWCH block = GetEnvironmentStringsW()) {
        for (wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
          const wchar_t* eq = std::wcschr(entry, L'=');
          if (!eq || eq == entry)
            continue;
          auto key = wide_to_utf8(entry, static_cast<int>(eq - entry));
          std::string ignored_value;
          if (!key.empty() && env_value(key, ignored_value))
            keys.push_back(key);
        }
        FreeEnvironmentStringsW(block);
      }
#else
      char** e = environ;
      for (; e && *e; ++e) {
        std::string entry(*e);
        auto eq = entry.find('=');
        if (eq != std::string::npos)
          keys.push_back(entry.substr(0, eq));
      }
#endif
      auto arr = Array::New(iso, static_cast<int>(keys.size()));
      for (u32 i = 0; i < keys.size(); ++i)
        (void)arr->Set(ctx, i, to_v8_string(iso, keys[i]));
      info.GetReturnValue().Set(arr);
    }

    Local<Object> build_env(Isolate* iso, Local<Context> ctx) {
      auto t = ObjectTemplate::New(iso);
      NamedPropertyHandlerConfiguration cfg(env_getter, env_setter, env_query, env_deleter,
                                            env_enumerator);
      t->SetHandler(cfg);
      return t->NewInstance(ctx).ToLocalChecked();
    }

    // === simple methods ===

    void process_cwd(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      std::error_code ec;
      auto p = std::filesystem::current_path(ec);
      info.GetReturnValue().Set(to_v8_string(iso, ec ? std::string{} : p.generic_string()));
    }

    void process_chdir(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "chdir(path)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      std::error_code ec;
      std::filesystem::current_path(p, ec);
      if (ec) {
        auto err = Exception::Error(to_v8_string(iso, "chdir: " + ec.message())).As<Object>();
        (void)err->Set(iso->GetCurrentContext(), "code"_v8(iso), "ENOENT"_v8(iso));
        iso->ThrowException(err);
      }
    }

    void process_exit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      int code = 0;
      if (info.Length() >= 1)
        code = info[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0);
      // Fire 'exit' listeners before exiting.
      {
        HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        Local<Value> argv[1] = {Integer::New(iso, code)};
        (void)emit_process_event(iso, ctx, "exit", 1, argv);
      }
      // Pump microtasks once so any scheduled work runs.
      iso->PerformMicrotaskCheckpoint();
      std::fflush(stdout);
      std::fflush(stderr);
      std::exit(code);
    }

    void process_on_off_impl(const FunctionCallbackInfo<Value>& info, bool add) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction()) {
        (void)throw_type_error(iso, "on(event, fn)");
        return;
      }
      auto event = to_std_string(iso, info[0]);
      auto fn = info[1].As<Function>();
      auto& reg = listeners_for(iso);
      auto& vec = reg.events[event];
      if (add) {
        vec.push_back(std::make_unique<Global<Function>>(iso, fn));
      } else {
        for (auto& g : vec) {
          if (g && g->Get(iso)->StrictEquals(fn)) {
            g->Reset();
            g.reset();
          }
        }
      }
      info.GetReturnValue().Set(info.This());
    }
    void process_on(const FunctionCallbackInfo<Value>& info) {
      process_on_off_impl(info, true);
    }
    void process_off(const FunctionCallbackInfo<Value>& info) {
      process_on_off_impl(info, false);
    }

    // nextTick: enqueue a JS function microtask with bound arguments.

    void process_next_tick(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "nextTick(fn, ...args)");
        return;
      }
      auto ctx = iso->GetCurrentContext();
      auto fn = info[0].As<Function>();
      Local<Value> bind_value;
      if (!fn->Get(ctx, "bind"_v8(iso)).ToLocal(&bind_value) || !bind_value->IsFunction()) {
        (void)throw_type_error(iso, "nextTick(fn, ...args)");
        return;
      }
      std::vector<Local<Value>> argv;
      argv.reserve(static_cast<usize>(info.Length()));
      argv.push_back(Undefined(iso));
      for (int i = 1; i < info.Length(); ++i)
        argv.push_back(info[i]);
      Local<Value> bound_value;
      if (!bind_value.As<Function>()
               ->Call(ctx, fn, static_cast<int>(argv.size()), argv.data())
               .ToLocal(&bound_value) ||
          !bound_value->IsFunction()) {
        (void)throw_type_error(iso, "nextTick(fn, ...args)");
        return;
      }
      iso->EnqueueMicrotask(bound_value.As<Function>());
    }

    int signal_number_from_string(const std::string& name, bool& ok) {
      if (name == "0") {
        ok = true;
        return 0;
      }
#ifdef SIGHUP
      if (name == "SIGHUP" || name == "HUP") {
        ok = true;
        return SIGHUP;
      }
#endif
#ifdef SIGINT
      if (name == "SIGINT" || name == "INT") {
        ok = true;
        return SIGINT;
      }
#endif
#ifdef SIGTERM
      if (name == "SIGTERM" || name == "TERM") {
        ok = true;
        return SIGTERM;
      }
#endif
#ifdef SIGKILL
      if (name == "SIGKILL" || name == "KILL") {
        ok = true;
        return SIGKILL;
      }
#endif
#ifdef SIGUSR1
      if (name == "SIGUSR1" || name == "USR1") {
        ok = true;
        return SIGUSR1;
      }
#endif
#ifdef SIGUSR2
      if (name == "SIGUSR2" || name == "USR2") {
        ok = true;
        return SIGUSR2;
      }
#endif
      ok = false;
      return 0;
    }

    int process_signal_arg(Isolate* iso, Local<Context> ctx, Local<Value> value, bool& ok) {
      ok = true;
      if (value->IsUndefined() || value->IsNull())
        return SIGTERM;
      if (value->IsNumber())
        return value->Int32Value(ctx).FromMaybe(SIGTERM);
      if (value->IsString()) {
        auto name = to_std_string(iso, value);
        return signal_number_from_string(name, ok);
      }
      ok = false;
      return 0;
    }

    void process_kill(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsNumber()) {
        (void)throw_type_error(iso, "kill(pid[, signal])");
        return;
      }
      const int pid = info[0]->Int32Value(ctx).FromMaybe(0);
      bool ok_signal = true;
      const int sig =
          process_signal_arg(iso, ctx, info.Length() > 1 ? info[1] : Undefined(iso), ok_signal);
      if (!ok_signal) {
        (void)throw_type_error(iso, "unknown signal");
        return;
      }
#if defined(_WIN32)
      HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
      if (!process) {
        auto err = Exception::Error("kill: process not found"_v8(iso)).As<Object>();
        (void)err->Set(ctx, "code"_v8(iso), "ESRCH"_v8(iso));
        iso->ThrowException(err);
        return;
      }
      const bool success = sig == 0 || TerminateProcess(process, 1);
      CloseHandle(process);
      if (!success) {
        auto err = Exception::Error("kill: permission denied"_v8(iso)).As<Object>();
        (void)err->Set(ctx, "code"_v8(iso), "EPERM"_v8(iso));
        iso->ThrowException(err);
        return;
      }
#else
      if (::kill(pid, sig) != 0) {
        auto err = Exception::Error(to_v8_string(iso, std::string("kill: ") + std::strerror(errno)))
                       .As<Object>();
        (void)err->Set(ctx, "code"_v8(iso), to_v8_string(iso, errno == ESRCH ? "ESRCH" : "EPERM"));
        iso->ThrowException(err);
        return;
      }
#endif
      info.GetReturnValue().Set(Boolean::New(iso, true));
    }

    void process_umask(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      int mask = 0;
      const bool has_arg = info.Length() > 0 && !info[0]->IsUndefined();
      if (has_arg) {
        mask = info[0]->Int32Value(ctx).FromMaybe(0);
        if (mask < 0 || mask > 0777) {
          (void)throw_range_error(iso, "umask mask must be between 0 and 0o777");
          return;
        }
      }
#if defined(_WIN32)
      const int old_mask = has_arg ? _umask(mask) : _umask(0);
      if (!has_arg)
        (void)_umask(old_mask);
#else
      const mode_t old_mask = has_arg ? ::umask(static_cast<mode_t>(mask)) : ::umask(0);
      if (!has_arg)
        (void)::umask(old_mask);
#endif
      info.GetReturnValue().Set(Integer::New(iso, static_cast<i32>(old_mask)));
    }

    i64 hrtime_nanoseconds() {
      using clock = std::chrono::steady_clock;
      static const auto start = clock::now();
      return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    }

    void set_hrtime_array(Isolate* iso, Local<Context> ctx, i64 ns, Local<Array> out) {
      const i64 sec = ns / 1000000000LL;
      i64 nsec = ns % 1000000000LL;
      if (nsec < 0) {
        nsec += 1000000000LL;
      }
      (void)out->Set(ctx, 0, Number::New(iso, static_cast<double>(sec)));
      (void)out->Set(ctx, 1, Number::New(iso, static_cast<double>(nsec)));
    }

    void process_hrtime(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      i64 ns = hrtime_nanoseconds();
      if (info.Length() > 0 && info[0]->IsArray()) {
        auto previous = info[0].As<Array>();
        Local<Value> sec_value;
        Local<Value> nsec_value;
        if (previous->Get(ctx, 0).ToLocal(&sec_value) &&
            previous->Get(ctx, 1).ToLocal(&nsec_value)) {
          const i64 prev_sec = static_cast<i64>(sec_value->IntegerValue(ctx).FromMaybe(0));
          const i64 prev_nsec = static_cast<i64>(nsec_value->IntegerValue(ctx).FromMaybe(0));
          ns -= prev_sec * 1000000000LL + prev_nsec;
        }
      }
      auto out = Array::New(iso, 2);
      set_hrtime_array(iso, ctx, ns, out);
      info.GetReturnValue().Set(out);
    }

    void process_hrtime_bigint(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(BigInt::New(info.GetIsolate(), hrtime_nanoseconds()));
    }

    void stream_write_impl(const FunctionCallbackInfo<Value>& info, FILE* sink) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1) {
        info.GetReturnValue().Set(true);
        return;
      }
      auto v = info[0];
      if (v->IsString()) {
        auto s = to_std_string(iso, v);
        if (!s.empty())
          std::fwrite(s.data(), 1, s.size(), sink);
      } else if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        usize n = view->ByteLength();
        if (n) {
          std::vector<u8> buf(n);
          view->CopyContents(buf.data(), n);
          std::fwrite(buf.data(), 1, n, sink);
        }
      }
      std::fflush(sink);
      info.GetReturnValue().Set(true);
    }
    void stdout_write(const FunctionCallbackInfo<Value>& info) {
      stream_write_impl(info, stdout);
    }
    void stderr_write(const FunctionCallbackInfo<Value>& info) {
      stream_write_impl(info, stderr);
    }

    bool stdin_emit(Isolate* iso, Local<Object> input, const std::string& event,
                    Local<Value> arg = Local<Value>()) {
      auto ctx = iso->GetCurrentContext();
      auto key = to_v8_string(iso, std::string("__fxe_stdin_") + event);
      Local<Value> slot;
      if (!input->Get(ctx, key).ToLocal(&slot) || !slot->IsArray())
        return true;
      auto listeners = slot.As<Array>();
      std::vector<Local<Object>> keep;
      for (u32 i = 0; i < listeners->Length(); ++i) {
        Local<Value> entry_value;
        if (!listeners->Get(ctx, i).ToLocal(&entry_value) || !entry_value->IsObject())
          continue;
        auto entry = entry_value.As<Object>();
        Local<Value> fn_value;
        if (!entry->Get(ctx, "fn"_v8(iso)).ToLocal(&fn_value) || !fn_value->IsFunction())
          continue;
        bool once = false;
        Local<Value> once_value;
        if (entry->Get(ctx, "once"_v8(iso)).ToLocal(&once_value))
          once = once_value->BooleanValue(iso);
        Local<Value> argv[1] = {arg.IsEmpty() ? Undefined(iso) : arg};
        Local<Value> ignored;
        if (!fn_value.As<Function>()
                 ->Call(ctx, input, arg.IsEmpty() ? 0 : 1, arg.IsEmpty() ? nullptr : argv)
                 .ToLocal(&ignored))
          return false;
        if (!once)
          keep.push_back(entry);
      }
      for (u32 i = 0; i < keep.size(); ++i)
        (void)listeners->Set(ctx, i, keep[i]);
      (void)listeners->Set(ctx, "length"_v8(iso),
                           Integer::NewFromUnsigned(iso, static_cast<u32>(keep.size())));
      return true;
    }

    Local<Value> stdin_chunk_value(Isolate* iso, Local<Object> input, const char* data,
                                   usize size) {
      auto ctx = iso->GetCurrentContext();
      Local<Value> encoding;
      if (input->Get(ctx, "readableEncoding"_v8(iso)).ToLocal(&encoding) && encoding->IsString()) {
        return to_v8_string(iso, std::string_view(data, size));
      }
      auto ab = ArrayBuffer::New(iso, size);
      std::memcpy(ab->GetBackingStore()->Data(), data, size);
      return Uint8Array::New(ab, 0, size);
    }

    bool stdin_mark_nonblocking(Isolate* iso, Local<Object> input) {
#if defined(_WIN32)
      (void)iso;
      (void)input;
      return true;
#else
      auto ctx = iso->GetCurrentContext();
      Local<Value> marked;
      if (input->Get(ctx, "__fxe_stdin_nonblocking"_v8(iso)).ToLocal(&marked) &&
          marked->BooleanValue(iso))
        return true;
      int flags = fcntl(0, F_GETFL, 0);
      if (flags < 0)
        return errno == EBADF;
      if ((flags & O_NONBLOCK) == 0)
        (void)fcntl(0, F_SETFL, flags | O_NONBLOCK);
      (void)input->Set(ctx, "__fxe_stdin_nonblocking"_v8(iso), Boolean::New(iso, true));
      return true;
#endif
    }

    void stdin_poll(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto input = info.This();
      if (!stdin_mark_nonblocking(iso, input)) {
        info.GetReturnValue().Set(false);
        return;
      }
#if defined(_WIN32)
      info.GetReturnValue().Set(false);
#else
      bool keep_polling = true;
      for (;;) {
        char buf[8192];
        ssize_t n = ::read(0, buf, sizeof(buf));
        if (n > 0) {
          if (!stdin_emit(iso, input, "data",
                          stdin_chunk_value(iso, input, buf, static_cast<usize>(n))))
            return;
          continue;
        }
        if (n == 0) {
          (void)input->Set(ctx, "readable"_v8(iso), Boolean::New(iso, false));
          (void)input->Set(ctx, "__fxe_stdin_ended"_v8(iso), Boolean::New(iso, true));
          (void)stdin_emit(iso, input, "end");
          keep_polling = false;
          break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
          break;
        (void)input->Set(ctx, "readable"_v8(iso), Boolean::New(iso, false));
        if (errno != EBADF) {
          auto err = Exception::Error(
                         to_v8_string(iso, std::string("stdin read: ") + std::strerror(errno)))
                         .As<Object>();
          (void)err->Set(ctx, "code"_v8(iso), to_v8_string(iso, std::strerror(errno)));
          (void)stdin_emit(iso, input, "error", err);
        }
        keep_polling = false;
        break;
      }
      info.GetReturnValue().Set(Boolean::New(iso, keep_polling));
#endif
    }

    void stdin_resume(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      (void)info.This()->Set(ctx, "__fxe_stdin_flowing"_v8(iso), Boolean::New(iso, true));
      constexpr const char kResume[] = R"JS(
(function(input) {
  if (input.__fxe_stdin_timer !== undefined || input.__fxe_stdin_ended) return input;
  const tick = () => {
    if (!input.__fxe_stdin_flowing || !input.__fxe_stdin_poll()) {
      if (input.__fxe_stdin_timer !== undefined) {
        clearInterval(input.__fxe_stdin_timer);
        input.__fxe_stdin_timer = undefined;
      }
    }
  };
  input.__fxe_stdin_timer = setInterval(tick, 5);
  Promise.resolve().then(tick);
  return input;
})
)JS";
      Local<Script> script;
      Local<Value> factory_value;
      Local<Value> result;
      if (Script::Compile(ctx, to_v8_string(iso, kResume)).ToLocal(&script) &&
          script->Run(ctx).ToLocal(&factory_value) && factory_value->IsFunction()) {
        Local<Value> argv[1] = {info.This()};
        (void)factory_value.As<Function>()->Call(ctx, info.This(), 1, argv).ToLocal(&result);
      }
      info.GetReturnValue().Set(info.This());
    }

    void stdin_pause(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      constexpr const char kPause[] = R"JS(
(function(input) {
  input.__fxe_stdin_flowing = false;
  if (input.__fxe_stdin_timer !== undefined) {
    clearInterval(input.__fxe_stdin_timer);
    input.__fxe_stdin_timer = undefined;
  }
  return input;
})
)JS";
      Local<Script> script;
      Local<Value> factory_value;
      Local<Value> result;
      if (Script::Compile(ctx, to_v8_string(iso, kPause)).ToLocal(&script) &&
          script->Run(ctx).ToLocal(&factory_value) && factory_value->IsFunction()) {
        Local<Value> argv[1] = {info.This()};
        (void)factory_value.As<Function>()->Call(ctx, info.This(), 1, argv).ToLocal(&result);
      }
      info.GetReturnValue().Set(info.This());
    }

    void stdin_set_encoding(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "setEncoding(encoding)");
        return;
      }
      (void)info.This()->Set(ctx, "readableEncoding"_v8(iso), info[0]);
      info.GetReturnValue().Set(info.This());
    }

    void stdin_on_off_impl(const FunctionCallbackInfo<Value>& info, bool add, bool once = false) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction()) {
        (void)throw_type_error(iso, "on(event, fn)");
        return;
      }
      auto event = to_std_string(iso, info[0]);
      if (event != "data" && event != "end" && event != "error") {
        info.GetReturnValue().Set(info.This());
        return;
      }
      auto ctx = iso->GetCurrentContext();
      auto key = to_v8_string(iso, std::string("__fxe_stdin_") + event);
      Local<Value> slot;
      Local<Array> listeners;
      if (info.This()->Get(ctx, key).ToLocal(&slot) && slot->IsArray()) {
        listeners = slot.As<Array>();
      } else {
        listeners = Array::New(iso);
        (void)info.This()->Set(ctx, key, listeners);
      }

      auto fn = info[1].As<Function>();
      if (add) {
        auto entry = Object::New(iso);
        (void)entry->Set(ctx, "fn"_v8(iso), fn);
        (void)entry->Set(ctx, "once"_v8(iso), Boolean::New(iso, once));
        (void)listeners->Set(ctx, listeners->Length(), entry);
        if (event == "data") {
          Local<Value> resume_value;
          Local<Value> ignored;
          if (info.This()->Get(ctx, "resume"_v8(iso)).ToLocal(&resume_value) &&
              resume_value->IsFunction())
            (void)resume_value.As<Function>()->Call(ctx, info.This(), 0, nullptr).ToLocal(&ignored);
        }
      } else {
        u32 out = 0;
        for (u32 i = 0; i < listeners->Length(); ++i) {
          Local<Value> entry_value;
          if (!listeners->Get(ctx, i).ToLocal(&entry_value) || !entry_value->IsObject())
            continue;
          auto entry = entry_value.As<Object>();
          Local<Value> fn_value;
          if (!entry->Get(ctx, "fn"_v8(iso)).ToLocal(&fn_value) || !fn_value->StrictEquals(fn)) {
            if (out != i)
              (void)listeners->Set(ctx, out, entry);
            ++out;
          }
        }
        (void)listeners->Set(ctx, "length"_v8(iso), Integer::NewFromUnsigned(iso, out));
      }
      info.GetReturnValue().Set(info.This());
    }
    void stdin_on(const FunctionCallbackInfo<Value>& info) {
      stdin_on_off_impl(info, true);
    }
    void stdin_once(const FunctionCallbackInfo<Value>& info) {
      stdin_on_off_impl(info, true, true);
    }
    void stdin_off(const FunctionCallbackInfo<Value>& info) {
      stdin_on_off_impl(info, false);
    }

    void stdin_async_iterator_next(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto result = Object::New(iso);
      (void)result->Set(ctx, "done"_v8(iso), Boolean::New(iso, true));
      (void)result->Set(ctx, "value"_v8(iso), Undefined(iso));
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Resolve(ctx, result);
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    void stdin_async_iterator(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto iterator = Object::New(iso);
      (void)iterator->Set(ctx, "next"_v8(iso),
                          Function::New(ctx, stdin_async_iterator_next).ToLocalChecked());
      info.GetReturnValue().Set(iterator);
    }

    const char* platform_name() {
#if defined(__APPLE__)
      return "darwin";
#elif defined(__linux__)
      return "linux";
#elif defined(_WIN32)
      return "win32";
#else
      return nullptr;
#endif
    }
    const char* arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
      return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
      return "x64";
#elif defined(__i386__) || defined(_M_IX86)
      return "ia32";
#else
      return nullptr;
#endif
    }

    void process_platform_getter(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      const char* name = platform_name();
      if (!name) {
        (void)throw_error(iso, "process.platform is unsupported for this build target");
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, name));
    }

    void process_arch_getter(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      const char* name = arch_name();
      if (!name) {
        (void)throw_error(iso, "process.arch is unsupported for this build target");
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, name));
    }
  } // namespace

  void set_host_argv(std::vector<std::string> argv) {
    std::lock_guard<std::mutex> lk(g_argv_mu);
    g_argv = std::move(argv);
  }

  int emit_process_event(Isolate* iso, Local<Context> ctx, std::string_view event, int argc,
                         Local<Value> argv[]) {
    HandleScope hs(iso);
    // Snapshot live listener handles under the registry mutex so that a
    // listener that calls process.on/off during dispatch can mutate the
    // registry without invalidating our iteration.
    std::vector<Global<Function>*> snapshot;
    {
      std::lock_guard<std::mutex> lk(g_listeners_mu);
      auto reg_it = g_listeners.find(iso);
      if (reg_it == g_listeners.end() || !reg_it->second)
        return 0;
      auto event_it = reg_it->second->events.find(std::string(event));
      if (event_it == reg_it->second->events.end())
        return 0;
      snapshot.reserve(event_it->second.size());
      for (auto& g : event_it->second) {
        if (g)
          snapshot.push_back(g.get());
      }
    }
    Local<Object> recv;
    {
      Local<Value> proc_value;
      if (ctx->Global()->Get(ctx, "process"_v8(iso)).ToLocal(&proc_value) &&
          proc_value->IsObject()) {
        recv = proc_value.As<Object>();
      } else {
        recv = ctx->Global();
      }
    }
    int invoked = 0;
    for (auto* g : snapshot) {
      // Re-check the slot after every dispatch — a previous listener may have
      // detached this one via process.off, in which case the unique_ptr was
      // reset (see process_on_off_impl above).
      if (!g || g->IsEmpty())
        continue;
      auto fn = g->Get(iso);
      Local<Value> ignored;
      TryCatch tc(iso);
      if (!fn->Call(ctx, recv, argc, argv).ToLocal(&ignored)) {
        // Surface listener exceptions to stderr so they don't silently swallow
        // unhandledRejection / exit hooks. We can't rethrow — the caller (e.g.
        // promise_reject_callback) is in a context where throwing is unsafe.
        if (tc.HasCaught()) {
          auto message = to_std_string(iso, tc.Exception());
          if (!message.empty())
            FXE_ERROR("js.process", "process listener for '{}' threw: {}",
                      std::string_view(event.data(), event.size()), message);
          tc.Reset();
        }
      }
      ++invoked;
    }
    return invoked;
  }

  void clear_process_listeners(Isolate* iso) {
    std::lock_guard<std::mutex> lk(g_listeners_mu);
    auto it = g_listeners.find(iso);
    if (it == g_listeners.end())
      return;
    if (it->second) {
      for (auto& [_event, vec] : it->second->events) {
        for (auto& g : vec) {
          if (g)
            g->Reset();
        }
      }
    }
    g_listeners.erase(it);
  }

  void install_process_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);

    auto proc = ObjectTemplate::New(iso);
    proc->Set(iso, "cwd", FunctionTemplate::New(iso, process_cwd));
    proc->Set(iso, "chdir", FunctionTemplate::New(iso, process_chdir));
    proc->Set(iso, "exit", FunctionTemplate::New(iso, process_exit));
    proc->Set(iso, "on", FunctionTemplate::New(iso, process_on));
    proc->Set(iso, "off", FunctionTemplate::New(iso, process_off));
    proc->Set(iso, "nextTick", FunctionTemplate::New(iso, process_next_tick));
    proc->Set(iso, "kill", FunctionTemplate::New(iso, process_kill));
    proc->Set(iso, "umask", FunctionTemplate::New(iso, process_umask));
    auto hrtime = FunctionTemplate::New(iso, process_hrtime);
    hrtime->Set(iso, "bigint", FunctionTemplate::New(iso, process_hrtime_bigint));
    proc->Set(iso, "hrtime", hrtime);
    proc->SetAccessorProperty("platform"_v8(iso),
                              FunctionTemplate::New(iso, process_platform_getter));
    proc->SetAccessorProperty("arch"_v8(iso), FunctionTemplate::New(iso, process_arch_getter));
    proc->Set(iso, "pid", Integer::New(iso, static_cast<i32>(fxe_getpid())));

    auto versions = ObjectTemplate::New(iso);
    versions->Set(iso, "fxe", to_v8(iso, FXE_VERSION));
    versions->Set(iso, "v8", to_v8(iso, V8::GetVersion()));
    versions->Set(iso, "dawn", to_v8(iso, FXE_DAWN_VERSION));
    proc->Set(iso, "versions", versions);

    auto release = ObjectTemplate::New(iso);
    release->Set(iso, "name", "fxe"_v8(iso));
    proc->Set(iso, "release", release);

    auto sout = ObjectTemplate::New(iso);
    sout->Set(iso, "write", FunctionTemplate::New(iso, stdout_write));
    proc->Set(iso, "stdout", sout);
    auto serr = ObjectTemplate::New(iso);
    serr->Set(iso, "write", FunctionTemplate::New(iso, stderr_write));
    auto sin = ObjectTemplate::New(iso);
    sin->Set(iso, "fd", 0_v8(iso));
    sin->Set(iso, "isTTY", Boolean::New(iso, false));
    sin->Set(iso, "readable", Boolean::New(iso, true));
    sin->Set(iso, "readableEncoding", Null(iso));
    sin->Set(iso, "setEncoding", FunctionTemplate::New(iso, stdin_set_encoding));
    sin->Set(iso, "on", FunctionTemplate::New(iso, stdin_on));
    sin->Set(iso, "once", FunctionTemplate::New(iso, stdin_once));
    sin->Set(iso, "off", FunctionTemplate::New(iso, stdin_off));
    sin->Set(iso, "removeListener", FunctionTemplate::New(iso, stdin_off));
    sin->Set(iso, "resume", FunctionTemplate::New(iso, stdin_resume));
    sin->Set(iso, "pause", FunctionTemplate::New(iso, stdin_pause));
    sin->Set(iso, "__fxe_stdin_poll", FunctionTemplate::New(iso, stdin_poll));
    sin->Set(Symbol::GetAsyncIterator(iso), FunctionTemplate::New(iso, stdin_async_iterator));
    proc->Set(iso, "stdin", sin);
    proc->Set(iso, "stderr", serr);

    // argv: snapshot at install time; if empty, leave as []. Process.argv is
    // read-only-ish, set as a plain array on the template.
    {
      std::lock_guard<std::mutex> lk(g_argv_mu);
      // ObjectTemplate::Set with an array literal isn't directly supported, so
      // use a property accessor that materialises the current argv lazily.
    }
    proc->SetAccessorProperty(
        "argv"_v8(iso), FunctionTemplate::New(iso, [](const FunctionCallbackInfo<Value>& info) {
          auto* iso = info.GetIsolate();
          HandleScope hs(iso);
          auto ctx = iso->GetCurrentContext();
          std::vector<std::string> snap;
          {
            std::lock_guard<std::mutex> lk(g_argv_mu);
            snap = g_argv;
          }
          auto arr = Array::New(iso, static_cast<int>(snap.size()));
          for (u32 i = 0; i < snap.size(); ++i)
            (void)arr->Set(ctx, i, to_v8_string(iso, snap[i]));
          info.GetReturnValue().Set(arr);
        }));

    // env is a per-instance object; install via accessor that builds it once
    // per context. The handler-based template provides Proxy-like semantics.
    proc->SetAccessorProperty(
        "env"_v8(iso), FunctionTemplate::New(iso, [](const FunctionCallbackInfo<Value>& info) {
          auto* iso = info.GetIsolate();
          HandleScope hs(iso);
          auto ctx = iso->GetCurrentContext();
          info.GetReturnValue().Set(build_env(iso, ctx));
        }));

    global->Set(iso, "process", proc);
  }
} // namespace fxe::js
