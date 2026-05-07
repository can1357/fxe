#include "fxe_native.hpp"
#include "runtime/capabilities.hpp"
#include "runtime/v8/fs_fd.hpp"
#include "runtime/uv_loop.hpp"
#include <fxe/v8_host.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mbedtls/asn1.h>
#include <mbedtls/bignum.h>
#include <mbedtls/cipher.h>
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/rsa.h>
#include <memory>
#include <mutex>
#include <optional>
#include <sodium.h>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <v8.h>
#include <vector>

#if defined(_WIN32)
#include <bcrypt.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

#if defined(__APPLE__)
#include <mach/host_info.h>
#include <mach/mach.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <sys/sysinfo.h>
#endif

namespace fxe::runtime {

  namespace {
    thread_local const worker_bootstrap* tls_worker_bootstrap = nullptr;
  }

  worker_bootstrap_scope::worker_bootstrap_scope(const worker_bootstrap& bootstrap) noexcept
      : previous_(tls_worker_bootstrap) {
    tls_worker_bootstrap = &bootstrap;
  }

  worker_bootstrap_scope::~worker_bootstrap_scope() noexcept {
    tls_worker_bootstrap = previous_;
  }

  const worker_bootstrap* current_worker_bootstrap() noexcept {
    return tls_worker_bootstrap;
  }
  namespace {
    using namespace v8;

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    void set(Local<Context> ctx, Local<Object> obj, const char* key, Local<Value> value) {
      (void)obj->Set(ctx, str(Isolate::GetCurrent(), key), value);
    }

    void set_string(Local<Context> ctx, Local<Object> obj, const char* key,
                    std::string_view value) {
      set(ctx, obj, key, str(Isolate::GetCurrent(), value));
    }

    void set_number(Local<Context> ctx, Local<Object> obj, const char* key, double value) {
      set(ctx, obj, key, Number::New(Isolate::GetCurrent(), value));
    }

    void set_bool(Local<Context> ctx, Local<Object> obj, const char* key, bool value) {
      set(ctx, obj, key, Boolean::New(Isolate::GetCurrent(), value));
    }

    const char* getenv_or_empty(const char* name) {
      const char* value = std::getenv(name);
      return value ? value : "";
    }

    std::string home_dir() {
#if defined(_WIN32)
      const char* profile = std::getenv("USERPROFILE");
      if (profile && *profile)
        return profile;
      std::string drive = getenv_or_empty("HOMEDRIVE");
      std::string path = getenv_or_empty("HOMEPATH");
      if (!drive.empty() || !path.empty())
        return drive + path;
      return {};
#else
      const char* home = std::getenv("HOME");
      if (home && *home)
        return home;
      if (auto* pw = getpwuid(getuid()); pw && pw->pw_dir)
        return pw->pw_dir;
      return {};
#endif
    }

    std::string tmp_dir() {
#if defined(_WIN32)
      for (const char* name : {"TMP", "TEMP", "USERPROFILE"}) {
#else
      for (const char* name : {"TMPDIR", "TMP", "TEMP", "TEMPDIR"}) {
#endif
        const char* value = std::getenv(name);
        if (value && *value)
          return value;
      }
#if defined(_WIN32)
      return "C:\\Windows\\Temp";
#else
      return "/tmp";
#endif
    }

    std::string host_name() {
#if defined(_WIN32)
      char buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
      DWORD size = sizeof(buf);
      if (GetComputerNameA(buf, &size))
        return std::string(buf, size);
      return {};
#else
      char buf[256] = {};
      if (gethostname(buf, sizeof(buf) - 1) == 0)
        return buf;
      return {};
#endif
    }

    std::string os_release() {
#if defined(_WIN32)
      OSVERSIONINFOEXA info{};
      info.dwOSVersionInfoSize = sizeof(info);
#pragma warning(push)
#pragma warning(disable : 4996)
      if (GetVersionExA(reinterpret_cast<OSVERSIONINFOA*>(&info))) {
#pragma warning(pop)
        return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
               "." + std::to_string(info.dwBuildNumber);
      }
      return {};
#else
      struct utsname u{};
      if (uname(&u) == 0)
        return u.release;
      return {};
#endif
    }

    double os_uptime() {
#if defined(__APPLE__)
      timeval boot{};
      size_t size = sizeof(boot);
      int mib[2] = {CTL_KERN, KERN_BOOTTIME};
      if (sysctl(mib, 2, &boot, &size, nullptr, 0) == 0 && boot.tv_sec > 0) {
        timeval now{};
        gettimeofday(&now, nullptr);
        return static_cast<double>(now.tv_sec - boot.tv_sec) +
               static_cast<double>(now.tv_usec - boot.tv_usec) / 1000000.0;
      }
      return 0;
#elif defined(__linux__)
      struct sysinfo info{};
      return sysinfo(&info) == 0 ? static_cast<double>(info.uptime) : 0;
#elif defined(_WIN32)
    return static_cast<double>(GetTickCount64()) / 1000.0;
#else
    return 0;
#endif
    }

    double total_mem() {
#if defined(__APPLE__)
      std::uint64_t mem = 0;
      size_t size = sizeof(mem);
      return sysctlbyname("hw.memsize", &mem, &size, nullptr, 0) == 0 ? static_cast<double>(mem)
                                                                      : 0;
#elif defined(__linux__)
      struct sysinfo info{};
      if (sysinfo(&info) != 0)
        return 0;
      return static_cast<double>(info.totalram) * static_cast<double>(info.mem_unit);
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<double>(status.ullTotalPhys) : 0;
#else
    return 0;
#endif
    }

    double free_mem() {
#if defined(__APPLE__)
      mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
      vm_statistics64_data_t vmstat{};
      if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&vmstat), &count) != KERN_SUCCESS)
        return 0;
      std::uint64_t page_size = 0;
      size_t size = sizeof(page_size);
      if (sysctlbyname("hw.pagesize", &page_size, &size, nullptr, 0) != 0 || page_size == 0)
        page_size = 4096;
      const auto pages = static_cast<std::uint64_t>(vmstat.free_count) +
                         static_cast<std::uint64_t>(vmstat.inactive_count);
      return static_cast<double>(pages * page_size);
#elif defined(__linux__)
      struct sysinfo info{};
      if (sysinfo(&info) != 0)
        return 0;
      return static_cast<double>(info.freeram) * static_cast<double>(info.mem_unit);
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<double>(status.ullAvailPhys) : 0;
#else
    return 0;
#endif
    }

    std::string cpu_model() {
#if defined(__APPLE__)
      char model[256] = {};
      size_t size = sizeof(model);
      if (sysctlbyname("machdep.cpu.brand_string", model, &size, nullptr, 0) == 0 && model[0])
        return model;
      size = sizeof(model);
      if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && model[0])
        return model;
      return {};
#elif defined(_WIN32)
      return getenv_or_empty("PROCESSOR_IDENTIFIER");
#else
    return {};
#endif
    }

    int cpu_count() {
#if defined(_WIN32)
      SYSTEM_INFO info{};
      GetSystemInfo(&info);
      return static_cast<int>(info.dwNumberOfProcessors);
#else
      long n = sysconf(_SC_NPROCESSORS_ONLN);
      return n > 0 ? static_cast<int>(n) : 0;
#endif
    }

    int cpu_speed_mhz() {
#if defined(__APPLE__)
      std::uint64_t hz = 0;
      size_t size = sizeof(hz);
      if (sysctlbyname("hw.cpufrequency", &hz, &size, nullptr, 0) == 0 && hz > 0)
        return static_cast<int>(hz / 1000000);
#endif
      return 0;
    }

#if !defined(_WIN32)
    std::string sockaddr_to_numeric(const sockaddr* addr) {
      if (!addr)
        return {};
      char host[NI_MAXHOST] = {};
      const auto len = addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
      if (getnameinfo(addr, static_cast<socklen_t>(len), host, sizeof(host), nullptr, 0,
                      NI_NUMERICHOST) == 0)
        return host;
      return {};
    }
#endif

#if !defined(_WIN32)
    extern "C" char** environ;

    struct child_process_handle {
      pid_t pid = -1;
      int stdin_fd = -1;
      int stdout_fd = -1;
      int stderr_fd = -1;
      bool exited = false;
      int status = 0;
      Global<Object> self_persistent;
    };

    void close_fd(int& fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }

    void close_child_fds(child_process_handle* h) {
      if (!h)
        return;
      close_fd(h->stdin_fd);
      close_fd(h->stdout_fd);
      close_fd(h->stderr_fd);
    }

    void child_finalizer(const WeakCallbackInfo<child_process_handle>& info) {
      auto* h = info.GetParameter();
      if (h) {
        h->self_persistent.Reset();
        close_child_fds(h);
        delete h;
      }
    }

    child_process_handle* child_handle_from(Local<Object> self) {
      if (self->InternalFieldCount() < 1)
        return nullptr;
      auto field = self->GetInternalField(0);
      return static_cast<child_process_handle*>(
          field.As<External>()->Value(v8::kExternalPointerTypeTagDefault));
    }

    bool make_pipe(int fds[2]) {
      if (::pipe(fds) != 0)
        return false;
      for (int i = 0; i < 2; ++i) {
        (void)::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
      }
      return true;
    }

    void set_nonblocking(int fd) {
      if (fd < 0)
        return;
      int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    std::string read_available(int& fd) {
      std::string out;
      if (fd < 0)
        return out;
      std::array<char, 4096> buf{};
      for (;;) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
          out.append(buf.data(), static_cast<std::size_t>(n));
          continue;
        }
        if (n == 0) {
          close_fd(fd);
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        close_fd(fd);
        break;
      }
      return out;
    }

    void child_read_stdout(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      info.GetReturnValue().Set(str(info.GetIsolate(), h ? read_available(h->stdout_fd) : ""));
    }

    void child_read_stderr(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      info.GetReturnValue().Set(str(info.GetIsolate(), h ? read_available(h->stderr_fd) : ""));
    }

    void child_write_stdin(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = child_handle_from(info.This());
      if (!h || h->stdin_fd < 0) {
        info.GetReturnValue().Set(Boolean::New(iso, false));
        return;
      }
      if (info.Length() < 1) {
        info.GetReturnValue().Set(Boolean::New(iso, true));
        return;
      }
      String::Utf8Value data(iso, info[0]);
      const char* p = *data ? *data : "";
      std::size_t left = static_cast<std::size_t>(data.length());
      while (left > 0) {
        ssize_t n = ::write(h->stdin_fd, p, left);
        if (n > 0) {
          p += n;
          left -= static_cast<std::size_t>(n);
          continue;
        }
        if (n < 0 && errno == EINTR)
          continue;
        iso->ThrowException(Exception::Error(str(iso, "child_process stdin write failed")));
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, true));
    }

    void child_end_stdin(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      if (h)
        close_fd(h->stdin_fd);
      info.GetReturnValue().Set(Boolean::New(info.GetIsolate(), true));
    }

    void child_kill(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = child_handle_from(info.This());
      if (!h || h->pid <= 0 || h->exited) {
        info.GetReturnValue().Set(Boolean::New(iso, false));
        return;
      }
      int sig = SIGTERM;
      if (info.Length() > 0) {
        if (info[0]->IsNumber()) {
          sig = info[0]->Int32Value(ctx).FromMaybe(SIGTERM);
        } else if (info[0]->IsString()) {
          String::Utf8Value name(iso, info[0]);
          std::string s(*name ? *name : "");
          if (s == "SIGKILL")
            sig = SIGKILL;
          else if (s == "SIGINT")
            sig = SIGINT;
          else
            sig = SIGTERM;
        }
      }
      info.GetReturnValue().Set(Boolean::New(iso, ::kill(h->pid, sig) == 0));
    }

    Local<Value> make_wait_result(Isolate* iso, Local<Context> ctx, child_process_handle* h) {
      if (!h)
        return Null(iso);
      if (!h->exited) {
        int status = 0;
        pid_t r = ::waitpid(h->pid, &status, WNOHANG);
        if (r == 0)
          return Null(iso);
        if (r < 0) {
          if (errno == ECHILD) {
            h->exited = true;
            h->status = 0;
          } else {
            return Null(iso);
          }
        } else {
          h->exited = true;
          h->status = status;
        }
      }
      auto out = Object::New(iso);
      if (WIFEXITED(h->status)) {
        set(ctx, out, "exitCode", Integer::New(iso, WEXITSTATUS(h->status)));
        set(ctx, out, "signal", Null(iso));
      } else if (WIFSIGNALED(h->status)) {
        set(ctx, out, "exitCode", Null(iso));
        set(ctx, out, "signal", Integer::New(iso, WTERMSIG(h->status)));
      } else {
        set(ctx, out, "exitCode", Null(iso));
        set(ctx, out, "signal", Null(iso));
      }
      return out;
    }

    void child_wait(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      info.GetReturnValue().Set(make_wait_result(iso, ctx, child_handle_from(info.This())));
    }

    void child_sleep_ms(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int ms = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(0) : 0;
      if (ms > 0)
        ::usleep(static_cast<useconds_t>(ms) * 1000);
      info.GetReturnValue().Set(Boolean::New(iso, true));
    }

    std::vector<std::string> string_array_arg(Isolate* iso, Local<Context> ctx,
                                              Local<Value> value) {
      std::vector<std::string> out;
      if (!value->IsArray())
        return out;
      auto array = value.As<Array>();
      const uint32_t len = array->Length();
      out.reserve(len);
      for (uint32_t i = 0; i < len; ++i) {
        Local<Value> item;
        if (!array->Get(ctx, i).ToLocal(&item))
          continue;
        String::Utf8Value s(iso, item);
        out.emplace_back(*s ? *s : "");
      }
      return out;
    }

    void spawn_spawn(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(Exception::TypeError(
            str(iso, "__fxe_native.spawn.spawn(file, args, opts) requires file")));
        return;
      }

      String::Utf8Value file_value(iso, info[0]);
      std::string file(*file_value ? *file_value : "");
      auto args =
          info.Length() > 1 ? string_array_arg(iso, ctx, info[1]) : std::vector<std::string>{};

      int stdin_pipe[2] = {-1, -1};
      int stdout_pipe[2] = {-1, -1};
      int stderr_pipe[2] = {-1, -1};
      if (!make_pipe(stdin_pipe) || !make_pipe(stdout_pipe) || !make_pipe(stderr_pipe)) {
        int err = errno;
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        iso->ThrowException(
            Exception::Error(str(iso, std::string("pipe failed: ") + std::strerror(err))));
        return;
      }

      posix_spawn_file_actions_t actions;
      int rc = posix_spawn_file_actions_init(&actions);
      if (rc == 0)
        rc = posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
      if (rc == 0)
        rc = posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
      if (rc == 0)
        rc = posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
      if (rc == 0) {
        (void)posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
        (void)posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
        (void)posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
      }
      if (rc != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        iso->ThrowException(Exception::Error(
            str(iso, std::string("posix_spawn file actions failed: ") + std::strerror(rc))));
        return;
      }

      std::vector<char*> argv;
      argv.reserve(args.size() + 2);
      argv.push_back(file.data());
      for (auto& arg : args)
        argv.push_back(arg.data());
      argv.push_back(nullptr);

      pid_t pid = -1;
      rc = ::posix_spawnp(&pid, file.c_str(), &actions, nullptr, argv.data(), environ);
      (void)posix_spawn_file_actions_destroy(&actions);
      close_fd(stdin_pipe[0]);
      close_fd(stdout_pipe[1]);
      close_fd(stderr_pipe[1]);

      if (rc != 0) {
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stderr_pipe[0]);
        iso->ThrowException(
            Exception::Error(str(iso, std::string("spawn failed: ") + std::strerror(rc))));
        return;
      }

      set_nonblocking(stdin_pipe[1]);
      set_nonblocking(stdout_pipe[0]);
      set_nonblocking(stderr_pipe[0]);
      auto* h = new child_process_handle{
          pid, stdin_pipe[1], stdout_pipe[0], stderr_pipe[0], false, 0, {}};
      auto tpl = ObjectTemplate::New(iso);
      tpl->SetInternalFieldCount(1);
      tpl->Set(iso, "readStdout", FunctionTemplate::New(iso, child_read_stdout));
      tpl->Set(iso, "readStderr", FunctionTemplate::New(iso, child_read_stderr));
      tpl->Set(iso, "writeStdin", FunctionTemplate::New(iso, child_write_stdin));
      tpl->Set(iso, "endStdin", FunctionTemplate::New(iso, child_end_stdin));
      tpl->Set(iso, "kill", FunctionTemplate::New(iso, child_kill));
      tpl->Set(iso, "wait", FunctionTemplate::New(iso, child_wait));
      tpl->Set(iso, "sleep", FunctionTemplate::New(iso, child_sleep_ms));
      auto obj = tpl->NewInstance(ctx).ToLocalChecked();
      obj->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      set(ctx, obj, "pid", Integer::New(iso, pid));
      h->self_persistent.Reset(iso, obj);
      h->self_persistent.SetWeak(h, child_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(obj);
    }
#else
    struct child_process_handle {
      PROCESS_INFORMATION process{};
      HANDLE stdin_write = nullptr;
      HANDLE stdout_read = nullptr;
      HANDLE stderr_read = nullptr;
      bool exited = false;
      DWORD exit_code = 0;
      Global<Object> self_persistent;
    };

    std::wstring widen_utf8(const std::string& value) {
      if (value.empty())
        return {};
      const int needed =
          MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
      std::wstring out(static_cast<std::size_t>(needed), L'\0');
      (void)MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                out.data(), needed);
      return out;
    }

    std::wstring quote_windows_arg(const std::string& arg) {
      if (arg.empty())
        return L"\"\"";
      bool needs_quotes = false;
      for (char c : arg) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
          needs_quotes = true;
          break;
        }
      }
      std::wstring w = widen_utf8(arg);
      if (!needs_quotes)
        return w;
      std::wstring out = L"\"";
      unsigned backslashes = 0;
      for (wchar_t c : w) {
        if (c == L'\\') {
          ++backslashes;
          continue;
        }
        if (c == L'"') {
          out.append(backslashes * 2 + 1, L'\\');
          out.push_back(c);
          backslashes = 0;
          continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
      }
      out.append(backslashes * 2, L'\\');
      out.push_back(L'"');
      return out;
    }

    bool windows_path_has_separator(const std::string& path) {
      return path.find('\\') != std::string::npos || path.find('/') != std::string::npos ||
             path.find(':') != std::string::npos;
    }

    std::vector<std::string> string_array_arg(Isolate* iso, Local<Context> ctx,
                                              Local<Value> value) {
      std::vector<std::string> out;
      if (!value->IsArray())
        return out;
      auto array = value.As<Array>();
      const uint32_t len = array->Length();
      out.reserve(len);
      for (uint32_t i = 0; i < len; ++i) {
        Local<Value> item;
        if (!array->Get(ctx, i).ToLocal(&item))
          continue;
        String::Utf8Value s(iso, item);
        out.emplace_back(*s ? *s : "");
      }
      return out;
    }

    void close_handle(HANDLE& h) {
      if (h) {
        CloseHandle(h);
        h = nullptr;
      }
    }

    void close_child_fds(child_process_handle* h) {
      if (!h)
        return;
      close_handle(h->stdin_write);
      close_handle(h->stdout_read);
      close_handle(h->stderr_read);
      close_handle(h->process.hProcess);
      close_handle(h->process.hThread);
    }

    void child_finalizer(const WeakCallbackInfo<child_process_handle>& info) {
      auto* h = info.GetParameter();
      if (h) {
        h->self_persistent.Reset();
        close_child_fds(h);
        delete h;
      }
    }

    child_process_handle* child_handle_from(Local<Object> self) {
      if (self->InternalFieldCount() < 1)
        return nullptr;
      auto field = self->GetInternalField(0);
      return static_cast<child_process_handle*>(
          field.As<External>()->Value(v8::kExternalPointerTypeTagDefault));
    }

    std::string read_pipe_available(HANDLE h) {
      if (!h)
        return {};
      DWORD available = 0;
      if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr) || available == 0)
        return {};
      std::string out;
      out.resize(static_cast<std::size_t>(available));
      DWORD read = 0;
      if (!ReadFile(h, out.data(), available, &read, nullptr))
        return {};
      out.resize(static_cast<std::size_t>(read));
      return out;
    }

    void child_read_stdout(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      info.GetReturnValue().Set(
          str(info.GetIsolate(), h ? read_pipe_available(h->stdout_read) : ""));
    }

    void child_read_stderr(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      info.GetReturnValue().Set(
          str(info.GetIsolate(), h ? read_pipe_available(h->stderr_read) : ""));
    }

    void child_write_stdin(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = child_handle_from(info.This());
      if (!h || !h->stdin_write) {
        info.GetReturnValue().Set(Boolean::New(iso, false));
        return;
      }
      String::Utf8Value data(iso, info.Length() > 0 ? info[0] : Undefined(iso));
      DWORD written = 0;
      const BOOL ok = WriteFile(h->stdin_write, *data ? *data : "",
                                static_cast<DWORD>(data.length()), &written, nullptr);
      if (!ok) {
        iso->ThrowException(Exception::Error(str(iso, "child_process stdin write failed")));
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, true));
    }

    void child_end_stdin(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      if (h)
        close_handle(h->stdin_write);
      info.GetReturnValue().Set(Boolean::New(info.GetIsolate(), true));
    }

    void child_kill(const FunctionCallbackInfo<Value>& info) {
      auto* h = child_handle_from(info.This());
      const bool ok =
          h && h->process.hProcess && !h->exited && TerminateProcess(h->process.hProcess, 1);
      info.GetReturnValue().Set(Boolean::New(info.GetIsolate(), ok));
    }

    Local<Value> make_wait_result(Isolate* iso, Local<Context> ctx, child_process_handle* h) {
      if (!h || !h->process.hProcess)
        return Null(iso);
      if (!h->exited) {
        const DWORD wait = WaitForSingleObject(h->process.hProcess, 0);
        if (wait == WAIT_TIMEOUT)
          return Null(iso);
        DWORD code = 0;
        if (!GetExitCodeProcess(h->process.hProcess, &code))
          return Null(iso);
        h->exit_code = code;
        h->exited = true;
      }
      auto out = Object::New(iso);
      set(ctx, out, "exitCode", Integer::New(iso, static_cast<int32_t>(h->exit_code)));
      set(ctx, out, "signal", Null(iso));
      return out;
    }

    void child_wait(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      info.GetReturnValue().Set(make_wait_result(iso, ctx, child_handle_from(info.This())));
    }

    void child_sleep_ms(const FunctionCallbackInfo<Value>& info) {
      const int ms = info.Length() > 0
                         ? info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0)
                         : 0;
      if (ms > 0)
        Sleep(static_cast<DWORD>(ms));
      info.GetReturnValue().Set(Boolean::New(info.GetIsolate(), true));
    }

    bool make_inheritable_pipe(HANDLE& read_handle, HANDLE& write_handle, bool child_reads) {
      SECURITY_ATTRIBUTES sa{};
      sa.nLength = sizeof(sa);
      sa.bInheritHandle = TRUE;
      if (!CreatePipe(&read_handle, &write_handle, &sa, 0))
        return false;
      HANDLE parent_handle = child_reads ? write_handle : read_handle;
      return SetHandleInformation(parent_handle, HANDLE_FLAG_INHERIT, 0) != 0;
    }

    void spawn_spawn(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(Exception::TypeError(
            str(iso, "__fxe_native.spawn.spawn(file, args, opts) requires file")));
        return;
      }
      String::Utf8Value file_value(iso, info[0]);
      std::string file(*file_value ? *file_value : "");
      auto args =
          info.Length() > 1 ? string_array_arg(iso, ctx, info[1]) : std::vector<std::string>{};

      HANDLE stdin_read = nullptr;
      HANDLE stdin_write = nullptr;
      HANDLE stdout_read = nullptr;
      HANDLE stdout_write = nullptr;
      HANDLE stderr_read = nullptr;
      HANDLE stderr_write = nullptr;
      if (!make_inheritable_pipe(stdin_read, stdin_write, true) ||
          !make_inheritable_pipe(stdout_read, stdout_write, false) ||
          !make_inheritable_pipe(stderr_read, stderr_write, false)) {
        close_handle(stdin_read);
        close_handle(stdin_write);
        close_handle(stdout_read);
        close_handle(stdout_write);
        close_handle(stderr_read);
        close_handle(stderr_write);
        iso->ThrowException(Exception::Error(str(iso, "CreatePipe failed")));
        return;
      }

      std::wstring command = quote_windows_arg(file);
      for (const auto& arg : args) {
        command.push_back(L' ');
        command += quote_windows_arg(arg);
      }
      std::wstring application;
      const wchar_t* application_ptr = nullptr;
      if (windows_path_has_separator(file)) {
        application = widen_utf8(file);
        application_ptr = application.empty() ? nullptr : application.c_str();
      }
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = stdin_read;
      startup.hStdOutput = stdout_write;
      startup.hStdError = stderr_write;
      PROCESS_INFORMATION pi{};
      BOOL ok = CreateProcessW(application_ptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                               nullptr, &startup, &pi);
      close_handle(stdin_read);
      close_handle(stdout_write);
      close_handle(stderr_write);
      if (!ok) {
        close_handle(stdin_write);
        close_handle(stdout_read);
        close_handle(stderr_read);
        iso->ThrowException(Exception::Error(str(iso, "CreateProcessW failed")));
        return;
      }

      auto* h = new child_process_handle{pi, stdin_write, stdout_read, stderr_read, false, 0, {}};
      auto tpl = ObjectTemplate::New(iso);
      tpl->SetInternalFieldCount(1);
      tpl->Set(iso, "readStdout", FunctionTemplate::New(iso, child_read_stdout));
      tpl->Set(iso, "readStderr", FunctionTemplate::New(iso, child_read_stderr));
      tpl->Set(iso, "writeStdin", FunctionTemplate::New(iso, child_write_stdin));
      tpl->Set(iso, "endStdin", FunctionTemplate::New(iso, child_end_stdin));
      tpl->Set(iso, "kill", FunctionTemplate::New(iso, child_kill));
      tpl->Set(iso, "wait", FunctionTemplate::New(iso, child_wait));
      tpl->Set(iso, "sleep", FunctionTemplate::New(iso, child_sleep_ms));
      auto obj = tpl->NewInstance(ctx).ToLocalChecked();
      obj->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      set(ctx, obj, "pid", Integer::New(iso, static_cast<int32_t>(pi.dwProcessId)));
      h->self_persistent.Reset(iso, obj);
      h->self_persistent.SetWeak(h, child_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(obj);
    }
#endif

    struct byte_view {
      const std::uint8_t* data = nullptr;
      std::size_t size = 0;
      std::vector<std::uint8_t> owned;
    };

    void throw_error(Isolate* iso, std::string_view message) {
      iso->ThrowException(Exception::Error(str(iso, message)));
    }

    void throw_error(Isolate* iso, const char* message) {
      throw_error(iso, std::string_view(message));
    }

    void throw_error(Isolate* iso, const std::string& message) {
      throw_error(iso, std::string_view(message));
    }

    Local<Value> make_permission_denied(Isolate* iso, std::string_view what) {
      auto ctx = iso->GetCurrentContext();
      std::string msg = "Permission denied: ";
      msg.append(what);
      auto err = Exception::Error(str(iso, msg)).As<Object>();
      (void)err->Set(ctx, str(iso, "name"), str(iso, "PermissionDenied"));
      return err;
    }

    bool value_to_bytes(Isolate* iso, Local<Context> ctx, Local<Value> value, byte_view& out) {
      if (value->IsString()) {
        String::Utf8Value utf8(iso, value);
        if (*utf8 == nullptr)
          return false;
        out.owned.assign(reinterpret_cast<const std::uint8_t*>(*utf8),
                         reinterpret_cast<const std::uint8_t*>(*utf8) + utf8.length());
        out.data = out.owned.data();
        out.size = out.owned.size();
        return true;
      }
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        out.data = static_cast<const std::uint8_t*>(backing->Data()) + view->ByteOffset();
        out.size = view->ByteLength();
        return true;
      }
      if (value->IsArrayBuffer()) {
        auto buffer = value.As<ArrayBuffer>();
        auto backing = buffer->GetBackingStore();
        out.data = static_cast<const std::uint8_t*>(backing->Data());
        out.size = backing->ByteLength();
        return true;
      }
      (void)ctx;
      return false;
    }
    bool read_bytes_property(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                             byte_view& out, bool required) {
      auto v = obj->Get(ctx, str(iso, key));
      if (v.IsEmpty()) {
        if (required) {
          throw_error(iso, std::string("missing pk component '") + key + "'");
          return false;
        }
        out = byte_view{};
        return true;
      }
      auto val = v.ToLocalChecked();
      if (val->IsUndefined() || val->IsNull()) {
        if (required) {
          throw_error(iso, std::string("missing pk component '") + key + "'");
          return false;
        }
        out = byte_view{};
        return true;
      }
      if (!value_to_bytes(iso, ctx, val, out)) {
        iso->ThrowException(Exception::TypeError(
            str(iso, std::string("pk component '") + key + "' must be a Uint8Array")));
        return false;
      }
      return true;
    }

    std::string read_string_property(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                     const char* key) {
      auto v = obj->Get(ctx, str(iso, key));
      if (v.IsEmpty())
        return {};
      auto val = v.ToLocalChecked();
      if (!val->IsString())
        return {};
      String::Utf8Value utf8(iso, val);
      return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
    }

    std::string mbedtls_err_str(int rc) {
      char buf[160];
      mbedtls_strerror(rc, buf, sizeof(buf));
      return std::string(buf);
    }

    bool fill_secure_random(std::uint8_t* data, std::size_t size,
                            [[maybe_unused]] std::string& error) {
      if (size == 0)
        return true;
#if defined(_WIN32)
      while (size > 0) {
        const auto chunk =
            static_cast<ULONG>(std::min<std::size_t>(size, std::numeric_limits<ULONG>::max()));
        const NTSTATUS status =
            BCryptGenRandom(nullptr, data, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
          error = "BCryptGenRandom failed";
          return false;
        }
        data += chunk;
        size -= chunk;
      }
      return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__)
      arc4random_buf(data, size);
      return true;
#elif defined(__linux__)
    std::size_t done = 0;
    while (done < size) {
      const std::size_t want = std::min<std::size_t>(
          size - done, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const ssize_t n = ::getrandom(data + done, want, 0);
      if (n > 0) {
        done += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
    if (done == size)
      return true;

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open("/dev/urandom", flags);
    if (fd < 0) {
      error = "failed to open /dev/urandom";
      return false;
    }
    while (done < size) {
      const std::size_t want = std::min<std::size_t>(
          size - done, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const ssize_t n = ::read(fd, data + done, want);
      if (n > 0) {
        done += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      error = "failed to read /dev/urandom";
      ::close(fd);
      return false;
    }
    ::close(fd);
    return true;
#else
    error = "secure random source is not implemented on this platform";
    return false;
#endif
    }

    void random_fill(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsUint8Array()) {
        throw_error(iso, "__fxe_native.random.fill requires a Uint8Array");
        return;
      }
      auto view = info[0].As<Uint8Array>();
      auto backing = view->Buffer()->GetBackingStore();
      auto* data = static_cast<std::uint8_t*>(backing->Data()) + view->ByteOffset();
      std::string error;
      if (!fill_secure_random(data, view->ByteLength(), error)) {
        throw_error(iso, error.empty() ? "secure random fill failed" : error);
        return;
      }
      (void)ctx;
      info.GetReturnValue().Set(view);
    }

    void ensure_sodium_initialized() {
      static std::once_flag flag;
      std::call_once(flag, []() {
        if (sodium_init() < 0)
          std::abort();
      });
    }

    struct mbedtls_rng_state {
      mbedtls_entropy_context entropy;
      mbedtls_ctr_drbg_context ctr_drbg;
      bool ready = false;

      ~mbedtls_rng_state() {
        if (ready) {
          mbedtls_ctr_drbg_free(&ctr_drbg);
          mbedtls_entropy_free(&entropy);
        }
      }
    };

    mbedtls_rng_state& mbedtls_rng() {
      static mbedtls_rng_state state;
      static std::once_flag flag;
      std::call_once(flag, [&]() {
        mbedtls_entropy_init(&state.entropy);
        mbedtls_ctr_drbg_init(&state.ctr_drbg);
        static const unsigned char personalization[] = "fxe_native_pk";
        if (mbedtls_ctr_drbg_seed(&state.ctr_drbg, mbedtls_entropy_func, &state.entropy,
                                  personalization, sizeof(personalization) - 1) != 0) {
          std::abort();
        }
        state.ready = true;
      });
      return state;
    }

    mbedtls_md_type_t hash_type_for(std::string_view algo) {
      std::string normalized;
      normalized.reserve(algo.size());
      for (char c : algo) {
        if (c != '-' && c != '_')
          normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      if (normalized == "sha1")
        return MBEDTLS_MD_SHA1;
      if (normalized == "sha256")
        return MBEDTLS_MD_SHA256;
      if (normalized == "sha384")
        return MBEDTLS_MD_SHA384;
      if (normalized == "sha512")
        return MBEDTLS_MD_SHA512;
      if (normalized == "md5")
        return MBEDTLS_MD_MD5;
      return MBEDTLS_MD_NONE;
    }

    bool ecp_group_id_for(std::string_view curve, mbedtls_ecp_group_id& out) {
      std::string n;
      n.reserve(curve.size());
      for (char c : curve) {
        if (c != '-' && c != '_')
          n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      if (n == "p256" || n == "secp256r1" || n == "prime256v1") {
        out = MBEDTLS_ECP_DP_SECP256R1;
        return true;
      }
      if (n == "p384" || n == "secp384r1") {
        out = MBEDTLS_ECP_DP_SECP384R1;
        return true;
      }
      if (n == "p521" || n == "secp521r1") {
        out = MBEDTLS_ECP_DP_SECP521R1;
        return true;
      }
      return false;
    }

    std::string_view canonical_curve_name(mbedtls_ecp_group_id id) {
      switch (id) {
      case MBEDTLS_ECP_DP_SECP256R1:
        return "P-256";
      case MBEDTLS_ECP_DP_SECP384R1:
        return "P-384";
      case MBEDTLS_ECP_DP_SECP521R1:
        return "P-521";
      default:
        return "";
      }
    }

    std::size_t coord_size_bytes(mbedtls_ecp_group_id id) {
      switch (id) {
      case MBEDTLS_ECP_DP_SECP256R1:
        return 32;
      case MBEDTLS_ECP_DP_SECP384R1:
        return 48;
      case MBEDTLS_ECP_DP_SECP521R1:
        return 66;
      default:
        return 0;
      }
    }

    struct hash_state {
      mbedtls_md_context_t ctx;
      bool freed = false;
      std::size_t digest_size = 0;
      bool hmac = false;
      Global<Object> self;

      hash_state() {
        mbedtls_md_init(&ctx);
      }
      ~hash_state() {
        if (!freed)
          mbedtls_md_free(&ctx);
      }
    };

    hash_state* get_hash_state(const FunctionCallbackInfo<Value>& info) {
      return static_cast<hash_state*>(
          info.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
    }

    void hash_finalizer(const WeakCallbackInfo<hash_state>& data) {
      auto* state = data.GetParameter();
      state->self.Reset();
      delete state;
    }

    void hash_update(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_hash_state(info);
      if (!state || state->freed) {
        throw_error(iso, "hash context has already been digested");
        return;
      }
      if (info.Length() < 1) {
        throw_error(iso, "hash.update requires data");
        return;
      }
      byte_view bytes;
      if (!value_to_bytes(iso, iso->GetCurrentContext(), info[0], bytes)) {
        throw_error(iso, "hash.update requires a string, ArrayBuffer, or typed array");
        return;
      }
      if (bytes.size > 0) {
        const int rc = state->hmac ? mbedtls_md_hmac_update(&state->ctx, bytes.data, bytes.size)
                                   : mbedtls_md_update(&state->ctx, bytes.data, bytes.size);
        if (rc != 0) {
          throw_error(iso, state->hmac ? "hmac.update failed" : "hash.update failed");
          return;
        }
      }
      info.GetReturnValue().Set(info.This());
    }

    void hash_digest(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_hash_state(info);
      if (!state || state->freed) {
        throw_error(iso, "hash context has already been digested");
        return;
      }
      const auto size = state->digest_size;
      auto backing = ArrayBuffer::NewBackingStore(iso, size);
      const int rc =
          state->hmac
              ? mbedtls_md_hmac_finish(&state->ctx, static_cast<unsigned char*>(backing->Data()))
              : mbedtls_md_finish(&state->ctx, static_cast<unsigned char*>(backing->Data()));
      if (rc != 0) {
        throw_error(iso, state->hmac ? "hmac.digest failed" : "hash.digest failed");
        return;
      }
      mbedtls_md_free(&state->ctx);
      state->freed = true;
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      info.GetReturnValue().Set(Uint8Array::New(buffer, 0, size));
    }

    void hash_create(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.hash.create requires an algorithm string");
        return;
      }
      String::Utf8Value algo_utf8(iso, info[0]);
      const auto type = hash_type_for(*algo_utf8 ? std::string_view(*algo_utf8, algo_utf8.length())
                                                 : std::string_view{});
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(type);
      if (md == nullptr) {
        throw_error(iso, "unsupported hash algorithm");
        return;
      }
      auto state = std::make_unique<hash_state>();
      if (mbedtls_md_setup(&state->ctx, md, 0) != 0 || mbedtls_md_starts(&state->ctx) != 0) {
        throw_error(iso, "hash initialization failed");
        return;
      }
      state->digest_size = mbedtls_md_get_size(md);
      auto out = Object::New(iso);
      auto external = External::New(iso, state.get(), v8::kExternalPointerTypeTagDefault);
      (void)out->Set(ctx, str(iso, "update"),
                     Function::New(ctx, hash_update, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "digest"),
                     Function::New(ctx, hash_digest, external).ToLocalChecked());
      state->self.Reset(iso, out);
      state->self.SetWeak(state.get(), hash_finalizer, WeakCallbackType::kParameter);
      (void)state.release();
      info.GetReturnValue().Set(out);
    }

    void hmac_create(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.hash.createHmac requires algorithm and key");
        return;
      }
      String::Utf8Value algo_utf8(iso, info[0]);
      const auto type = hash_type_for(*algo_utf8 ? std::string_view(*algo_utf8, algo_utf8.length())
                                                 : std::string_view{});
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(type);
      if (md == nullptr) {
        throw_error(iso, "unsupported hmac algorithm");
        return;
      }
      byte_view key;
      if (!value_to_bytes(iso, ctx, info[1], key)) {
        throw_error(iso, "hmac key must be a string, ArrayBuffer, or typed array");
        return;
      }
      auto state = std::make_unique<hash_state>();
      if (mbedtls_md_setup(&state->ctx, md, 1) != 0 ||
          mbedtls_md_hmac_starts(&state->ctx, key.data, key.size) != 0) {
        throw_error(iso, "hmac initialization failed");
        return;
      }
      state->digest_size = mbedtls_md_get_size(md);
      state->hmac = true;
      auto out = Object::New(iso);
      auto external = External::New(iso, state.get(), v8::kExternalPointerTypeTagDefault);
      (void)out->Set(ctx, str(iso, "update"),
                     Function::New(ctx, hash_update, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "digest"),
                     Function::New(ctx, hash_digest, external).ToLocalChecked());
      state->self.Reset(iso, out);
      state->self.SetWeak(state.get(), hash_finalizer, WeakCallbackType::kParameter);
      (void)state.release();
      info.GetReturnValue().Set(out);
    }

    void pbkdf2_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 5 || !info[2]->IsNumber() || !info[3]->IsNumber() ||
          !info[4]->IsString()) {
        throw_error(
            iso,
            "__fxe_native.hash.pbkdf2Sync requires password, salt, iterations, keylen, digest");
        return;
      }
      byte_view password;
      byte_view salt;
      if (!value_to_bytes(iso, ctx, info[0], password) ||
          !value_to_bytes(iso, ctx, info[1], salt)) {
        throw_error(iso, "pbkdf2 password and salt must be strings, ArrayBuffers, or typed arrays");
        return;
      }
      const int iterations = info[2]->Int32Value(ctx).FromMaybe(0);
      const int keylen = info[3]->Int32Value(ctx).FromMaybe(0);
      if (iterations <= 0 || keylen < 0) {
        throw_error(iso, "pbkdf2 iterations must be positive and keylen non-negative");
        return;
      }
      String::Utf8Value digest_utf8(iso, info[4]);
      const auto type = hash_type_for(
          *digest_utf8 ? std::string_view(*digest_utf8, digest_utf8.length()) : std::string_view{});
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(type);
      if (md == nullptr) {
        throw_error(iso, "unsupported pbkdf2 digest");
        return;
      }
      auto backing = ArrayBuffer::NewBackingStore(iso, static_cast<std::size_t>(keylen));
      const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(type, password.data, password.size, salt.data,
                                                   salt.size, static_cast<unsigned int>(iterations),
                                                   static_cast<std::uint32_t>(keylen),
                                                   static_cast<unsigned char*>(backing->Data()));
      if (rc != 0) {
        throw_error(iso, "pbkdf2 derivation failed");
        return;
      }
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      info.GetReturnValue().Set(Uint8Array::New(buffer, 0, static_cast<std::size_t>(keylen)));
    }

    void kdf_scrypt_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 6) {
        throw_error(iso, "__fxe_native.kdf.scryptSync requires password, salt, N, r, p, keylen");
        return;
      }
      byte_view password;
      byte_view salt;
      if (!value_to_bytes(iso, ctx, info[0], password) ||
          !value_to_bytes(iso, ctx, info[1], salt)) {
        throw_error(iso, "scrypt password and salt must be strings, ArrayBuffers, or typed arrays");
        return;
      }

      // N: power of two >= 2, fits in uint64_t (libsodium uses uint64_t).
      // r, p: positive uint32_t.
      // keylen: positive size_t.
      // Validate ranges before calling the FFI to avoid surfacing libsodium's EINVAL as opaque.
      Local<Value> n_val = info[2], r_val = info[3], p_val = info[4], keylen_val = info[5];
      if (!n_val->IsNumber() || !r_val->IsNumber() || !p_val->IsNumber() ||
          !keylen_val->IsNumber()) {
        iso->ThrowException(
            Exception::TypeError(str(iso, "scrypt N, r, p, and keylen must be numbers")));
        return;
      }
      const double n_d = n_val->NumberValue(ctx).FromMaybe(-1.0);
      const double r_d = r_val->NumberValue(ctx).FromMaybe(-1.0);
      const double p_d = p_val->NumberValue(ctx).FromMaybe(-1.0);
      const double keylen_d = keylen_val->NumberValue(ctx).FromMaybe(-1.0);

      // RFC 7914 + libsodium parameter constraints:
      //   N is a power of two strictly greater than 1, fits in uint64_t.
      //   r >= 1, p >= 1, both fit in uint32_t.
      //   keylen >= 1 (we reject 0 — Node's scryptSync also rejects 0 keylen).
      // Upper bounds: cap N to 2^31 (1 << 31), r/p to 2^24 to keep memory bounded
      // (libsodium itself enforces 128 * N * r <= some platform-dependent limit).
      auto bad_range = [&](const char* msg) {
        iso->ThrowException(Exception::RangeError(str(iso, msg)));
      };
      if (!(n_d >= 2.0) || std::floor(n_d) != n_d || n_d > static_cast<double>(1ull << 31)) {
        bad_range("scrypt N must be a power of two between 2 and 2^31");
        return;
      }
      const std::uint64_t n_u64 = static_cast<std::uint64_t>(n_d);
      if ((n_u64 & (n_u64 - 1)) != 0) {
        bad_range("scrypt N must be a power of two");
        return;
      }
      if (!(r_d >= 1.0) || std::floor(r_d) != r_d || r_d > static_cast<double>(0x00ffffffu)) {
        bad_range("scrypt r must be an integer in [1, 2^24]");
        return;
      }
      if (!(p_d >= 1.0) || std::floor(p_d) != p_d || p_d > static_cast<double>(0x00ffffffu)) {
        bad_range("scrypt p must be an integer in [1, 2^24]");
        return;
      }
      if (!(keylen_d >= 1.0) || std::floor(keylen_d) != keylen_d ||
          keylen_d > static_cast<double>(static_cast<std::size_t>(-1) / 2)) {
        bad_range("scrypt keylen must be a positive integer");
        return;
      }

      const std::uint32_t r_u32 = static_cast<std::uint32_t>(r_d);
      const std::uint32_t p_u32 = static_cast<std::uint32_t>(p_d);
      const std::size_t keylen = static_cast<std::size_t>(keylen_d);

      ensure_sodium_initialized();

      auto backing = ArrayBuffer::NewBackingStore(iso, keylen);
      auto* out = static_cast<unsigned char*>(backing->Data());
      const int rc = crypto_pwhash_scryptsalsa208sha256_ll(
          password.data, password.size, salt.data, salt.size, n_u64, r_u32, p_u32, out, keylen);
      if (rc != 0) {
        throw_error(iso, "scrypt derivation failed");
        return;
      }
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      info.GetReturnValue().Set(Uint8Array::New(buffer, 0, keylen));
    }

    std::string cipher_name_for(std::string_view algo) {
      std::string normalized;
      normalized.reserve(algo.size());
      for (char c : algo) {
        if (c == '_' || c == '-')
          normalized.push_back('-');
        else
          normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      }
      return normalized;
    }

    struct cipher_state {
      mbedtls_cipher_context_t ctx;
      bool freed = false;
      mbedtls_operation_t operation = MBEDTLS_ENCRYPT;
      std::vector<std::uint8_t> auth_tag;
      Global<Object> self;

      cipher_state() {
        mbedtls_cipher_init(&ctx);
      }
      ~cipher_state() {
        if (!freed)
          mbedtls_cipher_free(&ctx);
      }
    };

    cipher_state* get_cipher_state(const FunctionCallbackInfo<Value>& info) {
      return static_cast<cipher_state*>(
          info.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
    }

    void cipher_finalizer(const WeakCallbackInfo<cipher_state>& data) {
      auto* state = data.GetParameter();
      state->self.Reset();
      delete state;
    }

    Local<Uint8Array> uint8_array_from_bytes(Isolate* iso, const std::uint8_t* data,
                                             std::size_t size) {
      auto backing = ArrayBuffer::NewBackingStore(iso, size);
      if (size > 0)
        std::memcpy(backing->Data(), data, size);
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      return Uint8Array::New(buffer, 0, size);
    }

    struct pk_context_guard {
      mbedtls_pk_context ctx;
      pk_context_guard() {
        mbedtls_pk_init(&ctx);
      }
      ~pk_context_guard() {
        mbedtls_pk_free(&ctx);
      }
    };

    struct rsa_context_guard {
      mbedtls_rsa_context ctx;
      rsa_context_guard() {
        mbedtls_rsa_init(&ctx);
      }
      ~rsa_context_guard() {
        mbedtls_rsa_free(&ctx);
      }
    };

    struct ecp_group_guard {
      mbedtls_ecp_group ctx;
      ecp_group_guard() {
        mbedtls_ecp_group_init(&ctx);
      }
      ~ecp_group_guard() {
        mbedtls_ecp_group_free(&ctx);
      }
    };

    struct ecp_point_guard {
      mbedtls_ecp_point ctx;
      ecp_point_guard() {
        mbedtls_ecp_point_init(&ctx);
      }
      ~ecp_point_guard() {
        mbedtls_ecp_point_free(&ctx);
      }
    };

    struct mpi_guard {
      mbedtls_mpi ctx;
      mpi_guard() {
        mbedtls_mpi_init(&ctx);
      }
      ~mpi_guard() {
        mbedtls_mpi_free(&ctx);
      }
    };

    Local<Uint8Array> mpi_to_uint8_minimal(Isolate* iso, const mbedtls_mpi* m) {
      std::size_t len = mbedtls_mpi_size(m);
      if (len == 0)
        len = 1;
      std::vector<std::uint8_t> buf(len);
      (void)mbedtls_mpi_write_binary(m, reinterpret_cast<unsigned char*>(buf.data()), len);
      return uint8_array_from_bytes(iso, buf.data(), buf.size());
    }

    Local<Uint8Array> mpi_to_uint8_fixed(Isolate* iso, const mbedtls_mpi* m,
                                         std::size_t fixed_len) {
      std::vector<std::uint8_t> buf(fixed_len);
      (void)mbedtls_mpi_write_binary(m, reinterpret_cast<unsigned char*>(buf.data()), fixed_len);
      return uint8_array_from_bytes(iso, buf.data(), buf.size());
    }

    bool require_ec_coord_size(Isolate* iso, std::string_view name, const byte_view& bytes,
                               std::size_t coord_len) {
      if (bytes.size == coord_len)
        return true;
      iso->ThrowException(
          Exception::RangeError(str(iso, std::string("EC component '") + std::string(name) +
                                             "' must match the curve coordinate length")));
      return false;
    }

    bool require_ec_scalar_size(Isolate* iso, const byte_view& bytes, std::size_t coord_len) {
      if (bytes.size > 0 && bytes.size <= coord_len)
        return true;
      iso->ThrowException(Exception::RangeError(
          str(iso, "EC private scalar must be non-empty and no longer than the curve length")));
      return false;
    }

    bool read_ec_curve(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                       mbedtls_ecp_group_id& group_id, std::size_t& coord_len) {
      const auto curve = read_string_property(iso, ctx, obj, "curve");
      if (curve.empty()) {
        throw_error(iso, "missing EC curve");
        return false;
      }
      if (!ecp_group_id_for(curve, group_id)) {
        throw_error(iso, "unsupported EC curve");
        return false;
      }
      coord_len = coord_size_bytes(group_id);
      if (coord_len == 0) {
        throw_error(iso, "unsupported EC curve");
        return false;
      }
      return true;
    }

    bool setup_rsa_pk(Isolate* iso, Local<Context> ctx, Local<Object> components,
                      bool require_private, pk_context_guard& pk) {
      byte_view n, e, d, p, q, dp, dq, qi;
      if (!read_bytes_property(iso, ctx, components, "n", n, true) ||
          !read_bytes_property(iso, ctx, components, "e", e, true))
        return false;
      if (require_private && !read_bytes_property(iso, ctx, components, "d", d, true))
        return false;
      if (!require_private)
        (void)read_bytes_property(iso, ctx, components, "d", d, false);
      if (!read_bytes_property(iso, ctx, components, "p", p, false) ||
          !read_bytes_property(iso, ctx, components, "q", q, false) ||
          !read_bytes_property(iso, ctx, components, "dp", dp, false) ||
          !read_bytes_property(iso, ctx, components, "dq", dq, false) ||
          !read_bytes_property(iso, ctx, components, "qi", qi, false))
        return false;
      (void)dp;
      (void)dq;
      (void)qi;

      int rc = mbedtls_pk_setup(&pk.ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
      if (rc != 0) {
        throw_error(iso, "failed to initialize RSA key: " + mbedtls_err_str(rc));
        return false;
      }
      auto* rsa = mbedtls_pk_rsa(pk.ctx);
      mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
      rc = mbedtls_rsa_import_raw(rsa, n.data, n.size, p.size ? p.data : nullptr, p.size,
                                  q.size ? q.data : nullptr, q.size, d.size ? d.data : nullptr,
                                  d.size, e.data, e.size);
      if (rc != 0) {
        throw_error(iso, "invalid RSA key components: " + mbedtls_err_str(rc));
        return false;
      }
      if (require_private) {
        rc = mbedtls_rsa_complete(rsa);
        if (rc != 0) {
          throw_error(iso, "invalid RSA private key components: " + mbedtls_err_str(rc));
          return false;
        }
      }
      return true;
    }

    bool setup_ec_pk(Isolate* iso, Local<Context> ctx, Local<Object> components,
                     bool require_private, pk_context_guard& pk) {
      mbedtls_ecp_group_id group_id = MBEDTLS_ECP_DP_NONE;
      std::size_t coord_len = 0;
      if (!read_ec_curve(iso, ctx, components, group_id, coord_len))
        return false;

      byte_view x, y, d;
      if (!read_bytes_property(iso, ctx, components, "x", x, true) ||
          !read_bytes_property(iso, ctx, components, "y", y, true))
        return false;
      if (!require_ec_coord_size(iso, "x", x, coord_len) ||
          !require_ec_coord_size(iso, "y", y, coord_len))
        return false;
      if (require_private) {
        if (!read_bytes_property(iso, ctx, components, "d", d, true) ||
            !require_ec_scalar_size(iso, d, coord_len))
          return false;
      }

      int rc = mbedtls_pk_setup(&pk.ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
      if (rc != 0) {
        throw_error(iso, "failed to initialize EC key: " + mbedtls_err_str(rc));
        return false;
      }
      auto* ec = mbedtls_pk_ec(pk.ctx);
      rc = mbedtls_ecp_group_load(&ec->MBEDTLS_PRIVATE(grp), group_id);
      if (rc != 0) {
        throw_error(iso, "failed to load EC curve: " + mbedtls_err_str(rc));
        return false;
      }
      rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), x.data, x.size);
      if (rc == 0)
        rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), y.data, y.size);
      if (rc == 0)
        rc = mbedtls_mpi_lset(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1);
      if (rc == 0 && require_private)
        rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(d), d.data, d.size);
      if (rc != 0) {
        throw_error(iso, "invalid EC key components: " + mbedtls_err_str(rc));
        return false;
      }
      return true;
    }

    bool setup_pk_for_components(Isolate* iso, Local<Context> ctx, Local<Object> components,
                                 bool require_private, pk_context_guard& pk) {
      const auto kind = read_string_property(iso, ctx, components, "kind");
      if (kind == "rsa")
        return setup_rsa_pk(iso, ctx, components, require_private, pk);
      if (kind == "ec")
        return setup_ec_pk(iso, ctx, components, require_private, pk);
      if (!read_string_property(iso, ctx, components, "curve").empty())
        return setup_ec_pk(iso, ctx, components, require_private, pk);
      return setup_rsa_pk(iso, ctx, components, require_private, pk);
    }

    bool set_rsa_public_components(Isolate* iso, Local<Context> ctx, Local<Object> out,
                                   mbedtls_rsa_context* rsa) {
      mpi_guard N;
      mpi_guard E;
      const int rc = mbedtls_rsa_export(rsa, &N.ctx, nullptr, nullptr, nullptr, &E.ctx);
      if (rc != 0) {
        throw_error(iso, "failed to export RSA components: " + mbedtls_err_str(rc));
        return false;
      }
      (void)out->Set(ctx, str(iso, "n"), mpi_to_uint8_minimal(iso, &N.ctx));
      (void)out->Set(ctx, str(iso, "e"), mpi_to_uint8_minimal(iso, &E.ctx));
      return true;
    }

    void pk_parse_public_key_der(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_view der;
      if (info.Length() < 1 || !value_to_bytes(iso, ctx, info[0], der)) {
        throw_error(iso, "__fxe_native.pk.parsePublicKeyDer requires a Uint8Array");
        return;
      }

      pk_context_guard pk;
      const int rc = mbedtls_pk_parse_public_key(&pk.ctx, der.data, der.size);
      if (rc != 0) {
        throw_error(iso, "failed to parse SPKI public key: " + mbedtls_err_str(rc));
        return;
      }

      auto out = Object::New(iso);
      const auto type = mbedtls_pk_get_type(&pk.ctx);
      if (type == MBEDTLS_PK_RSA) {
        set_string(ctx, out, "kind", "rsa");
        if (!set_rsa_public_components(iso, ctx, out, mbedtls_pk_rsa(pk.ctx)))
          return;
      } else if (type == MBEDTLS_PK_ECKEY || type == MBEDTLS_PK_ECKEY_DH) {
        auto* ec = mbedtls_pk_ec(pk.ctx);
        const auto group_id = ec->MBEDTLS_PRIVATE(grp).id;
        const auto coord_len = coord_size_bytes(group_id);
        if (coord_len == 0) {
          throw_error(iso, "unsupported EC curve");
          return;
        }
        set_string(ctx, out, "kind", "ec");
        set_string(ctx, out, "curve", canonical_curve_name(group_id));
        (void)out->Set(
            ctx, str(iso, "x"),
            mpi_to_uint8_fixed(iso, &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), coord_len));
        (void)out->Set(
            ctx, str(iso, "y"),
            mpi_to_uint8_fixed(iso, &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), coord_len));
      } else {
        throw_error(iso, "unsupported PK type in SPKI");
        return;
      }
      info.GetReturnValue().Set(out);
    }

    void pk_parse_private_key_der(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_view der;
      if (info.Length() < 1 || !value_to_bytes(iso, ctx, info[0], der)) {
        throw_error(iso, "__fxe_native.pk.parsePrivateKeyDer requires a Uint8Array");
        return;
      }

      pk_context_guard pk;
      auto& rng = mbedtls_rng();
      const int rc = mbedtls_pk_parse_key(&pk.ctx, der.data, der.size, nullptr, 0,
                                          mbedtls_ctr_drbg_random, &rng.ctr_drbg);
      if (rc != 0) {
        throw_error(iso, "failed to parse PKCS8 private key: " + mbedtls_err_str(rc));
        return;
      }

      auto out = Object::New(iso);
      const auto type = mbedtls_pk_get_type(&pk.ctx);
      if (type == MBEDTLS_PK_RSA) {
        set_string(ctx, out, "kind", "rsa");
        auto* rsa = mbedtls_pk_rsa(pk.ctx);
        mpi_guard N;
        mpi_guard P;
        mpi_guard Q;
        mpi_guard D;
        mpi_guard E;
        mpi_guard DP;
        mpi_guard DQ;
        mpi_guard QP;
        int export_rc = mbedtls_rsa_export(rsa, &N.ctx, &P.ctx, &Q.ctx, &D.ctx, &E.ctx);
        if (export_rc == 0)
          export_rc = mbedtls_rsa_export_crt(rsa, &DP.ctx, &DQ.ctx, &QP.ctx);
        if (export_rc != 0) {
          throw_error(iso,
                      "failed to export RSA private components: " + mbedtls_err_str(export_rc));
          return;
        }
        (void)out->Set(ctx, str(iso, "n"), mpi_to_uint8_minimal(iso, &N.ctx));
        (void)out->Set(ctx, str(iso, "e"), mpi_to_uint8_minimal(iso, &E.ctx));
        (void)out->Set(ctx, str(iso, "d"), mpi_to_uint8_minimal(iso, &D.ctx));
        (void)out->Set(ctx, str(iso, "p"), mpi_to_uint8_minimal(iso, &P.ctx));
        (void)out->Set(ctx, str(iso, "q"), mpi_to_uint8_minimal(iso, &Q.ctx));
        (void)out->Set(ctx, str(iso, "dp"), mpi_to_uint8_minimal(iso, &DP.ctx));
        (void)out->Set(ctx, str(iso, "dq"), mpi_to_uint8_minimal(iso, &DQ.ctx));
        (void)out->Set(ctx, str(iso, "qi"), mpi_to_uint8_minimal(iso, &QP.ctx));
      } else if (type == MBEDTLS_PK_ECKEY || type == MBEDTLS_PK_ECKEY_DH) {
        auto* ec = mbedtls_pk_ec(pk.ctx);
        const auto group_id = ec->MBEDTLS_PRIVATE(grp).id;
        const auto coord_len = coord_size_bytes(group_id);
        if (coord_len == 0) {
          throw_error(iso, "unsupported EC curve");
          return;
        }
        set_string(ctx, out, "kind", "ec");
        set_string(ctx, out, "curve", canonical_curve_name(group_id));
        (void)out->Set(
            ctx, str(iso, "x"),
            mpi_to_uint8_fixed(iso, &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), coord_len));
        (void)out->Set(
            ctx, str(iso, "y"),
            mpi_to_uint8_fixed(iso, &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), coord_len));
        (void)out->Set(ctx, str(iso, "d"),
                       mpi_to_uint8_fixed(iso, &ec->MBEDTLS_PRIVATE(d), coord_len));
      } else {
        throw_error(iso, "unsupported PK type in PKCS8");
        return;
      }
      info.GetReturnValue().Set(out);
    }

    void pk_write_public_key_der(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        throw_error(iso, "__fxe_native.pk.writePublicKeyDer requires components");
        return;
      }

      pk_context_guard pk;
      if (!setup_pk_for_components(iso, ctx, info[0].As<Object>(), false, pk))
        return;

      std::array<unsigned char, 4096> buf{};
      const int written = mbedtls_pk_write_pubkey_der(&pk.ctx, buf.data(), buf.size());
      if (written < 0) {
        throw_error(iso, "failed to write SPKI public key: " + mbedtls_err_str(written));
        return;
      }
      const auto written_size = static_cast<std::size_t>(written);
      info.GetReturnValue().Set(uint8_array_from_bytes(
          iso, reinterpret_cast<const std::uint8_t*>(buf.data() + (buf.size() - written_size)),
          written_size));
    }

    void pk_write_private_key_der(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsObject()) {
        throw_error(iso, "__fxe_native.pk.writePrivateKeyDer requires components");
        return;
      }

      pk_context_guard pk;
      if (!setup_pk_for_components(iso, ctx, info[0].As<Object>(), true, pk))
        return;

      std::array<unsigned char, 4096> buf{};
      const int written = mbedtls_pk_write_key_der(&pk.ctx, buf.data(), buf.size());
      if (written < 0) {
        throw_error(iso, "failed to write PKCS8 private key: " + mbedtls_err_str(written));
        return;
      }
      const auto written_size = static_cast<std::size_t>(written);
      info.GetReturnValue().Set(uint8_array_from_bytes(
          iso, reinterpret_cast<const std::uint8_t*>(buf.data() + (buf.size() - written_size)),
          written_size));
    }

    void pk_rsa_oaep_encrypt(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 4 || !info[0]->IsObject() || !info[1]->IsString()) {
        throw_error(iso, "__fxe_native.pk.rsaOaepEncrypt requires (components, hashName, label, "
                         "plaintext)");
        return;
      }

      String::Utf8Value hash_utf8(iso, info[1]);
      const auto md_type = hash_type_for(
          *hash_utf8 ? std::string_view(*hash_utf8, hash_utf8.length()) : std::string_view{});
      if (md_type == MBEDTLS_MD_NONE) {
        throw_error(iso, "unsupported RSA-OAEP hash");
        return;
      }

      byte_view label, plaintext, n, e;
      auto components = info[0].As<Object>();
      if (!read_bytes_property(iso, ctx, components, "n", n, true) ||
          !read_bytes_property(iso, ctx, components, "e", e, true))
        return;
      if (!value_to_bytes(iso, ctx, info[2], label)) {
        throw_error(iso, "label must be Uint8Array");
        return;
      }
      if (!value_to_bytes(iso, ctx, info[3], plaintext)) {
        throw_error(iso, "plaintext must be Uint8Array");
        return;
      }

      rsa_context_guard rsa;
      int rc = mbedtls_rsa_import_raw(&rsa.ctx, n.data, n.size, nullptr, 0, nullptr, 0, nullptr, 0,
                                      e.data, e.size);
      if (rc == 0)
        mbedtls_rsa_set_padding(&rsa.ctx, MBEDTLS_RSA_PKCS_V21, md_type);
      if (rc != 0) {
        throw_error(iso, "invalid RSA public key: " + mbedtls_err_str(rc));
        return;
      }

      std::vector<std::uint8_t> out(mbedtls_rsa_get_len(&rsa.ctx));
      auto& rng = mbedtls_rng();
      rc = mbedtls_rsa_rsaes_oaep_encrypt(&rsa.ctx, mbedtls_ctr_drbg_random, &rng.ctr_drbg,
                                          label.size ? label.data : nullptr, label.size,
                                          plaintext.size, plaintext.data, out.data());
      if (rc != 0) {
        throw_error(iso, "RSA-OAEP encrypt failed: " + mbedtls_err_str(rc));
        return;
      }
      info.GetReturnValue().Set(uint8_array_from_bytes(iso, out.data(), out.size()));
    }

    void pk_rsa_oaep_decrypt(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 4 || !info[0]->IsObject() || !info[1]->IsString()) {
        throw_error(iso, "__fxe_native.pk.rsaOaepDecrypt requires (components, hashName, label, "
                         "ciphertext)");
        return;
      }

      String::Utf8Value hash_utf8(iso, info[1]);
      const auto md_type = hash_type_for(
          *hash_utf8 ? std::string_view(*hash_utf8, hash_utf8.length()) : std::string_view{});
      if (md_type == MBEDTLS_MD_NONE) {
        throw_error(iso, "unsupported RSA-OAEP hash");
        return;
      }

      byte_view n, e, d, p, q, dp, dq, qi, label, ciphertext;
      auto components = info[0].As<Object>();
      if (!read_bytes_property(iso, ctx, components, "n", n, true) ||
          !read_bytes_property(iso, ctx, components, "e", e, true) ||
          !read_bytes_property(iso, ctx, components, "d", d, true) ||
          !read_bytes_property(iso, ctx, components, "p", p, false) ||
          !read_bytes_property(iso, ctx, components, "q", q, false) ||
          !read_bytes_property(iso, ctx, components, "dp", dp, false) ||
          !read_bytes_property(iso, ctx, components, "dq", dq, false) ||
          !read_bytes_property(iso, ctx, components, "qi", qi, false))
        return;
      (void)dp;
      (void)dq;
      (void)qi;
      if (!value_to_bytes(iso, ctx, info[2], label)) {
        throw_error(iso, "label must be Uint8Array");
        return;
      }
      if (!value_to_bytes(iso, ctx, info[3], ciphertext)) {
        throw_error(iso, "ciphertext must be Uint8Array");
        return;
      }

      rsa_context_guard rsa;
      int rc =
          mbedtls_rsa_import_raw(&rsa.ctx, n.data, n.size, p.size ? p.data : nullptr, p.size,
                                 q.size ? q.data : nullptr, q.size, d.data, d.size, e.data, e.size);
      if (rc == 0)
        rc = mbedtls_rsa_complete(&rsa.ctx);
      if (rc == 0)
        mbedtls_rsa_set_padding(&rsa.ctx, MBEDTLS_RSA_PKCS_V21, md_type);
      if (rc != 0) {
        throw_error(iso, "invalid RSA private key components: " + mbedtls_err_str(rc));
        return;
      }

      const std::size_t key_len = mbedtls_rsa_get_len(&rsa.ctx);
      std::vector<std::uint8_t> out(key_len);
      std::size_t out_len = 0;
      auto& rng = mbedtls_rng();
      rc = mbedtls_rsa_rsaes_oaep_decrypt(&rsa.ctx, mbedtls_ctr_drbg_random, &rng.ctr_drbg,
                                          label.size ? label.data : nullptr, label.size, &out_len,
                                          ciphertext.data, out.data(), out.size());
      if (rc != 0) {
        throw_error(iso, "RSA-OAEP decrypt failed: " + mbedtls_err_str(rc));
        return;
      }
      info.GetReturnValue().Set(uint8_array_from_bytes(iso, out.data(), out_len));
    }

    bool hash_data_for_pk(Isolate* iso, mbedtls_md_type_t md_type, const byte_view& data,
                          std::vector<std::uint8_t>& hash) {
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(md_type);
      if (md == nullptr) {
        throw_error(iso, "unsupported ECDSA hash");
        return false;
      }
      hash.resize(mbedtls_md_get_size(md));
      const int rc = mbedtls_md(md, data.data, data.size, hash.data());
      if (rc != 0) {
        throw_error(iso, "failed to hash ECDSA data: " + mbedtls_err_str(rc));
        return false;
      }
      return true;
    }

    void pk_ecdsa_sign(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsObject() || !info[1]->IsString()) {
        throw_error(iso, "__fxe_native.pk.ecdsaSign requires (components, hashName, data)");
        return;
      }
      String::Utf8Value hash_utf8(iso, info[1]);
      const auto md_type = hash_type_for(
          *hash_utf8 ? std::string_view(*hash_utf8, hash_utf8.length()) : std::string_view{});
      if (md_type == MBEDTLS_MD_NONE) {
        throw_error(iso, "unsupported ECDSA hash");
        return;
      }

      auto components = info[0].As<Object>();
      mbedtls_ecp_group_id group_id = MBEDTLS_ECP_DP_NONE;
      std::size_t coord_len = 0;
      if (!read_ec_curve(iso, ctx, components, group_id, coord_len))
        return;
      byte_view d, data;
      if (!read_bytes_property(iso, ctx, components, "d", d, true) ||
          !require_ec_scalar_size(iso, d, coord_len))
        return;
      if (!value_to_bytes(iso, ctx, info[2], data)) {
        throw_error(iso, "ECDSA data must be Uint8Array");
        return;
      }

      ecp_group_guard grp;
      int rc = mbedtls_ecp_group_load(&grp.ctx, group_id);
      if (rc != 0) {
        throw_error(iso, "failed to load EC curve: " + mbedtls_err_str(rc));
        return;
      }
      mpi_guard d_mpi;
      mpi_guard r;
      mpi_guard s;
      rc = mbedtls_mpi_read_binary(&d_mpi.ctx, d.data, d.size);
      if (rc != 0) {
        throw_error(iso, "invalid EC private scalar: " + mbedtls_err_str(rc));
        return;
      }
      std::vector<std::uint8_t> hash;
      if (!hash_data_for_pk(iso, md_type, data, hash))
        return;
      auto& rng = mbedtls_rng();
      rc = mbedtls_ecdsa_sign_det_ext(&grp.ctx, &r.ctx, &s.ctx, &d_mpi.ctx, hash.data(),
                                      hash.size(), md_type, mbedtls_ctr_drbg_random, &rng.ctr_drbg);
      if (rc != 0) {
        throw_error(iso, "ECDSA sign failed: " + mbedtls_err_str(rc));
        return;
      }
      std::vector<std::uint8_t> out(coord_len * 2);
      (void)mbedtls_mpi_write_binary(&r.ctx, out.data(), coord_len);
      (void)mbedtls_mpi_write_binary(&s.ctx, out.data() + coord_len, coord_len);
      info.GetReturnValue().Set(uint8_array_from_bytes(iso, out.data(), out.size()));
    }

    void pk_ecdsa_verify(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 4 || !info[0]->IsObject() || !info[1]->IsString()) {
        throw_error(iso, "__fxe_native.pk.ecdsaVerify requires (components, hashName, data, "
                         "signature)");
        return;
      }
      String::Utf8Value hash_utf8(iso, info[1]);
      const auto md_type = hash_type_for(
          *hash_utf8 ? std::string_view(*hash_utf8, hash_utf8.length()) : std::string_view{});
      if (md_type == MBEDTLS_MD_NONE) {
        throw_error(iso, "unsupported ECDSA hash");
        return;
      }

      auto components = info[0].As<Object>();
      mbedtls_ecp_group_id group_id = MBEDTLS_ECP_DP_NONE;
      std::size_t coord_len = 0;
      if (!read_ec_curve(iso, ctx, components, group_id, coord_len))
        return;
      byte_view x, y, data, signature;
      if (!read_bytes_property(iso, ctx, components, "x", x, true) ||
          !read_bytes_property(iso, ctx, components, "y", y, true) ||
          !require_ec_coord_size(iso, "x", x, coord_len) ||
          !require_ec_coord_size(iso, "y", y, coord_len))
        return;
      if (!value_to_bytes(iso, ctx, info[2], data)) {
        throw_error(iso, "ECDSA data must be Uint8Array");
        return;
      }
      if (!value_to_bytes(iso, ctx, info[3], signature)) {
        throw_error(iso, "ECDSA signature must be Uint8Array");
        return;
      }
      if (signature.size != coord_len * 2) {
        info.GetReturnValue().Set(Boolean::New(iso, false));
        return;
      }

      ecp_group_guard grp;
      int rc = mbedtls_ecp_group_load(&grp.ctx, group_id);
      if (rc != 0) {
        throw_error(iso, "failed to load EC curve: " + mbedtls_err_str(rc));
        return;
      }
      ecp_point_guard Q;
      rc = mbedtls_mpi_read_binary(&Q.ctx.MBEDTLS_PRIVATE(X), x.data, x.size);
      if (rc == 0)
        rc = mbedtls_mpi_read_binary(&Q.ctx.MBEDTLS_PRIVATE(Y), y.data, y.size);
      if (rc == 0)
        rc = mbedtls_mpi_lset(&Q.ctx.MBEDTLS_PRIVATE(Z), 1);
      if (rc != 0) {
        throw_error(iso, "invalid EC public point: " + mbedtls_err_str(rc));
        return;
      }
      mpi_guard r;
      mpi_guard s;
      rc = mbedtls_mpi_read_binary(&r.ctx, signature.data, coord_len);
      if (rc == 0)
        rc = mbedtls_mpi_read_binary(&s.ctx, signature.data + coord_len, coord_len);
      if (rc != 0) {
        throw_error(iso, "invalid ECDSA signature: " + mbedtls_err_str(rc));
        return;
      }
      std::vector<std::uint8_t> hash;
      if (!hash_data_for_pk(iso, md_type, data, hash))
        return;
      rc = mbedtls_ecdsa_verify(&grp.ctx, hash.data(), hash.size(), &Q.ctx, &r.ctx, &s.ctx);
      info.GetReturnValue().Set(Boolean::New(iso, rc == 0));
    }

    void pk_ecdsa_generate(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.pk.ecdsaGenerate requires a curve string");
        return;
      }
      String::Utf8Value curve_utf8(iso, info[0]);
      mbedtls_ecp_group_id group_id = MBEDTLS_ECP_DP_NONE;
      if (!ecp_group_id_for(*curve_utf8 ? std::string_view(*curve_utf8, curve_utf8.length())
                                        : std::string_view{},
                            group_id)) {
        throw_error(iso, "unsupported EC curve");
        return;
      }
      const auto coord_len = coord_size_bytes(group_id);
      if (coord_len == 0) {
        throw_error(iso, "unsupported EC curve");
        return;
      }

      ecp_group_guard grp;
      mpi_guard d;
      ecp_point_guard Q;
      int rc = mbedtls_ecp_group_load(&grp.ctx, group_id);
      if (rc != 0) {
        throw_error(iso, "failed to load EC curve: " + mbedtls_err_str(rc));
        return;
      }
      auto& rng = mbedtls_rng();
      rc =
          mbedtls_ecp_gen_keypair(&grp.ctx, &d.ctx, &Q.ctx, mbedtls_ctr_drbg_random, &rng.ctr_drbg);
      if (rc != 0) {
        throw_error(iso, "ECDSA key generation failed: " + mbedtls_err_str(rc));
        return;
      }
      auto out = Object::New(iso);
      set_string(ctx, out, "kind", "ec");
      set_string(ctx, out, "curve", canonical_curve_name(group_id));
      (void)out->Set(ctx, str(iso, "x"),
                     mpi_to_uint8_fixed(iso, &Q.ctx.MBEDTLS_PRIVATE(X), coord_len));
      (void)out->Set(ctx, str(iso, "y"),
                     mpi_to_uint8_fixed(iso, &Q.ctx.MBEDTLS_PRIVATE(Y), coord_len));
      (void)out->Set(ctx, str(iso, "d"), mpi_to_uint8_fixed(iso, &d.ctx, coord_len));
      info.GetReturnValue().Set(out);
    }

    void pk_timing_safe_equal(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_view a, b;
      if (info.Length() < 2 || !value_to_bytes(iso, ctx, info[0], a) ||
          !value_to_bytes(iso, ctx, info[1], b)) {
        iso->ThrowException(
            Exception::TypeError(str(iso, "__fxe_native.pk.timingSafeEqual requires two "
                                          "Uint8Arrays")));
        return;
      }
      if (a.size != b.size) {
        iso->ThrowException(
            Exception::RangeError(str(iso, "timingSafeEqual inputs must have the same length")));
        return;
      }
      if (a.size == 0) {
        info.GetReturnValue().Set(Boolean::New(iso, true));
        return;
      }
      const int rc = mbedtls_ct_memcmp(a.data, b.data, a.size);
      info.GetReturnValue().Set(Boolean::New(iso, rc == 0));
    }

    void cipher_update(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_cipher_state(info);
      if (!state || state->freed) {
        throw_error(iso, "cipher context has already been finalized");
        return;
      }
      if (info.Length() < 1) {
        throw_error(iso, "cipher.update requires data");
        return;
      }
      byte_view bytes;
      if (!value_to_bytes(iso, iso->GetCurrentContext(), info[0], bytes)) {
        throw_error(iso, "cipher.update requires a string, ArrayBuffer, or typed array");
        return;
      }
      const std::size_t block_size = mbedtls_cipher_get_block_size(&state->ctx);
      std::vector<std::uint8_t> out(bytes.size + block_size);
      std::size_t out_len = 0;
      const int rc =
          mbedtls_cipher_update(&state->ctx, bytes.data, bytes.size, out.data(), &out_len);
      if (rc != 0) {
        throw_error(iso, "cipher.update failed");
        return;
      }
      info.GetReturnValue().Set(uint8_array_from_bytes(iso, out.data(), out_len));
    }

    void cipher_set_aad(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_cipher_state(info);
      if (!state || state->freed) {
        throw_error(iso, "cipher context has already been finalized");
        return;
      }
      byte_view bytes;
      if (info.Length() < 1 || !value_to_bytes(iso, iso->GetCurrentContext(), info[0], bytes)) {
        throw_error(iso, "cipher.setAAD requires bytes");
        return;
      }
      const int rc = mbedtls_cipher_update_ad(&state->ctx, bytes.data, bytes.size);
      if (rc != 0) {
        throw_error(iso, "cipher.setAAD failed");
        return;
      }
      info.GetReturnValue().Set(info.This());
    }
    void cipher_final(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_cipher_state(info);
      if (!state || state->freed) {
        throw_error(iso, "cipher context has already been finalized");
        return;
      }
      const std::size_t block_size = mbedtls_cipher_get_block_size(&state->ctx);
      std::vector<std::uint8_t> out(block_size == 0 ? 1 : block_size);
      std::size_t out_len = 0;
      int rc = mbedtls_cipher_finish(&state->ctx, out.data(), &out_len);
      if (rc == 0 && state->operation == MBEDTLS_DECRYPT && !state->auth_tag.empty())
        rc = mbedtls_cipher_check_tag(&state->ctx, state->auth_tag.data(), state->auth_tag.size());
      if (rc != 0) {
        throw_error(iso, "cipher.final failed");
        return;
      }
      if (state->operation == MBEDTLS_ENCRYPT) {
        state->auth_tag.assign(16, 0);
        if (mbedtls_cipher_write_tag(&state->ctx, state->auth_tag.data(), state->auth_tag.size()) !=
            0)
          state->auth_tag.clear();
      }
      mbedtls_cipher_free(&state->ctx);
      state->freed = true;
      info.GetReturnValue().Set(uint8_array_from_bytes(iso, out.data(), out_len));
    }

    void cipher_set_auto_padding(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_cipher_state(info);
      if (!state || state->freed) {
        throw_error(iso, "cipher context has already been finalized");
        return;
      }
      const bool enabled = info.Length() < 1 || info[0]->BooleanValue(iso);
      const int rc = mbedtls_cipher_set_padding_mode(&state->ctx, enabled ? MBEDTLS_PADDING_PKCS7
                                                                          : MBEDTLS_PADDING_NONE);
      if (rc != 0) {
        throw_error(iso, "cipher.setAutoPadding failed");
        return;
      }
      info.GetReturnValue().Set(info.This());
    }

    void cipher_get_auth_tag(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_cipher_state(info);
      if (!state || state->auth_tag.empty()) {
        throw_error(iso, "cipher auth tag is not available");
        return;
      }
      info.GetReturnValue().Set(
          uint8_array_from_bytes(iso, state->auth_tag.data(), state->auth_tag.size()));
    }

    void cipher_set_auth_tag(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_cipher_state(info);
      if (!state || state->freed) {
        throw_error(iso, "cipher context has already been finalized");
        return;
      }
      byte_view bytes;
      if (info.Length() < 1 || !value_to_bytes(iso, iso->GetCurrentContext(), info[0], bytes)) {
        throw_error(iso, "cipher.setAuthTag requires bytes");
        return;
      }
      state->auth_tag.assign(bytes.data, bytes.data + bytes.size);
      info.GetReturnValue().Set(info.This());
    }
    void cipher_create_common(const FunctionCallbackInfo<Value>& info,
                              mbedtls_operation_t operation) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.cipher.createCipheriv requires algorithm, key, and iv");
        return;
      }
      String::Utf8Value algo_utf8(iso, info[0]);
      const std::string cipher_name = cipher_name_for(
          *algo_utf8 ? std::string_view(*algo_utf8, algo_utf8.length()) : std::string_view{});
      const mbedtls_cipher_info_t* cipher_info =
          mbedtls_cipher_info_from_string(cipher_name.c_str());
      if (cipher_info == nullptr) {
        throw_error(iso, "unsupported cipher algorithm");
        return;
      }
      byte_view key;
      byte_view iv;
      if (!value_to_bytes(iso, ctx, info[1], key) || !value_to_bytes(iso, ctx, info[2], iv)) {
        throw_error(iso, "cipher key and iv must be strings, ArrayBuffers, or typed arrays");
        return;
      }
      auto state = std::make_unique<cipher_state>();
      if (mbedtls_cipher_setup(&state->ctx, cipher_info) != 0 ||
          mbedtls_cipher_setkey(&state->ctx, key.data, static_cast<int>(key.size * 8), operation) !=
              0 ||
          mbedtls_cipher_set_iv(&state->ctx, iv.data, iv.size) != 0 ||
          mbedtls_cipher_reset(&state->ctx) != 0) {
        throw_error(iso, "cipher initialization failed");
        return;
      }
      state->operation = operation;
      if (mbedtls_cipher_get_cipher_mode(&state->ctx) == MBEDTLS_MODE_CBC) {
        (void)mbedtls_cipher_set_padding_mode(&state->ctx, MBEDTLS_PADDING_PKCS7);
      }
      auto out = Object::New(iso);
      auto external = External::New(iso, state.get(), v8::kExternalPointerTypeTagDefault);
      (void)out->Set(ctx, str(iso, "update"),
                     Function::New(ctx, cipher_update, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "final"),
                     Function::New(ctx, cipher_final, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "setAutoPadding"),
                     Function::New(ctx, cipher_set_auto_padding, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "setAAD"),
                     Function::New(ctx, cipher_set_aad, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "getAuthTag"),
                     Function::New(ctx, cipher_get_auth_tag, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "setAuthTag"),
                     Function::New(ctx, cipher_set_auth_tag, external).ToLocalChecked());
      state->self.Reset(iso, out);
      state->self.SetWeak(state.get(), cipher_finalizer, WeakCallbackType::kParameter);
      (void)state.release();
      info.GetReturnValue().Set(out);
    }

    void cipher_create_cipheriv(const FunctionCallbackInfo<Value>& info) {
      cipher_create_common(info, MBEDTLS_ENCRYPT);
    }

    void cipher_create_decipheriv(const FunctionCallbackInfo<Value>& info) {
      cipher_create_common(info, MBEDTLS_DECRYPT);
    }

    void os_platform(const FunctionCallbackInfo<Value>& info) {
#if defined(__APPLE__)
      info.GetReturnValue().Set(str(info.GetIsolate(), "darwin"));
#elif defined(__linux__)
      info.GetReturnValue().Set(str(info.GetIsolate(), "linux"));
#elif defined(_WIN32)
    info.GetReturnValue().Set(str(info.GetIsolate(), "win32"));
#else
    info.GetReturnValue().Set(str(info.GetIsolate(), "unknown"));
#endif
    }

    void os_arch(const FunctionCallbackInfo<Value>& info) {
#if defined(__aarch64__) || defined(_M_ARM64)
      info.GetReturnValue().Set(str(info.GetIsolate(), "arm64"));
#elif defined(__x86_64__) || defined(_M_X64)
      info.GetReturnValue().Set(str(info.GetIsolate(), "x64"));
#elif defined(__arm__) || defined(_M_ARM)
    info.GetReturnValue().Set(str(info.GetIsolate(), "arm"));
#elif defined(__i386__) || defined(_M_IX86)
    info.GetReturnValue().Set(str(info.GetIsolate(), "ia32"));
#else
    info.GetReturnValue().Set(str(info.GetIsolate(), "unknown"));
#endif
    }

    void os_type(const FunctionCallbackInfo<Value>& info) {
#if defined(__APPLE__)
      info.GetReturnValue().Set(str(info.GetIsolate(), "Darwin"));
#elif defined(__linux__)
      info.GetReturnValue().Set(str(info.GetIsolate(), "Linux"));
#elif defined(_WIN32)
    info.GetReturnValue().Set(str(info.GetIsolate(), "Windows_NT"));
#else
    info.GetReturnValue().Set(str(info.GetIsolate(), "Unknown"));
#endif
    }

    void os_endianness(const FunctionCallbackInfo<Value>& info) {
      const std::uint16_t marker = 0x0102;
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(&marker);
      info.GetReturnValue().Set(str(info.GetIsolate(), bytes[0] == 0x02 ? "LE" : "BE"));
    }

    void os_release_fn(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(str(info.GetIsolate(), os_release()));
    }

    void os_homedir(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(str(info.GetIsolate(), home_dir()));
    }

    void os_tmpdir(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(str(info.GetIsolate(), tmp_dir()));
    }

    void os_hostname(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(str(info.GetIsolate(), host_name()));
    }

    void os_uptime_fn(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Number::New(info.GetIsolate(), os_uptime()));
    }

    void os_totalmem(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Number::New(info.GetIsolate(), total_mem()));
    }

    void os_freemem(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Number::New(info.GetIsolate(), free_mem()));
    }

    void os_cpus(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int count = cpu_count();
      const uint32_t count_u32 = count > 0 ? static_cast<uint32_t>(count) : 0;
      auto array = Array::New(iso, count > 0 ? count : 0);
      const std::string model = cpu_model();
      const int speed = cpu_speed_mhz();
      for (uint32_t i = 0; i < count_u32; ++i) {
        auto cpu = Object::New(iso);
        set_string(ctx, cpu, "model", model);
        set_number(ctx, cpu, "speed", speed);
        auto times = Object::New(iso);
        set_number(ctx, times, "user", 0);
        set_number(ctx, times, "nice", 0);
        set_number(ctx, times, "sys", 0);
        set_number(ctx, times, "idle", 0);
        set_number(ctx, times, "irq", 0);
        set(ctx, cpu, "times", times);
        (void)array->Set(ctx, i, cpu);
      }
      info.GetReturnValue().Set(array);
    }

    void os_network_interfaces(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto out = Object::New(iso);
#if !defined(_WIN32)
      ifaddrs* interfaces = nullptr;
      if (getifaddrs(&interfaces) == 0) {
        std::uint32_t next_index = 0;
        for (auto* it = interfaces; it; it = it->ifa_next) {
          if (!it->ifa_addr || !it->ifa_name)
            continue;
          const int family = it->ifa_addr->sa_family;
          if (family != AF_INET && family != AF_INET6)
            continue;
          auto name = str(iso, it->ifa_name);
          Local<Value> existing;
          Local<Array> entries;
          if (out->Get(ctx, name).ToLocal(&existing) && existing->IsArray()) {
            entries = existing.As<Array>();
          } else {
            entries = Array::New(iso);
            (void)out->Set(ctx, name, entries);
          }
          auto entry = Object::New(iso);
          set_string(ctx, entry, "address", sockaddr_to_numeric(it->ifa_addr));
          set_string(ctx, entry, "netmask", sockaddr_to_numeric(it->ifa_netmask));
          set_string(ctx, entry, "family", family == AF_INET ? "IPv4" : "IPv6");
          set_string(ctx, entry, "mac", "");
          set_bool(ctx, entry, "internal", (it->ifa_flags & IFF_LOOPBACK) != 0);
          set(ctx, entry, "cidr", Null(iso));
          (void)entries->Set(ctx, entries->Length(), entry);
          ++next_index;
        }
        (void)next_index;
        freeifaddrs(interfaces);
      }
#endif
      info.GetReturnValue().Set(out);
    }

    void os_user_info(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto out = Object::New(iso);
#if defined(_WIN32)
      set_string(ctx, out, "username", getenv_or_empty("USERNAME"));
      set_number(ctx, out, "uid", -1);
      set_number(ctx, out, "gid", -1);
      set_string(ctx, out, "shell", getenv_or_empty("ComSpec"));
      set_string(ctx, out, "homedir", home_dir());
#else
      passwd* pw = getpwuid(getuid());
      set_string(ctx, out, "username", pw && pw->pw_name ? pw->pw_name : getenv_or_empty("USER"));
      set_number(ctx, out, "uid", static_cast<double>(getuid()));
      set_number(ctx, out, "gid", static_cast<double>(getgid()));
      set_string(ctx, out, "shell", pw && pw->pw_shell ? pw->pw_shell : getenv_or_empty("SHELL"));
      set_string(ctx, out, "homedir", pw && pw->pw_dir ? pw->pw_dir : home_dir());
#endif
      info.GetReturnValue().Set(out);
    }

    void tty_isatty(const FunctionCallbackInfo<Value>& info) {
      int fd = 0;
      if (info.Length() > 0)
        fd = info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0);
#if defined(_WIN32)
      const bool result = _isatty(fd) != 0;
#else
      const bool result = ::isatty(fd) == 1;
#endif
      info.GetReturnValue().Set(Boolean::New(info.GetIsolate(), result));
    }

    void tty_get_window_size(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      int fd = 0;
      if (info.Length() > 0)
        fd = info[0]->Int32Value(ctx).FromMaybe(0);
      int columns = 0;
      int rows = 0;
#if defined(_WIN32)
      intptr_t os_handle = _get_osfhandle(fd);
      if (os_handle != -1) {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (GetConsoleScreenBufferInfo(reinterpret_cast<HANDLE>(os_handle), &csbi)) {
          columns = csbi.srWindow.Right - csbi.srWindow.Left + 1;
          rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
      }
#else
      winsize ws{};
      if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        columns = ws.ws_col;
        rows = ws.ws_row;
      }
#endif
      auto array = Array::New(iso, 2);
      (void)array->Set(ctx, 0, Number::New(iso, columns));
      (void)array->Set(ctx, 1, Number::New(iso, rows));
      info.GetReturnValue().Set(array);
    }

    std::string gai_error_message(int err) {
#if defined(_WIN32)
      const char* msg = gai_strerrorA(err);
#else
      const char* msg = gai_strerror(err);
#endif
      return msg ? msg : "getaddrinfo failed";
    }

    Local<Object> make_js_error(Isolate* iso, Local<Context> ctx, std::string_view message,
                                std::string_view code) {
      auto error = Exception::Error(str(iso, message)).As<Object>();
      set_string(ctx, error, "code", code);
      return error;
    }

    void call_dns_lookup_callback(Isolate* iso, Local<Context> ctx, Local<Function> callback,
                                  Local<Value> error, Local<Value> result);
    bool ensure_winsock(Isolate* iso);

    enum class dns_rr_type : std::uint16_t {
      A = 1,
      NS = 2,
      CNAME = 5,
      SOA = 6,
      PTR = 12,
      MX = 15,
      TXT = 16,
      AAAA = 28,
      SRV = 33,
      NAPTR = 35,
      CAA = 257,
    };

    std::optional<dns_rr_type> dns_rr_type_from_string(std::string type) {
      std::transform(type.begin(), type.end(), type.begin(),
                     [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      if (type == "A")
        return dns_rr_type::A;
      if (type == "AAAA")
        return dns_rr_type::AAAA;
      if (type == "MX")
        return dns_rr_type::MX;
      if (type == "NS")
        return dns_rr_type::NS;
      if (type == "SRV")
        return dns_rr_type::SRV;
      if (type == "TXT")
        return dns_rr_type::TXT;
      if (type == "CAA")
        return dns_rr_type::CAA;
      if (type == "CNAME")
        return dns_rr_type::CNAME;
      if (type == "PTR")
        return dns_rr_type::PTR;
      if (type == "SOA")
        return dns_rr_type::SOA;
      if (type == "NAPTR")
        return dns_rr_type::NAPTR;
      return std::nullopt;
    }

    void dns_write_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
      out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
      out.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    std::uint16_t dns_read_u16(const std::vector<std::uint8_t>& data, std::size_t offset) {
      return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) |
                                        static_cast<std::uint16_t>(data[offset + 1]));
    }

    std::uint32_t dns_read_u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
      return (static_cast<std::uint32_t>(data[offset]) << 24) |
             (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
             (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
             static_cast<std::uint32_t>(data[offset + 3]);
    }

    bool dns_encode_name(std::string_view name, std::vector<std::uint8_t>& out) {
      if (name.empty())
        return false;
      std::size_t start = 0;
      while (start < name.size()) {
        const std::size_t dot = name.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? name.size() : dot;
        const std::size_t len = end - start;
        if (len == 0) {
          start = end + 1;
          continue;
        }
        if (len > 63)
          return false;
        out.push_back(static_cast<std::uint8_t>(len));
        out.insert(out.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                   name.begin() + static_cast<std::ptrdiff_t>(end));
        if (dot == std::string_view::npos)
          break;
        start = end + 1;
      }
      out.push_back(0);
      return true;
    }

    bool dns_expand_name(const std::vector<std::uint8_t>& data, std::size_t& offset,
                         std::string& out, int depth = 0) {
      if (depth > 16)
        return false;
      std::size_t pos = offset;
      std::size_t consumed = 0;
      bool jumped = false;
      out.clear();
      for (;;) {
        if (pos >= data.size())
          return false;
        const std::uint8_t len = data[pos++];
        if ((len & 0xc0) == 0xc0) {
          if (pos >= data.size())
            return false;
          const std::size_t ptr =
              (static_cast<std::size_t>(len & 0x3f) << 8) | static_cast<std::size_t>(data[pos++]);
          if (!jumped)
            consumed = pos - offset;
          jumped = true;
          std::size_t nested = ptr;
          std::string suffix;
          if (!dns_expand_name(data, nested, suffix, depth + 1))
            return false;
          if (!out.empty() && !suffix.empty())
            out.push_back('.');
          out.append(suffix);
          break;
        }
        if ((len & 0xc0) != 0)
          return false;
        if (len == 0) {
          if (!jumped)
            consumed = pos - offset;
          break;
        }
        if (pos + len > data.size())
          return false;
        if (!out.empty())
          out.push_back('.');
        out.append(reinterpret_cast<const char*>(data.data() + pos), len);
        pos += len;
      }
      offset += consumed;
      return true;
    }

    std::string dns_default_server() {
      if (const char* env = std::getenv("FXE_DNS_SERVER"); env && *env)
        return env;
#if !defined(_WIN32)
      std::ifstream resolv("/etc/resolv.conf");
      std::string line;
      while (std::getline(resolv, line)) {
        std::istringstream iss(line);
        std::string key;
        std::string value;
        if (iss >> key >> value && key == "nameserver" && !value.empty())
          return value;
      }
#endif
      return "1.1.1.1";
    }

    bool dns_query_packet(Isolate* iso, Local<Context> ctx, std::string_view hostname,
                          dns_rr_type rrtype, std::vector<std::uint8_t>& response,
                          Local<Object>& error) {
      if (!ensure_winsock(iso)) {
        error = make_js_error(iso, ctx, "socket initialization failed", "EAI_FAIL");
        return false;
      }
      std::vector<std::uint8_t> query;
      query.reserve(512);
      const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
      const std::uint16_t id = static_cast<std::uint16_t>(now & 0xffff);
      dns_write_u16(query, id);
      dns_write_u16(query, 0x0100);
      dns_write_u16(query, 1);
      dns_write_u16(query, 0);
      dns_write_u16(query, 0);
      dns_write_u16(query, 0);
      if (!dns_encode_name(hostname, query)) {
        error = make_js_error(iso, ctx, "invalid DNS hostname", "EINVAL");
        return false;
      }
      dns_write_u16(query, static_cast<std::uint16_t>(rrtype));
      dns_write_u16(query, 1);

      const std::string server = dns_default_server();
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_DGRAM;
      addrinfo* server_info = nullptr;
      const int gai = getaddrinfo(server.c_str(), "53", &hints, &server_info);
      if (gai != 0 || !server_info) {
        error = make_js_error(iso, ctx, "DNS resolver address lookup failed", "EAI_FAIL");
        return false;
      }

#if defined(_WIN32)
      SOCKET fd = INVALID_SOCKET;
#else
      int fd = -1;
#endif
      addrinfo* selected = nullptr;
      for (addrinfo* it = server_info; it != nullptr; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
#if defined(_WIN32)
        if (fd != INVALID_SOCKET) {
#else
        if (fd >= 0) {
#endif
          selected = it;
          break;
        }
      }
      if (!selected) {
        freeaddrinfo(server_info);
        error = make_js_error(iso, ctx, "DNS resolver socket creation failed", "EAI_FAIL");
        return false;
      }

#if defined(_WIN32)
      DWORD timeout_ms = 3000;
      (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                       sizeof(timeout_ms));
#else
      timeval timeout{};
      timeout.tv_sec = 3;
      timeout.tv_usec = 0;
      (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
      const ssize_t sent = sendto(fd, reinterpret_cast<const char*>(query.data()), query.size(), 0,
                                  selected->ai_addr, static_cast<socklen_t>(selected->ai_addrlen));
      freeaddrinfo(server_info);
      if (sent != static_cast<ssize_t>(query.size())) {
#if defined(_WIN32)
        closesocket(fd);
#else
        close(fd);
#endif
        error = make_js_error(iso, ctx, "DNS query send failed", "EAI_AGAIN");
        return false;
      }

      response.assign(4096, 0);
      const ssize_t received = recvfrom(fd, reinterpret_cast<char*>(response.data()),
                                        response.size(), 0, nullptr, nullptr);
#if defined(_WIN32)
      closesocket(fd);
#else
      close(fd);
#endif
      if (received < 12) {
        error = make_js_error(iso, ctx, "DNS query timed out or returned a short response",
                              "EAI_AGAIN");
        return false;
      }
      response.resize(static_cast<std::size_t>(received));
      if (dns_read_u16(response, 0) != id) {
        error = make_js_error(iso, ctx, "DNS response id mismatch", "EAI_FAIL");
        return false;
      }
      const std::uint16_t flags = dns_read_u16(response, 2);
      const int rcode = flags & 0x000f;
      if (rcode == 3) {
        error = make_js_error(iso, ctx, "DNS name not found", "ENOTFOUND");
        return false;
      }
      if (rcode != 0) {
        error = make_js_error(iso, ctx, "DNS resolver returned an error", "EAI_FAIL");
        return false;
      }
      return true;
    }

    bool dns_parse_character_string(const std::vector<std::uint8_t>& packet, std::size_t& offset,
                                    std::size_t end, std::string& out) {
      if (offset >= end)
        return false;
      const std::size_t len = packet[offset++];
      if (offset + len > end)
        return false;
      out.assign(reinterpret_cast<const char*>(packet.data() + offset), len);
      offset += len;
      return true;
    }

    bool dns_parse_records(Isolate* iso, Local<Context> ctx,
                           const std::vector<std::uint8_t>& packet, dns_rr_type requested,
                           Local<Value>& out, Local<Object>& error) {
      if (packet.size() < 12) {
        error = make_js_error(iso, ctx, "short DNS response", "EAI_FAIL");
        return false;
      }
      const std::uint16_t qd = dns_read_u16(packet, 4);
      const std::uint16_t an = dns_read_u16(packet, 6);
      std::size_t offset = 12;
      for (std::uint16_t i = 0; i < qd; ++i) {
        std::string ignored;
        if (!dns_expand_name(packet, offset, ignored) || offset + 4 > packet.size()) {
          error = make_js_error(iso, ctx, "malformed DNS question", "EAI_FAIL");
          return false;
        }
        offset += 4;
      }

      auto array = Array::New(iso);
      std::uint32_t index = 0;
      Local<Value> singleton;
      for (std::uint16_t i = 0; i < an; ++i) {
        std::string owner;
        if (!dns_expand_name(packet, offset, owner) || offset + 10 > packet.size()) {
          error = make_js_error(iso, ctx, "malformed DNS answer", "EAI_FAIL");
          return false;
        }
        const auto type = static_cast<dns_rr_type>(dns_read_u16(packet, offset));
        offset += 2;
        const std::uint16_t klass = dns_read_u16(packet, offset);
        offset += 2;
        offset += 4;
        const std::uint16_t rdlen = dns_read_u16(packet, offset);
        offset += 2;
        const std::size_t rdata = offset;
        const std::size_t end = offset + rdlen;
        if (end > packet.size()) {
          error = make_js_error(iso, ctx, "truncated DNS rdata", "EAI_FAIL");
          return false;
        }
        if (klass != 1 || type != requested) {
          offset = end;
          continue;
        }

        std::size_t pos = rdata;
        if (requested == dns_rr_type::NS || requested == dns_rr_type::CNAME ||
            requested == dns_rr_type::PTR) {
          std::string name;
          if (!dns_expand_name(packet, pos, name)) {
            error = make_js_error(iso, ctx, "malformed DNS name rdata", "EAI_FAIL");
            return false;
          }
          (void)array->Set(ctx, index++, str(iso, name));
        } else if (requested == dns_rr_type::MX) {
          if (pos + 2 > end) {
            error = make_js_error(iso, ctx, "malformed MX rdata", "EAI_FAIL");
            return false;
          }
          const int priority = dns_read_u16(packet, pos);
          pos += 2;
          std::string exchange;
          if (!dns_expand_name(packet, pos, exchange)) {
            error = make_js_error(iso, ctx, "malformed MX exchange", "EAI_FAIL");
            return false;
          }
          auto record = Object::New(iso);
          set_number(ctx, record, "priority", priority);
          set_string(ctx, record, "exchange", exchange);
          (void)array->Set(ctx, index++, record);
        } else if (requested == dns_rr_type::SRV) {
          if (pos + 6 > end) {
            error = make_js_error(iso, ctx, "malformed SRV rdata", "EAI_FAIL");
            return false;
          }
          const int priority = dns_read_u16(packet, pos);
          const int weight = dns_read_u16(packet, pos + 2);
          const int port = dns_read_u16(packet, pos + 4);
          pos += 6;
          std::string name;
          if (!dns_expand_name(packet, pos, name)) {
            error = make_js_error(iso, ctx, "malformed SRV target", "EAI_FAIL");
            return false;
          }
          auto record = Object::New(iso);
          set_number(ctx, record, "priority", priority);
          set_number(ctx, record, "weight", weight);
          set_number(ctx, record, "port", port);
          set_string(ctx, record, "name", name);
          (void)array->Set(ctx, index++, record);
        } else if (requested == dns_rr_type::TXT) {
          auto chunks = Array::New(iso);
          std::uint32_t chunk_index = 0;
          while (pos < end) {
            std::string chunk;
            if (!dns_parse_character_string(packet, pos, end, chunk)) {
              error = make_js_error(iso, ctx, "malformed TXT rdata", "EAI_FAIL");
              return false;
            }
            (void)chunks->Set(ctx, chunk_index++, str(iso, chunk));
          }
          (void)array->Set(ctx, index++, chunks);
        } else if (requested == dns_rr_type::CAA) {
          if (pos + 2 > end) {
            error = make_js_error(iso, ctx, "malformed CAA rdata", "EAI_FAIL");
            return false;
          }
          const int critical = packet[pos++];
          const std::size_t tag_len = packet[pos++];
          if (pos + tag_len > end) {
            error = make_js_error(iso, ctx, "malformed CAA tag", "EAI_FAIL");
            return false;
          }
          std::string tag(reinterpret_cast<const char*>(packet.data() + pos), tag_len);
          pos += tag_len;
          std::string value(reinterpret_cast<const char*>(packet.data() + pos), end - pos);
          auto record = Object::New(iso);
          set_number(ctx, record, "critical", critical);
          set_string(ctx, record, tag.c_str(), value);
          (void)array->Set(ctx, index++, record);
        } else if (requested == dns_rr_type::SOA) {
          std::string nsname;
          std::string hostmaster;
          if (!dns_expand_name(packet, pos, nsname) || !dns_expand_name(packet, pos, hostmaster) ||
              pos + 20 > end) {
            error = make_js_error(iso, ctx, "malformed SOA rdata", "EAI_FAIL");
            return false;
          }
          auto record = Object::New(iso);
          set_string(ctx, record, "nsname", nsname);
          set_string(ctx, record, "hostmaster", hostmaster);
          set_number(ctx, record, "serial", dns_read_u32(packet, pos));
          set_number(ctx, record, "refresh", dns_read_u32(packet, pos + 4));
          set_number(ctx, record, "retry", dns_read_u32(packet, pos + 8));
          set_number(ctx, record, "expire", dns_read_u32(packet, pos + 12));
          set_number(ctx, record, "minttl", dns_read_u32(packet, pos + 16));
          singleton = record;
          index = 1;
        } else if (requested == dns_rr_type::NAPTR) {
          if (pos + 4 > end) {
            error = make_js_error(iso, ctx, "malformed NAPTR rdata", "EAI_FAIL");
            return false;
          }
          const int order = dns_read_u16(packet, pos);
          const int preference = dns_read_u16(packet, pos + 2);
          pos += 4;
          std::string flags;
          std::string service;
          std::string regexp;
          std::string replacement;
          if (!dns_parse_character_string(packet, pos, end, flags) ||
              !dns_parse_character_string(packet, pos, end, service) ||
              !dns_parse_character_string(packet, pos, end, regexp) ||
              !dns_expand_name(packet, pos, replacement)) {
            error = make_js_error(iso, ctx, "malformed NAPTR rdata", "EAI_FAIL");
            return false;
          }
          auto record = Object::New(iso);
          set_number(ctx, record, "order", order);
          set_number(ctx, record, "preference", preference);
          set_string(ctx, record, "flags", flags);
          set_string(ctx, record, "service", service);
          set_string(ctx, record, "regexp", regexp);
          set_string(ctx, record, "replacement", replacement);
          (void)array->Set(ctx, index++, record);
        }
        offset = end;
      }

      if (index == 0) {
        error = make_js_error(iso, ctx, "DNS record type has no data", "ENODATA");
        return false;
      }
      if (requested == dns_rr_type::SOA && !singleton.IsEmpty())
        out = singleton;
      else
        out = array.As<Value>();
      return true;
    }

    void dns_resolve_record(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString() || !info[1]->IsString() ||
          !info[2]->IsFunction()) {
        throw_error(iso, "__fxe_native.dns.resolveRecord requires hostname, rrtype, callback");
        return;
      }
      String::Utf8Value hostname_value(iso, info[0]);
      String::Utf8Value rrtype_value(iso, info[1]);
      const std::string hostname(*hostname_value ? *hostname_value : "");
      const std::string rrtype_string(*rrtype_value ? *rrtype_value : "");
      auto rrtype = dns_rr_type_from_string(rrtype_string);
      if (!rrtype) {
        call_dns_lookup_callback(iso, ctx, info[2].As<Function>(),
                                 make_js_error(iso, ctx, "unsupported DNS record type", "ENODATA"),
                                 Undefined(iso));
        return;
      }
      std::vector<std::uint8_t> packet;
      Local<Object> error;
      if (!dns_query_packet(iso, ctx, hostname, *rrtype, packet, error)) {
        call_dns_lookup_callback(iso, ctx, info[2].As<Function>(), error, Undefined(iso));
        return;
      }
      Local<Value> records;
      if (!dns_parse_records(iso, ctx, packet, *rrtype, records, error)) {
        call_dns_lookup_callback(iso, ctx, info[2].As<Function>(), error, Undefined(iso));
        return;
      }
      call_dns_lookup_callback(iso, ctx, info[2].As<Function>(), Null(iso), records);
    }

    void call_dns_lookup_callback(Isolate* iso, Local<Context> ctx, Local<Function> callback,
                                  Local<Value> error, Local<Value> result) {
      Local<Value> argv[] = {error, result};
      Local<Value> ignored;
      (void)callback->Call(ctx, Undefined(iso), 2, argv).ToLocal(&ignored);
    }

    int dns_family_from_options(Isolate* iso, Local<Context> ctx, Local<Value> options) {
      if (!options->IsObject())
        return AF_UNSPEC;
      Local<Value> family_value;
      if (!options.As<Object>()->Get(ctx, str(iso, "family")).ToLocal(&family_value))
        return AF_UNSPEC;
      const int family = family_value->Int32Value(ctx).FromMaybe(0);
      if (family == 4)
        return AF_INET;
      if (family == 6)
        return AF_INET6;
      return AF_UNSPEC;
    }

    bool dns_all_from_options(Isolate* iso, Local<Context> ctx, Local<Value> options) {
      if (!options->IsObject())
        return false;
      Local<Value> all_value;
      if (!options.As<Object>()->Get(ctx, str(iso, "all")).ToLocal(&all_value))
        return false;
      return all_value->BooleanValue(iso);
    }

    std::string numeric_address_for(const addrinfo* info) {
      if (!info || !info->ai_addr)
        return {};
      char host[NI_MAXHOST] = {};
      if (getnameinfo(info->ai_addr, static_cast<socklen_t>(info->ai_addrlen), host, sizeof(host),
                      nullptr, 0, NI_NUMERICHOST) != 0)
        return {};
      return host;
    }

    void free_dns_addrinfo(addrinfo* results, [[maybe_unused]] bool from_uv = false) {
#if FXE_HAS_LIBUV
      if (from_uv) {
        uv_freeaddrinfo(results);
        return;
      }
#endif
      freeaddrinfo(results);
    }

    Local<Object> make_dns_result(Isolate* iso, Local<Context> ctx, const addrinfo* info,
                                  std::string_view address) {
      auto result = Object::New(iso);
      set_string(ctx, result, "address", address);
      set_number(ctx, result, "family", info->ai_family == AF_INET6 ? 6 : 4);
      return result;
    }

    void complete_dns_lookup(Isolate* iso, Local<Context> ctx, Local<Function> callback, int rc,
                             addrinfo* results, std::string_view hostname, bool all,
                             bool from_uv = false) {
      if (rc != 0) {
        auto error = make_js_error(iso, ctx,
                                   std::string("getaddrinfo(") + std::string(hostname) +
                                       ") failed: " + gai_error_message(rc),
                                   rc == EAI_AGAIN ? "EAI_AGAIN" : "ENOTFOUND");
        call_dns_lookup_callback(iso, ctx, callback, error, Undefined(iso));
        return;
      }

      if (all) {
        auto array = Array::New(iso);
        uint32_t index = 0;
        for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
          if (it->ai_family != AF_INET && it->ai_family != AF_INET6)
            continue;
          const std::string address = numeric_address_for(it);
          if (address.empty())
            continue;
          (void)array->Set(ctx, index++, make_dns_result(iso, ctx, it, address));
        }
        free_dns_addrinfo(results, from_uv);
        call_dns_lookup_callback(iso, ctx, callback, Null(iso), array);
        return;
      }

      const addrinfo* selected = nullptr;
      for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET || it->ai_family == AF_INET6) {
          selected = it;
          break;
        }
      }
      if (!selected) {
        if (results)
          free_dns_addrinfo(results, from_uv);
        call_dns_lookup_callback(
            iso, ctx, callback,
            make_js_error(iso, ctx, "getaddrinfo returned no IPv4/IPv6 address", "ENOTFOUND"),
            Undefined(iso));
        return;
      }

      const std::string address = numeric_address_for(selected);
      if (address.empty()) {
        free_dns_addrinfo(results, from_uv);
        call_dns_lookup_callback(
            iso, ctx, callback,
            make_js_error(iso, ctx, "getnameinfo failed for DNS result", "EAI_FAIL"),
            Undefined(iso));
        return;
      }

      auto result = make_dns_result(iso, ctx, selected, address);
      free_dns_addrinfo(results, from_uv);
      call_dns_lookup_callback(iso, ctx, callback, Null(iso), result);
    }

    void dns_lookup(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.dns.lookup requires a hostname string");
        return;
      }

      Local<Value> options = info.Length() > 1 ? info[1] : Object::New(iso).As<Value>();
      Local<Value> callback_value =
          info.Length() > 2 ? info[2] : (info.Length() > 1 ? info[1] : Local<Value>());
      if (callback_value.IsEmpty() || !callback_value->IsFunction()) {
        throw_error(iso, "__fxe_native.dns.lookup requires a callback");
        return;
      }
      auto callback = callback_value.As<Function>();

#if defined(_WIN32)
      static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
      }();
      if (!winsock_ready) {
        call_dns_lookup_callback(iso, ctx, callback,
                                 make_js_error(iso, ctx, "WSAStartup failed", "EAI_FAIL"),
                                 Undefined(iso));
        return;
      }
#endif

      String::Utf8Value hostname_value(iso, info[0]);
      const std::string hostname(*hostname_value ? *hostname_value : "");
      addrinfo hints{};
      hints.ai_family = dns_family_from_options(iso, ctx, options);
      hints.ai_socktype = SOCK_STREAM;
      const bool all = dns_all_from_options(iso, ctx, options);

#if FXE_HAS_LIBUV
      if (auto* loop = uv_loop_runtime::instance().loop()) {
        uv_getaddrinfo_t req{};
        const int rc = uv_getaddrinfo(loop, &req, nullptr, hostname.c_str(), nullptr, &hints);
        complete_dns_lookup(iso, ctx, callback, rc, req.addrinfo, hostname, all, true);
        return;
      }
#endif

      addrinfo* results = nullptr;
      const int rc = getaddrinfo(hostname.c_str(), nullptr, &hints, &results);
      complete_dns_lookup(iso, ctx, callback, rc, results, hostname, all);
    }

    void dns_lookup_service(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString() || !info[2]->IsFunction()) {
        throw_error(iso, "__fxe_native.dns.lookupService requires address, port, callback");
        return;
      }
#if defined(_WIN32)
      static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
      }();
      if (!winsock_ready) {
        call_dns_lookup_callback(iso, ctx, info[2].As<Function>(),
                                 make_js_error(iso, ctx, "WSAStartup failed", "EAI_FAIL"),
                                 Undefined(iso));
        return;
      }
#endif
      String::Utf8Value address_value(iso, info[0]);
      const std::string address(*address_value ? *address_value : "");
      const int port = info[1]->Int32Value(ctx).FromMaybe(0);
      sockaddr_storage storage{};
      socklen_t len = 0;
      auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
      auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
      if (inet_pton(AF_INET, address.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(static_cast<std::uint16_t>(port));
        len = sizeof(sockaddr_in);
      } else if (inet_pton(AF_INET6, address.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(static_cast<std::uint16_t>(port));
        len = sizeof(sockaddr_in6);
      } else {
        call_dns_lookup_callback(iso, ctx, info[2].As<Function>(),
                                 make_js_error(iso, ctx, "invalid IP address", "EINVAL"),
                                 Undefined(iso));
        return;
      }
      char host[NI_MAXHOST] = {};
      char service[NI_MAXSERV] = {};
      const int rc = getnameinfo(reinterpret_cast<sockaddr*>(&storage), len, host, sizeof(host),
                                 service, sizeof(service), NI_NAMEREQD);
      if (rc != 0) {
        call_dns_lookup_callback(iso, ctx, info[2].As<Function>(),
                                 make_js_error(iso, ctx, gai_error_message(rc), "ENOTFOUND"),
                                 Undefined(iso));
        return;
      }
      auto result = Object::New(iso);
      set_string(ctx, result, "hostname", host);
      set_string(ctx, result, "service", service);
      call_dns_lookup_callback(iso, ctx, info[2].As<Function>(), Null(iso), result);
    }

#if defined(_WIN32)
    bool ensure_winsock(Isolate* iso) {
      static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
      }();
      if (!ready)
        throw_error(iso, "WSAStartup failed");
      return ready;
    }

    using socket_handle = SOCKET;
    constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;

    int last_socket_error() {
      return WSAGetLastError();
    }

    std::string socket_error_message(int err) {
      char* message = nullptr;
      const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                          FORMAT_MESSAGE_IGNORE_INSERTS;
      const DWORD len = FormatMessageA(flags, nullptr, static_cast<DWORD>(err), 0,
                                       reinterpret_cast<LPSTR>(&message), 0, nullptr);
      std::string out =
          len > 0 && message ? std::string(message, len) : "winsock error " + std::to_string(err);
      if (message)
        LocalFree(message);
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
      return out;
    }

    bool would_block_error(int err) {
      return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
    }

    void close_socket_handle(socket_handle fd) {
      if (fd != invalid_socket_handle)
        (void)closesocket(fd);
    }

    bool set_nonblocking_socket(socket_handle fd) {
      u_long mode = 1;
      return ioctlsocket(fd, FIONBIO, &mode) == 0;
    }

    socket_handle socket_arg(Local<Context> ctx, const FunctionCallbackInfo<Value>& info) {
      return info.Length() > 0
                 ? static_cast<socket_handle>(info[0]->IntegerValue(ctx).FromMaybe(-1))
                 : invalid_socket_handle;
    }
#else
    using socket_handle = int;
    constexpr socket_handle invalid_socket_handle = -1;

    bool ensure_winsock(Isolate*) {
      return true;
    }

    int last_socket_error() {
      return errno;
    }

    std::string socket_error_message(int err) {
      return std::strerror(err);
    }

    bool would_block_error(int err) {
      return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS;
    }

    void close_socket_handle(socket_handle fd) {
      if (fd >= 0)
        (void)close(fd);
    }

    bool set_nonblocking_socket(socket_handle fd) {
      const int flags = fcntl(fd, F_GETFL, 0);
      return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    socket_handle socket_arg(Local<Context> ctx, const FunctionCallbackInfo<Value>& info) {
      return info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
    }
#endif

    bool resolve_socket_addr(Isolate* iso, std::string_view host, int port, int socktype,
                             int protocol, bool passive, int family, sockaddr_storage& addr,
                             socklen_t& addr_len) {
      if (!ensure_winsock(iso))
        return false;
      if (port < 0 || port > 65535) {
        throw_error(iso, "socket port must be between 0 and 65535");
        return false;
      }
      addrinfo hints{};
      hints.ai_family = family;
      hints.ai_socktype = socktype;
      hints.ai_protocol = protocol;
      if (passive)
        hints.ai_flags |= AI_PASSIVE;
      const std::string host_string(host);
      const std::string service = std::to_string(port);
      addrinfo* results = nullptr;
      const char* node = host_string.empty() ? nullptr : host_string.c_str();
      const int rc = getaddrinfo(node, service.c_str(), &hints, &results);
      if (rc != 0 || !results) {
        throw_error(iso, std::string(socktype == SOCK_STREAM ? "tcp" : "udp") +
                             " address resolution failed: " + gai_error_message(rc));
        return false;
      }
      for (addrinfo* it = results; it; it = it->ai_next) {
        if ((it->ai_family == AF_INET || it->ai_family == AF_INET6) &&
            it->ai_addrlen <= sizeof(addr)) {
          std::memcpy(&addr, it->ai_addr, it->ai_addrlen);
          addr_len = static_cast<socklen_t>(it->ai_addrlen);
          freeaddrinfo(results);
          return true;
        }
      }
      freeaddrinfo(results);
      throw_error(iso, "socket address resolution returned no IPv4 or IPv6 address");
      return false;
    }

    bool resolve_tcp_addr(Isolate* iso, std::string_view host, int port, bool passive,
                          sockaddr_storage& addr, socklen_t& addr_len) {
      return resolve_socket_addr(iso, host, port, SOCK_STREAM, IPPROTO_TCP, passive, AF_UNSPEC,
                                 addr, addr_len);
    }

    bool resolve_udp_addr(Isolate* iso, std::string_view host, int port, bool passive, int family,
                          sockaddr_storage& addr, socklen_t& addr_len) {
      return resolve_socket_addr(iso, host, port, SOCK_DGRAM, IPPROTO_UDP, passive, family, addr,
                                 addr_len);
    }

    Local<Object> make_socket_error(Isolate* iso, Local<Context> ctx, int err) {
      auto obj = Object::New(iso);
      set_string(ctx, obj, "error", socket_error_message(err));
      set_number(ctx, obj, "errno", err);
      return obj;
    }

    Local<Object> make_sockaddr_object(Isolate* iso, Local<Context> ctx, const sockaddr* addr,
                                       socklen_t addr_len) {
      char host[NI_MAXHOST] = {};
      char service[NI_MAXSERV] = {};
      const int rc = getnameinfo(addr, addr_len, host, sizeof(host), service, sizeof(service),
                                 NI_NUMERICHOST | NI_NUMERICSERV);
      auto obj = Object::New(iso);
      set_string(ctx, obj, "address", rc == 0 ? host : "");
      set_string(ctx, obj, "family", addr && addr->sa_family == AF_INET6 ? "IPv6" : "IPv4");
      set_number(ctx, obj, "port", rc == 0 ? std::strtol(service, nullptr, 10) : 0);
      return obj;
    }

    std::string string_arg(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      return std::string(*utf8 ? *utf8 : "");
    }

    int socket_family_arg(Isolate* iso, Local<Value> value) {
      const std::string family = string_arg(iso, value);
      if (family == "udp4" || family == "IPv4" || family == "4")
        return AF_INET;
      if (family == "udp6" || family == "IPv6" || family == "6")
        return AF_INET6;
      return AF_UNSPEC;
    }

    int int_arg(Local<Context> ctx, Local<Value> value, int fallback = 0) {
      return value->Int32Value(ctx).FromMaybe(fallback);
    }

    std::string socket_policy_endpoint(std::string_view host, int port) {
      std::string endpoint;
      if (host.find(':') != std::string_view::npos && (host.empty() || host.front() != '[')) {
        endpoint.reserve(host.size() + 3 + 5);
        endpoint.push_back('[');
        endpoint.append(host);
        endpoint.push_back(']');
      } else {
        endpoint.append(host);
      }
      endpoint.push_back(':');
      endpoint.append(std::to_string(port));
      return endpoint;
    }

    bool guard_net_socket(Isolate* iso, std::string_view host, int port) {
      const auto endpoint = socket_policy_endpoint(host, port);
      if (fxe::runtime::net_host_allowed(endpoint))
        return true;
      std::string msg = "network access denied for '";
      msg.append(endpoint);
      msg.push_back('\'');
      iso->ThrowException(make_permission_denied(iso, msg));
      return false;
    }

    bool guard_ipc_path(Isolate* iso, std::string_view path) {
      // AF_UNIX sockets and Windows named pipes address filesystem-like paths.
      // Gate them with fs permissions; capabilities.net is hostname-scoped.
      if (fxe::runtime::fs_path_allowed(path))
        return true;
      std::string msg = "fs access denied for '";
      msg.append(path);
      msg.push_back('\'');
      iso->ThrowException(make_permission_denied(iso, msg));
      return false;
    }

    void close_fd(const FunctionCallbackInfo<Value>& info) {
      close_socket_handle(socket_arg(info.GetIsolate()->GetCurrentContext(), info));
    }

    void tcp_listen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string host = info.Length() > 0 ? string_arg(iso, info[0]) : "127.0.0.1";
      const int port = info.Length() > 1 ? int_arg(ctx, info[1]) : 0;
      if (!guard_net_socket(iso, host, port))
        return;
      sockaddr_storage addr{};
      socklen_t addr_len = 0;
      if (!resolve_tcp_addr(iso, host, port, true, addr, addr_len))
        return;
      const socket_handle fd = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
      if (fd == invalid_socket_handle) {
        throw_error(iso, std::string("socket(SOCK_STREAM) failed: ") +
                             socket_error_message(last_socket_error()));
        return;
      }
      const int yes = 1;
#if defined(_WIN32)
      (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                       sizeof(yes));
#else
      (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
      if (!set_nonblocking_socket(fd) ||
          bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0 || listen(fd, 32) != 0) {
        const int err = last_socket_error();
        close_socket_handle(fd);
        throw_error(iso, std::string("tcp listen failed: ") + socket_error_message(err));
        return;
      }
      addr_len = sizeof(addr);
      (void)getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
      auto out = make_sockaddr_object(iso, ctx, reinterpret_cast<sockaddr*>(&addr), addr_len);
      set_number(ctx, out, "fd", static_cast<double>(fd));
      info.GetReturnValue().Set(out);
    }

    void tcp_accept(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      sockaddr_storage addr{};
      socklen_t len = sizeof(addr);
      const socket_handle client =
          accept(socket_arg(ctx, info), reinterpret_cast<sockaddr*>(&addr), &len);
      if (client == invalid_socket_handle) {
        const int err = last_socket_error();
        if (would_block_error(err)) {
          info.GetReturnValue().Set(Null(iso));
          return;
        }
        info.GetReturnValue().Set(make_socket_error(iso, ctx, err));
        return;
      }
      (void)set_nonblocking_socket(client);
      auto out = make_sockaddr_object(iso, ctx, reinterpret_cast<sockaddr*>(&addr), len);
      set_number(ctx, out, "fd", static_cast<double>(client));
      info.GetReturnValue().Set(out);
    }

    void tcp_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string host = info.Length() > 0 ? string_arg(iso, info[0]) : "127.0.0.1";
      const int port = info.Length() > 1 ? int_arg(ctx, info[1]) : 0;
      if (!guard_net_socket(iso, host, port))
        return;
      sockaddr_storage addr{};
      socklen_t addr_len = 0;
      if (!resolve_tcp_addr(iso, host, port, false, addr, addr_len))
        return;
      const socket_handle fd = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
      if (fd == invalid_socket_handle) {
        throw_error(iso, std::string("socket(SOCK_STREAM) failed: ") +
                             socket_error_message(last_socket_error()));
        return;
      }
      if (!set_nonblocking_socket(fd)) {
        const int err = last_socket_error();
        close_socket_handle(fd);
        throw_error(iso,
                    std::string("nonblocking socket setup failed: ") + socket_error_message(err));
        return;
      }
      const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), addr_len);
      const int err = rc == 0 ? 0 : last_socket_error();
      if (rc != 0 && !would_block_error(err)) {
        close_socket_handle(fd);
        throw_error(iso, std::string("tcp connect failed: ") + socket_error_message(err));
        return;
      }
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", static_cast<double>(fd));
      set_bool(ctx, out, "connected", rc == 0);
      info.GetReturnValue().Set(out);
    }

    void tcp_finish_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      int err = 0;
      socklen_t len = sizeof(err);
#if defined(_WIN32)
      if (getsockopt(socket_arg(ctx, info), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err),
                     &len) != 0) {
#else
      if (getsockopt(socket_arg(ctx, info), SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
#endif
        info.GetReturnValue().Set(make_socket_error(iso, ctx, last_socket_error()));
        return;
      }
      auto out = Object::New(iso);
      set_bool(ctx, out, "connected", err == 0);
      if (err != 0 && !would_block_error(err)) {
        set_string(ctx, out, "error", socket_error_message(err));
        set_number(ctx, out, "errno", err);
      }
      info.GetReturnValue().Set(out);
    }

    void tcp_read(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      std::array<std::uint8_t, 65536> buf{};
      const auto fd = socket_arg(iso->GetCurrentContext(), info);
#if defined(_WIN32)
      const int n = recv(fd, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
#else
      const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
#endif
      if (n < 0) {
        const int err = last_socket_error();
        if (would_block_error(err)) {
          info.GetReturnValue().Set(Null(iso));
          return;
        }
        info.GetReturnValue().Set(make_socket_error(iso, iso->GetCurrentContext(), err));
        return;
      }
      auto out = Object::New(iso);
      if (n == 0) {
        set_bool(iso->GetCurrentContext(), out, "eof", true);
        info.GetReturnValue().Set(out);
        return;
      }
      auto backing = ArrayBuffer::NewBackingStore(iso, static_cast<std::size_t>(n));
      std::memcpy(backing->Data(), buf.data(), static_cast<std::size_t>(n));
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      (void)out->Set(iso->GetCurrentContext(), str(iso, "data"),
                     Uint8Array::New(buffer, 0, static_cast<std::size_t>(n)));
      info.GetReturnValue().Set(out);
    }

    void tcp_write(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_view bytes{};
      if (info.Length() < 2 || !value_to_bytes(iso, ctx, info[1], bytes)) {
        throw_error(iso, "tcp.write requires fd and string/bytes");
        return;
      }
      std::size_t written = 0;
      const auto fd = socket_arg(ctx, info);
      while (written < bytes.size) {
#if defined(_WIN32)
        const int n = send(
            fd, reinterpret_cast<const char*>(bytes.data + written),
            static_cast<int>(std::min<std::size_t>(
                bytes.size - written, static_cast<std::size_t>(std::numeric_limits<int>::max()))),
            0);
#else
        const ssize_t n = send(fd, bytes.data + written, bytes.size - written, 0);
#endif
        if (n < 0) {
          const int err = last_socket_error();
          if (would_block_error(err))
            break;
          throw_error(iso, std::string("tcp write failed: ") + socket_error_message(err));
          return;
        }
        written += static_cast<std::size_t>(n);
      }
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(written)));
    }

    void tcp_shutdown(const FunctionCallbackInfo<Value>& info) {
      const auto fd = socket_arg(info.GetIsolate()->GetCurrentContext(), info);
      if (fd != invalid_socket_handle)
        (void)shutdown(fd,
#if defined(_WIN32)
                       SD_SEND
#else
                       SHUT_WR
#endif
        );
    }

    void socket_address(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      sockaddr_storage addr{};
      socklen_t len = sizeof(addr);
      if (getsockname(socket_arg(ctx, info), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      info.GetReturnValue().Set(
          make_sockaddr_object(iso, ctx, reinterpret_cast<sockaddr*>(&addr), len));
    }

#if defined(_WIN32)
    struct ipcsock_pipe_state {
      HANDLE handle = INVALID_HANDLE_VALUE;
      std::wstring path;
      bool listener = false;
    };

    std::mutex& ipcsock_registry_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<int, ipcsock_pipe_state>& ipcsock_registry() {
      static std::unordered_map<int, ipcsock_pipe_state> registry;
      return registry;
    }

    int next_ipcsock_id() {
      static std::atomic<int> id{1};
      return id.fetch_add(1);
    }

    HANDLE create_ipcsock_listener(const std::wstring& path) {
      return CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                              PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    }

    void set_pipe_nowait(HANDLE handle) {
      DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
      (void)SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
    }

    int register_ipcsock_pipe(ipcsock_pipe_state state) {
      const int id = next_ipcsock_id();
      std::lock_guard<std::mutex> lock(ipcsock_registry_mutex());
      ipcsock_registry()[id] = std::move(state);
      return id;
    }

    HANDLE ipcsock_handle_arg(Local<Context> ctx, const FunctionCallbackInfo<Value>& info) {
      const int id = int_arg(ctx, info[0], 0);
      std::lock_guard<std::mutex> lock(ipcsock_registry_mutex());
      auto it = ipcsock_registry().find(id);
      return it == ipcsock_registry().end() ? INVALID_HANDLE_VALUE : it->second.handle;
    }

    void ipcsock_listen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string path = info.Length() > 0 ? string_arg(iso, info[0]) : "";
      if (!guard_ipc_path(iso, path))
        return;
      const std::wstring wide_path = widen_utf8(path);
      HANDLE handle = create_ipcsock_listener(wide_path);
      if (handle == INVALID_HANDLE_VALUE) {
        throw_error(iso, std::string("ipcsock listen failed: ") +
                             socket_error_message(static_cast<int>(GetLastError())));
        return;
      }
      const int id = register_ipcsock_pipe({handle, wide_path, true});
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", id);
      set_string(ctx, out, "path", path);
      info.GetReturnValue().Set(out);
    }

    void ipcsock_accept(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int listener_id = int_arg(ctx, info[0], 0);
      std::lock_guard<std::mutex> lock(ipcsock_registry_mutex());
      auto it = ipcsock_registry().find(listener_id);
      if (it == ipcsock_registry().end() || !it->second.listener) {
        info.GetReturnValue().Set(make_socket_error(iso, ctx, ERROR_INVALID_HANDLE));
        return;
      }
      BOOL ok = ConnectNamedPipe(it->second.handle, nullptr);
      DWORD err = ok ? ERROR_PIPE_CONNECTED : GetLastError();
      if (!ok && err == ERROR_PIPE_LISTENING) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      if (!ok && err != ERROR_PIPE_CONNECTED) {
        info.GetReturnValue().Set(make_socket_error(iso, ctx, static_cast<int>(err)));
        return;
      }
      HANDLE client = it->second.handle;
      HANDLE next = create_ipcsock_listener(it->second.path);
      if (next == INVALID_HANDLE_VALUE) {
        info.GetReturnValue().Set(make_socket_error(iso, ctx, static_cast<int>(GetLastError())));
        return;
      }
      it->second.handle = next;
      const int client_id = next_ipcsock_id();
      ipcsock_registry()[client_id] = {client, it->second.path, false};
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", client_id);
      info.GetReturnValue().Set(out);
    }

    void ipcsock_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string path = info.Length() > 0 ? string_arg(iso, info[0]) : "";
      if (!guard_ipc_path(iso, path))
        return;
      const std::wstring wide_path = widen_utf8(path);
      HANDLE handle = CreateFileW(wide_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        throw_error(iso, std::string("ipcsock connect failed: ") +
                             socket_error_message(static_cast<int>(GetLastError())));
        return;
      }
      set_pipe_nowait(handle);
      const int id = register_ipcsock_pipe({handle, wide_path, false});
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", id);
      set_bool(ctx, out, "connected", true);
      info.GetReturnValue().Set(out);
    }

    void ipcsock_finish_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto out = Object::New(iso);
      set_bool(iso->GetCurrentContext(), out, "connected", true);
      info.GetReturnValue().Set(out);
    }

    void ipcsock_read(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::array<std::uint8_t, 65536> buf{};
      HANDLE handle = ipcsock_handle_arg(ctx, info);
      if (handle == INVALID_HANDLE_VALUE) {
        info.GetReturnValue().Set(make_socket_error(iso, ctx, ERROR_INVALID_HANDLE));
        return;
      }
      DWORD read = 0;
      if (!ReadFile(handle, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_NO_DATA) {
          info.GetReturnValue().Set(Null(iso));
          return;
        }
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
          auto out = Object::New(iso);
          set_bool(ctx, out, "eof", true);
          info.GetReturnValue().Set(out);
          return;
        }
        info.GetReturnValue().Set(make_socket_error(iso, ctx, static_cast<int>(err)));
        return;
      }
      auto out = Object::New(iso);
      if (read == 0) {
        set_bool(ctx, out, "eof", true);
        info.GetReturnValue().Set(out);
        return;
      }
      auto backing = ArrayBuffer::NewBackingStore(iso, static_cast<std::size_t>(read));
      std::memcpy(backing->Data(), buf.data(), static_cast<std::size_t>(read));
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      (void)out->Set(ctx, str(iso, "data"),
                     Uint8Array::New(buffer, 0, static_cast<std::size_t>(read)));
      info.GetReturnValue().Set(out);
    }

    void ipcsock_write(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_view bytes{};
      if (info.Length() < 2 || !value_to_bytes(iso, ctx, info[1], bytes)) {
        throw_error(iso, "ipcsock.write requires fd and string/bytes");
        return;
      }
      HANDLE handle = ipcsock_handle_arg(ctx, info);
      if (handle == INVALID_HANDLE_VALUE) {
        throw_error(iso, "ipcsock write failed: invalid handle");
        return;
      }
      std::size_t written = 0;
      while (written < bytes.size) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size - written, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD n = 0;
        if (!WriteFile(handle, bytes.data + written, chunk, &n, nullptr)) {
          const DWORD err = GetLastError();
          if (err == ERROR_NO_DATA)
            break;
          throw_error(iso, std::string("ipcsock write failed: ") +
                               socket_error_message(static_cast<int>(err)));
          return;
        }
        written += static_cast<std::size_t>(n);
      }
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(written)));
    }

    void ipcsock_shutdown(const FunctionCallbackInfo<Value>& info) {
      if (auto handle = ipcsock_handle_arg(info.GetIsolate()->GetCurrentContext(), info);
          handle != INVALID_HANDLE_VALUE)
        (void)FlushFileBuffers(handle);
    }

    void ipcsock_close(const FunctionCallbackInfo<Value>& info) {
      auto ctx = info.GetIsolate()->GetCurrentContext();
      const int id = int_arg(ctx, info[0], 0);
      ipcsock_pipe_state state{};
      {
        std::lock_guard<std::mutex> lock(ipcsock_registry_mutex());
        auto it = ipcsock_registry().find(id);
        if (it == ipcsock_registry().end())
          return;
        state = std::move(it->second);
        ipcsock_registry().erase(it);
      }
      if (state.handle != INVALID_HANDLE_VALUE)
        CloseHandle(state.handle);
    }

    void ipcsock_address(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Null(info.GetIsolate()));
    }
#else
    std::mutex& ipcsock_listener_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<socket_handle, std::string>& ipcsock_listener_paths() {
      static std::unordered_map<socket_handle, std::string> paths;
      return paths;
    }

    bool make_unix_sockaddr(Isolate* iso, std::string_view path, sockaddr_un& addr,
                            socklen_t& addr_len) {
      if (path.empty()) {
        throw_error(iso, "ipcsock path must not be empty");
        return false;
      }
      if (path.size() >= sizeof(addr.sun_path)) {
        throw_error(iso, "ipcsock path is too long");
        return false;
      }
      std::memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, path.data(), path.size());
      addr.sun_path[path.size()] = '\0';
      addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
#if defined(__APPLE__)
      addr.sun_len = static_cast<unsigned char>(addr_len);
#endif
      return true;
    }

    void ipcsock_listen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string path = info.Length() > 0 ? string_arg(iso, info[0]) : "";
      if (!guard_ipc_path(iso, path))
        return;
      sockaddr_un addr{};
      socklen_t addr_len = 0;
      if (!make_unix_sockaddr(iso, path, addr, addr_len))
        return;
      const socket_handle fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd == invalid_socket_handle) {
        throw_error(iso, std::string("socket(AF_UNIX) failed: ") +
                             socket_error_message(last_socket_error()));
        return;
      }
      if (!set_nonblocking_socket(fd) ||
          bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0 || listen(fd, 32) != 0) {
        const int err = last_socket_error();
        close_socket_handle(fd);
        throw_error(iso, std::string("ipcsock listen failed: ") + socket_error_message(err));
        return;
      }
      {
        std::lock_guard<std::mutex> lock(ipcsock_listener_mutex());
        ipcsock_listener_paths()[fd] = path;
      }
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", static_cast<double>(fd));
      set_string(ctx, out, "path", path);
      info.GetReturnValue().Set(out);
    }

    void ipcsock_accept(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      sockaddr_un addr{};
      socklen_t len = sizeof(addr);
      const socket_handle client =
          accept(socket_arg(ctx, info), reinterpret_cast<sockaddr*>(&addr), &len);
      if (client == invalid_socket_handle) {
        const int err = last_socket_error();
        if (would_block_error(err)) {
          info.GetReturnValue().Set(Null(iso));
          return;
        }
        info.GetReturnValue().Set(make_socket_error(iso, ctx, err));
        return;
      }
      (void)set_nonblocking_socket(client);
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", static_cast<double>(client));
      info.GetReturnValue().Set(out);
    }

    void ipcsock_connect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string path = info.Length() > 0 ? string_arg(iso, info[0]) : "";
      if (!guard_ipc_path(iso, path))
        return;
      sockaddr_un addr{};
      socklen_t addr_len = 0;
      if (!make_unix_sockaddr(iso, path, addr, addr_len))
        return;
      const socket_handle fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd == invalid_socket_handle) {
        throw_error(iso, std::string("socket(AF_UNIX) failed: ") +
                             socket_error_message(last_socket_error()));
        return;
      }
      if (!set_nonblocking_socket(fd)) {
        const int err = last_socket_error();
        close_socket_handle(fd);
        throw_error(iso,
                    std::string("nonblocking ipcsock setup failed: ") + socket_error_message(err));
        return;
      }
      const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), addr_len);
      const int err = rc == 0 ? 0 : last_socket_error();
      if (rc != 0 && !would_block_error(err)) {
        close_socket_handle(fd);
        throw_error(iso, std::string("ipcsock connect failed: ") + socket_error_message(err));
        return;
      }
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", static_cast<double>(fd));
      set_bool(ctx, out, "connected", rc == 0);
      info.GetReturnValue().Set(out);
    }

    void ipcsock_finish_connect(const FunctionCallbackInfo<Value>& info) {
      tcp_finish_connect(info);
    }

    void ipcsock_read(const FunctionCallbackInfo<Value>& info) {
      tcp_read(info);
    }

    void ipcsock_write(const FunctionCallbackInfo<Value>& info) {
      tcp_write(info);
    }

    void ipcsock_shutdown(const FunctionCallbackInfo<Value>& info) {
      tcp_shutdown(info);
    }

    void ipcsock_close(const FunctionCallbackInfo<Value>& info) {
      const auto fd = socket_arg(info.GetIsolate()->GetCurrentContext(), info);
      std::string path;
      {
        std::lock_guard<std::mutex> lock(ipcsock_listener_mutex());
        auto it = ipcsock_listener_paths().find(fd);
        if (it != ipcsock_listener_paths().end()) {
          path = std::move(it->second);
          ipcsock_listener_paths().erase(it);
        }
      }
      close_socket_handle(fd);
      if (!path.empty())
        (void)unlink(path.c_str());
    }

    void ipcsock_address(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Null(info.GetIsolate()));
    }
#endif

    void ipcsock_recv(const FunctionCallbackInfo<Value>& info) {
      ipcsock_read(info);
    }

    void ipcsock_send(const FunctionCallbackInfo<Value>& info) {
      ipcsock_write(info);
    }

    void udp_bind(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const std::string host = info.Length() > 0 ? string_arg(iso, info[0]) : "127.0.0.1";
      const int port = info.Length() > 1 ? int_arg(ctx, info[1]) : 0;
      const int family = info.Length() > 2 ? socket_family_arg(iso, info[2]) : AF_UNSPEC;
      if (!guard_net_socket(iso, host, port))
        return;
      sockaddr_storage addr{};
      socklen_t addr_len = 0;
      if (!resolve_udp_addr(iso, host, port, true, family, addr, addr_len))
        return;
      const socket_handle fd = socket(addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
      if (fd == invalid_socket_handle) {
        throw_error(iso, std::string("socket(SOCK_DGRAM) failed: ") +
                             socket_error_message(last_socket_error()));
        return;
      }
      if (!set_nonblocking_socket(fd) ||
          bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
        const int err = last_socket_error();
        close_socket_handle(fd);
        throw_error(iso, std::string("udp bind failed: ") + socket_error_message(err));
        return;
      }
      addr_len = sizeof(addr);
      (void)getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
      auto out = make_sockaddr_object(iso, ctx, reinterpret_cast<sockaddr*>(&addr), addr_len);
      set_number(ctx, out, "fd", static_cast<double>(fd));
      info.GetReturnValue().Set(out);
    }

    void udp_recv(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::array<std::uint8_t, 65536> buf{};
      sockaddr_storage addr{};
      socklen_t len = sizeof(addr);
      const auto fd = socket_arg(ctx, info);
#if defined(_WIN32)
      const int n = recvfrom(fd, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()),
                             0, reinterpret_cast<sockaddr*>(&addr), &len);
#else
      const ssize_t n =
          recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&addr), &len);
#endif
      if (n < 0) {
        const int err = last_socket_error();
        if (would_block_error(err)) {
          info.GetReturnValue().Set(Null(iso));
          return;
        }
        info.GetReturnValue().Set(make_socket_error(iso, ctx, err));
        return;
      }
      auto out = make_sockaddr_object(iso, ctx, reinterpret_cast<sockaddr*>(&addr), len);
      auto backing = ArrayBuffer::NewBackingStore(iso, static_cast<std::size_t>(n));
      std::memcpy(backing->Data(), buf.data(), static_cast<std::size_t>(n));
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      (void)out->Set(ctx, str(iso, "data"),
                     Uint8Array::New(buffer, 0, static_cast<std::size_t>(n)));
      info.GetReturnValue().Set(out);
    }

    void udp_send(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_view bytes{};
      if (info.Length() < 4 || !value_to_bytes(iso, ctx, info[1], bytes)) {
        throw_error(iso, "udp.send requires fd, bytes, host, port");
        return;
      }
      const std::string host = string_arg(iso, info[2]);
      const int port = int_arg(ctx, info[3]);
      if (!guard_net_socket(iso, host, port))
        return;
      sockaddr_storage addr{};
      socklen_t addr_len = 0;
      const int family = info.Length() > 4 ? socket_family_arg(iso, info[4]) : AF_UNSPEC;
      if (!resolve_udp_addr(iso, host, port, false, family, addr, addr_len))
        return;
      const auto fd = socket_arg(ctx, info);
#if defined(_WIN32)
      const int n =
          sendto(fd, reinterpret_cast<const char*>(bytes.data),
                 static_cast<int>(std::min<std::size_t>(
                     bytes.size, static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                 0, reinterpret_cast<sockaddr*>(&addr), addr_len);
#else
      const ssize_t n =
          sendto(fd, bytes.data, bytes.size, 0, reinterpret_cast<sockaddr*>(&addr), addr_len);
#endif
      if (n < 0) {
        throw_error(iso,
                    std::string("udp send failed: ") + socket_error_message(last_socket_error()));
        return;
      }
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(n)));
    }

    struct array_buffer_transfer {
      std::uint32_t id = 0;
      std::shared_ptr<BackingStore> backing;
    };

    struct serialized_worker_payload {
      std::vector<std::uint8_t> bytes;
      std::vector<array_buffer_transfer> transfers;
    };

    struct worker_event {
      std::string type;
      serialized_worker_payload data;
      bool has_data = false;
      std::string message;
      int exit_code = 0;
    };

    struct worker_handle {
      int id = 0;
      std::string path;
      std::string worker_data_json = "null";
      std::mutex mutex;
      std::condition_variable cv;
      std::deque<worker_event> parent_events;
      std::deque<worker_event> worker_events;
      std::thread thread;
      v8::Isolate* isolate = nullptr;
      bool done = false;
    };

    std::mutex& worker_registry_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<int, std::shared_ptr<worker_handle>>& worker_registry() {
      static std::unordered_map<int, std::shared_ptr<worker_handle>> registry;
      return registry;
    }

    int next_worker_id() {
      static std::atomic<int> id{1};
      return id.fetch_add(1);
    }

    std::shared_ptr<worker_handle> find_worker(int id) {
      std::lock_guard<std::mutex> lock(worker_registry_mutex());
      auto it = worker_registry().find(id);
      return it == worker_registry().end() ? nullptr : it->second;
    }

    bool serialize_transfer_list([[maybe_unused]] Isolate* iso, Local<Context> ctx,
                                 Local<Value> transfer_list, ValueSerializer& serializer,
                                 std::vector<array_buffer_transfer>& transfers,
                                 std::string& error) {
      if (transfer_list.IsEmpty() || transfer_list->IsUndefined() || transfer_list->IsNull())
        return true;
      if (!transfer_list->IsArray()) {
        error = "postMessage transferList must be an array";
        return false;
      }
      auto array = transfer_list.As<Array>();
      const std::uint32_t length = array->Length();
      transfers.reserve(length);
      for (std::uint32_t i = 0; i < length; ++i) {
        Local<Value> item;
        if (!array->Get(ctx, i).ToLocal(&item)) {
          error = "postMessage transferList lookup failed";
          return false;
        }
        Local<ArrayBuffer> buffer;
        if (item->IsArrayBuffer()) {
          buffer = item.As<ArrayBuffer>();
        } else if (item->IsArrayBufferView()) {
          buffer = item.As<ArrayBufferView>()->Buffer();
        } else {
          error = "postMessage transferList entries must be ArrayBuffer or typed array values";
          return false;
        }
        const auto transfer_id = static_cast<std::uint32_t>(transfers.size() + 1);
        serializer.TransferArrayBuffer(transfer_id, buffer);
        transfers.push_back(array_buffer_transfer{transfer_id, buffer->GetBackingStore()});
      }
      return true;
    }

    bool serialize_worker_value(Isolate* iso, Local<Context> ctx, Local<Value> value,
                                Local<Value> transfer_list, serialized_worker_payload& out,
                                std::string& error) {
      ValueSerializer serializer(iso);
      if (!serialize_transfer_list(iso, ctx, transfer_list, serializer, out.transfers, error))
        return false;
      serializer.WriteHeader();
      if (!serializer.WriteValue(ctx, value).FromMaybe(false)) {
        error = "postMessage value could not be structured-cloned";
        return false;
      }
      auto released = serializer.Release();
      out.bytes.assign(released.first, released.first + released.second);
      std::free(released.first);
      return true;
    }

    MaybeLocal<Value> deserialize_worker_value(Isolate* iso, Local<Context> ctx,
                                               const serialized_worker_payload& payload) {
      ValueDeserializer deserializer(iso, payload.bytes.data(), payload.bytes.size());
      for (const auto& transfer : payload.transfers) {
        if (transfer.backing)
          deserializer.TransferArrayBuffer(transfer.id, ArrayBuffer::New(iso, transfer.backing));
      }
      if (!deserializer.ReadHeader(ctx).FromMaybe(false))
        return MaybeLocal<Value>();
      return deserializer.ReadValue(ctx);
    }

    Local<Value> optional_arg_or_undefined(Isolate* iso, const FunctionCallbackInfo<Value>& info,
                                           int index) {
      return info.Length() > index ? info[index] : Local<Value>(Undefined(iso));
    }

    bool make_message_event(Isolate* iso, Local<Context> ctx, Local<Value> value,
                            Local<Value> transfer_list, worker_event& event) {
      std::string error;
      serialized_worker_payload payload;
      if (!serialize_worker_value(iso, ctx, value, transfer_list, payload, error)) {
        throw_error(iso, error);
        return false;
      }
      event.type = "message";
      event.data = std::move(payload);
      event.has_data = true;
      return true;
    }

    void enqueue_parent_event(const std::shared_ptr<worker_handle>& worker, worker_event event) {
      if (!worker)
        return;
      std::lock_guard<std::mutex> lock(worker->mutex);
      worker->parent_events.push_back(std::move(event));
      worker->cv.notify_all();
    }

    void enqueue_worker_event(worker_handle* worker, worker_event event) {
      if (!worker)
        return;
      std::lock_guard<std::mutex> lock(worker->mutex);
      worker->worker_events.push_back(std::move(event));
      worker->cv.notify_all();
    }

    void run_worker_thread(std::shared_ptr<worker_handle> worker) {
      int exit_code = 0;
      worker_bootstrap bootstrap{worker->id, worker->worker_data_json.c_str(), worker.get()};
      worker_bootstrap_scope scope(bootstrap);
      try {
        fxe::js::host host(fxe::js::host::bootstrap_mode::worker_thread);
        auto result = host.run_file(worker->path);
        if (!result.ok) {
          exit_code = 1;
          enqueue_parent_event(worker, worker_event{"error", {}, false, result.message, 0});
        }
      } catch (const std::exception& e) {
        exit_code = 1;
        enqueue_parent_event(worker, worker_event{"error", {}, false, e.what(), 0});
      } catch (...) {
        exit_code = 1;
        enqueue_parent_event(
            worker,
            worker_event{"error", {}, false, "worker terminated with unknown native exception", 0});
      }
      {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->done = true;
        worker->isolate = nullptr;
        worker->parent_events.push_back(worker_event{"exit", {}, false, {}, exit_code});
        worker->cv.notify_all();
      }
    }

    void worker_start(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (current_worker_bootstrap() != nullptr) {
        throw_error(iso, "Worker construction is only available on the main thread");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_error(iso, "__fxe_native.worker.start requires a worker file path");
        return;
      }
      auto worker = std::make_shared<worker_handle>();
      worker->id = next_worker_id();
      worker->path = string_arg(iso, info[0]);
      if (info.Length() > 1 && info[1]->IsString())
        worker->worker_data_json = string_arg(iso, info[1]);
      {
        std::lock_guard<std::mutex> lock(worker_registry_mutex());
        worker_registry()[worker->id] = worker;
      }
      try {
        worker->thread = std::thread(run_worker_thread, worker);
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(worker_registry_mutex());
        worker_registry().erase(worker->id);
        throw_error(iso, std::string("worker thread start failed: ") + e.what());
        return;
      }
      info.GetReturnValue().Set(Integer::New(iso, worker->id));
    }

    void worker_post_message(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* bootstrap = current_worker_bootstrap();
      if (bootstrap != nullptr) {
        if (info.Length() < 1) {
          throw_error(iso, "__fxe_native.worker.postMessage requires a message");
          return;
        }
        auto* worker = static_cast<worker_handle*>(bootstrap->native_handle);
        worker_event event;
        if (!make_message_event(iso, ctx, info[0], optional_arg_or_undefined(iso, info, 1), event))
          return;
        enqueue_parent_event(worker ? find_worker(worker->id) : nullptr, std::move(event));
        info.GetReturnValue().Set(True(iso));
        return;
      }
      if (info.Length() < 2) {
        throw_error(iso, "__fxe_native.worker.postMessage requires a handle and message");
        return;
      }
      auto worker = find_worker(int_arg(ctx, info[0], 0));
      if (!worker) {
        throw_error(iso, "worker handle not found");
        return;
      }
      worker_event event;
      if (!make_message_event(iso, ctx, info[1], optional_arg_or_undefined(iso, info, 2), event))
        return;
      enqueue_worker_event(worker.get(), std::move(event));
      info.GetReturnValue().Set(True(iso));
    }

    struct message_port_channel {
      std::mutex mutex;
      std::deque<worker_event> queues[2];
      std::condition_variable cv[2];
      bool closed[2] = {false, false};
    };

    struct message_port_state {
      std::shared_ptr<message_port_channel> channel;
      int side = 0;
      bool closed = false;
      Global<Object> self;
    };

    message_port_state* get_message_port_state(const FunctionCallbackInfo<Value>& info) {
      return static_cast<message_port_state*>(
          info.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
    }

    void message_port_finalizer(const WeakCallbackInfo<message_port_state>& data) {
      auto* state = data.GetParameter();
      state->self.Reset();
      delete state;
    }

    void message_port_post_message(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* state = get_message_port_state(info);
      if (!state || state->closed || !state->channel || info.Length() < 1)
        return;
      worker_event event;
      if (!make_message_event(iso, ctx, info[0], optional_arg_or_undefined(iso, info, 1), event))
        return;
      const int peer = state->side == 0 ? 1 : 0;
      std::lock_guard<std::mutex> lock(state->channel->mutex);
      if (state->channel->closed[state->side] || state->channel->closed[peer])
        return;
      state->channel->queues[peer].push_back(std::move(event));
      state->channel->cv[peer].notify_all();
      info.GetReturnValue().Set(True(iso));
    }

    MaybeLocal<Object> worker_event_to_object(Isolate* iso, Local<Context> ctx,
                                              const worker_event& event) {
      auto out = Object::New(iso);
      set_string(ctx, out, "type", event.type);
      if (event.has_data) {
        Local<Value> data;
        if (!deserialize_worker_value(iso, ctx, event.data).ToLocal(&data))
          return MaybeLocal<Object>();
        (void)out->Set(ctx, str(iso, "data"), data);
      } else if (event.type == "close") {
        (void)out->Set(ctx, str(iso, "data"), Null(iso));
      }
      if (!event.message.empty())
        set_string(ctx, out, "message", event.message);
      if (event.type == "exit")
        set_number(ctx, out, "exitCode", event.exit_code);
      return out;
    }

    Local<Array> worker_events_to_array(Isolate* iso, Local<Context> ctx,
                                        const std::deque<worker_event>& events) {
      auto arr = Array::New(iso, static_cast<int>(events.size()));
      for (std::uint32_t i = 0; i < events.size(); ++i) {
        Local<Object> object;
        if (!worker_event_to_object(iso, ctx, events[i]).ToLocal(&object))
          return Array::New(iso, 0);
        (void)arr->Set(ctx, i, object);
      }
      return arr;
    }

    void message_port_drain_messages(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* state = get_message_port_state(info);
      std::deque<worker_event> events;
      if (state && state->channel) {
        std::lock_guard<std::mutex> lock(state->channel->mutex);
        events.swap(state->channel->queues[state->side]);
      }
      info.GetReturnValue().Set(worker_events_to_array(iso, ctx, events));
    }

    void message_port_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* state = get_message_port_state(info);
      if (!state || state->closed || !state->channel) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      state->closed = true;
      const int peer = state->side == 0 ? 1 : 0;
      {
        std::lock_guard<std::mutex> lock(state->channel->mutex);
        if (!state->channel->closed[state->side]) {
          state->channel->closed[state->side] = true;
          if (!state->channel->closed[peer]) {
            state->channel->queues[peer].push_back(worker_event{"close", {}, false, {}, 0});
            state->channel->cv[peer].notify_all();
          }
        }
      }
      info.GetReturnValue().Set(True(iso));
    }

    Local<Object> make_native_message_port(Isolate* iso, Local<Context> ctx,
                                           std::shared_ptr<message_port_channel> channel,
                                           int side) {
      auto state = std::make_unique<message_port_state>();
      state->channel = std::move(channel);
      state->side = side;
      auto out = Object::New(iso);
      auto external = External::New(iso, state.get(), v8::kExternalPointerTypeTagDefault);
      (void)out->Set(ctx, str(iso, "postMessage"),
                     Function::New(ctx, message_port_post_message, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "drainMessages"),
                     Function::New(ctx, message_port_drain_messages, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "close"),
                     Function::New(ctx, message_port_close, external).ToLocalChecked());
      (void)out->Set(ctx, str(iso, "start"),
                     Function::New(ctx, [](const FunctionCallbackInfo<Value>& info) {
                       info.GetReturnValue().Set(True(info.GetIsolate()));
                     }).ToLocalChecked());
      state->self.Reset(iso, out);
      state->self.SetWeak(state.get(), message_port_finalizer, WeakCallbackType::kParameter);
      (void)state.release();
      return out;
    }

    void worker_create_message_channel(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto channel = std::make_shared<message_port_channel>();
      auto out = Object::New(iso);
      (void)out->Set(ctx, str(iso, "port1"), make_native_message_port(iso, ctx, channel, 0));
      (void)out->Set(ctx, str(iso, "port2"),
                     make_native_message_port(iso, ctx, std::move(channel), 1));
      info.GetReturnValue().Set(out);
    }

    void worker_drain_messages(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      std::deque<worker_event> events;
      if (auto* bootstrap = current_worker_bootstrap()) {
        auto* worker = static_cast<worker_handle*>(bootstrap->native_handle);
        if (worker) {
          std::lock_guard<std::mutex> lock(worker->mutex);
          events.swap(worker->worker_events);
        }
      } else {
        if (info.Length() < 1) {
          throw_error(iso, "__fxe_native.worker.drainMessages requires a worker handle");
          return;
        }
        auto worker = find_worker(int_arg(ctx, info[0], 0));
        if (!worker) {
          info.GetReturnValue().Set(Array::New(iso, 0));
          return;
        }
        std::lock_guard<std::mutex> lock(worker->mutex);
        events.swap(worker->parent_events);
      }
      info.GetReturnValue().Set(worker_events_to_array(iso, ctx, events));
    }

    void worker_terminate(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (current_worker_bootstrap() != nullptr) {
        throw_error(iso, "cannot terminate the current worker from inside itself");
        return;
      }
      if (info.Length() < 1) {
        throw_error(iso, "__fxe_native.worker.terminate requires a worker handle");
        return;
      }
      const int id = int_arg(ctx, info[0], 0);
      auto worker = find_worker(id);
      if (!worker) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (worker->isolate)
          worker->isolate->TerminateExecution();
      }
      if (worker->thread.joinable())
        worker->thread.join();
      {
        std::lock_guard<std::mutex> lock(worker_registry_mutex());
        worker_registry().erase(id);
      }
      info.GetReturnValue().Set(True(iso));
    }

    void not_implemented(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* data = static_cast<const char*>(
          info.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
      auto full_name = std::string_view(data);
      std::string msg;
      msg.reserve(full_name.size() + sizeof(" is not implemented"));
      msg.append(full_name);
      msg.append(" is not implemented");
      iso->ThrowException(Exception::Error(str(iso, msg)));
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> ns, const char* name,
                      FunctionCallback callback) {
      auto fn = Function::New(ctx, callback).ToLocalChecked();
      (void)ns->Set(ctx, str(iso, name), fn);
    }

    void add_placeholder(Isolate* iso, Local<Context> ctx, Local<Object> ns, const char* name,
                         const char* full_name, const char* reason = nullptr) {
      auto data =
          External::New(iso, const_cast<char*>(full_name), v8::kExternalPointerTypeTagDefault);
      auto fn = Function::New(ctx, not_implemented, data).ToLocalChecked();
      (void)ns->Set(ctx, str(iso, name), fn);
      (void)fn->Set(ctx, str(iso, "notImplemented"), Boolean::New(iso, true));
      if (reason != nullptr) {
        (void)fn->Set(ctx, str(iso, "reason"), str(iso, reason));
      }
    }

    Local<Object> make_placeholder_namespace(Isolate* iso, Local<Context> ctx,
                                             const char* placeholder_name, const char* full_name) {
      auto ns = Object::New(iso);
      add_placeholder(iso, ctx, ns, placeholder_name, full_name);
      return ns;
    }

    Local<Object> make_os_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "platform", os_platform);
      add_function(iso, ctx, ns, "arch", os_arch);
      add_function(iso, ctx, ns, "release", os_release_fn);
      add_function(iso, ctx, ns, "type", os_type);
      add_function(iso, ctx, ns, "endianness", os_endianness);
      add_function(iso, ctx, ns, "homedir", os_homedir);
      add_function(iso, ctx, ns, "tmpdir", os_tmpdir);
      add_function(iso, ctx, ns, "hostname", os_hostname);
      add_function(iso, ctx, ns, "uptime", os_uptime_fn);
      add_function(iso, ctx, ns, "totalmem", os_totalmem);
      add_function(iso, ctx, ns, "freemem", os_freemem);
      add_function(iso, ctx, ns, "cpus", os_cpus);
      add_function(iso, ctx, ns, "networkInterfaces", os_network_interfaces);
      add_function(iso, ctx, ns, "userInfo", os_user_info);
      return ns;
    }

    Local<Object> make_tty_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "isatty", tty_isatty);
      add_function(iso, ctx, ns, "getWindowSize", tty_get_window_size);
      return ns;
    }

    Local<Object> make_random_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "fill", random_fill);
      return ns;
    }

    Local<Object> make_hash_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "create", hash_create);
      add_function(iso, ctx, ns, "createHmac", hmac_create);
      add_function(iso, ctx, ns, "pbkdf2Sync", pbkdf2_sync);
      return ns;
    }

    Local<Object> make_cipher_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "createCipheriv", cipher_create_cipheriv);
      add_function(iso, ctx, ns, "createDecipheriv", cipher_create_decipheriv);
      return ns;
    }

    Local<Object> make_kdf_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "scryptSync", kdf_scrypt_sync);
      return ns;
    }

    Local<Object> make_pk_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "parsePublicKeyDer", pk_parse_public_key_der);
      add_function(iso, ctx, ns, "parsePrivateKeyDer", pk_parse_private_key_der);
      add_function(iso, ctx, ns, "writePublicKeyDer", pk_write_public_key_der);
      add_function(iso, ctx, ns, "writePrivateKeyDer", pk_write_private_key_der);
      add_function(iso, ctx, ns, "rsaOaepEncrypt", pk_rsa_oaep_encrypt);
      add_function(iso, ctx, ns, "rsaOaepDecrypt", pk_rsa_oaep_decrypt);
      add_function(iso, ctx, ns, "ecdsaSign", pk_ecdsa_sign);
      add_function(iso, ctx, ns, "ecdsaVerify", pk_ecdsa_verify);
      add_function(iso, ctx, ns, "ecdsaGenerate", pk_ecdsa_generate);
      add_function(iso, ctx, ns, "timingSafeEqual", pk_timing_safe_equal);
      return ns;
    }

    Local<Object> make_spawn_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "spawn", spawn_spawn);
      return ns;
    }

    Local<Object> make_worker_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      auto* bootstrap = current_worker_bootstrap();
      const bool is_worker = bootstrap != nullptr;
      add_function(iso, ctx, ns, "start", worker_start);
      add_function(iso, ctx, ns, "createWorker", worker_start);
      add_function(iso, ctx, ns, "spawn", worker_start);
      add_function(iso, ctx, ns, "postMessage", worker_post_message);
      add_function(iso, ctx, ns, "drainMessages", worker_drain_messages);
      add_function(iso, ctx, ns, "createMessageChannel", worker_create_message_channel);
      add_function(iso, ctx, ns, "terminate", worker_terminate);
      add_function(iso, ctx, ns, "ref", [](const FunctionCallbackInfo<Value>& info) {
        info.GetReturnValue().Set(True(info.GetIsolate()));
      });
      add_function(iso, ctx, ns, "unref", [](const FunctionCallbackInfo<Value>& info) {
        info.GetReturnValue().Set(True(info.GetIsolate()));
      });
      set_bool(ctx, ns, "available", true);
      set_bool(ctx, ns, "notImplemented", false);
      set_bool(ctx, ns, "isMainThread", !is_worker);
      set_number(ctx, ns, "threadId", is_worker ? bootstrap->thread_id : 0);
      if (is_worker) {
        set_string(ctx, ns, "workerDataJson", bootstrap->worker_data_json);
        if (auto* worker = static_cast<worker_handle*>(bootstrap->native_handle)) {
          std::lock_guard<std::mutex> lock(worker->mutex);
          worker->isolate = iso;
        }
      }
      return ns;
    }

    Local<Object> make_dns_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "lookup", dns_lookup);
      add_function(iso, ctx, ns, "lookupService", dns_lookup_service);
      add_function(iso, ctx, ns, "resolveRecord", dns_resolve_record);
      return ns;
    }

    Local<Object> make_net_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "listen", tcp_listen);
      add_function(iso, ctx, ns, "accept", tcp_accept);
      add_function(iso, ctx, ns, "connect", tcp_connect);
      add_function(iso, ctx, ns, "finishConnect", tcp_finish_connect);
      add_function(iso, ctx, ns, "read", tcp_read);
      add_function(iso, ctx, ns, "write", tcp_write);
      add_function(iso, ctx, ns, "shutdown", tcp_shutdown);
      add_function(iso, ctx, ns, "close", close_fd);
      add_function(iso, ctx, ns, "address", socket_address);
      return ns;
    }

    Local<Object> make_ipcsock_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "listen", ipcsock_listen);
      add_function(iso, ctx, ns, "accept", ipcsock_accept);
      add_function(iso, ctx, ns, "connect", ipcsock_connect);
      add_function(iso, ctx, ns, "finishConnect", ipcsock_finish_connect);
      add_function(iso, ctx, ns, "read", ipcsock_read);
      add_function(iso, ctx, ns, "recv", ipcsock_recv);
      add_function(iso, ctx, ns, "write", ipcsock_write);
      add_function(iso, ctx, ns, "send", ipcsock_send);
      add_function(iso, ctx, ns, "shutdown", ipcsock_shutdown);
      add_function(iso, ctx, ns, "close", ipcsock_close);
      add_function(iso, ctx, ns, "address", ipcsock_address);
      return ns;
    }

    Local<Object> make_dgram_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "bind", udp_bind);
      add_function(iso, ctx, ns, "recv", udp_recv);
      add_function(iso, ctx, ns, "send", udp_send);
      add_function(iso, ctx, ns, "close", close_fd);
      return ns;
    }
  } // namespace

  void install_fxe_native(Isolate* iso, Local<Context> ctx) {
    auto native = Object::New(iso);
    (void)native->Set(ctx, str(iso, "os"), make_os_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "tty"), make_tty_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "spawn"), make_spawn_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "random"), make_random_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "hash"), make_hash_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "cipher"), make_cipher_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "kdf"), make_kdf_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "pk"), make_pk_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "worker"), make_worker_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "dns"), make_dns_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "net"), make_net_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "ipcsock"), make_ipcsock_namespace(iso, ctx));
    (void)native->Set(ctx, str(iso, "dgram"), make_dgram_namespace(iso, ctx));
    install_fs_fd_native(iso, ctx, native);

    struct namespace_spec {
      const char* name;
      const char* placeholder_name;
      const char* full_name;
    };

    static constexpr namespace_spec namespaces[] = {
        {"fs", "readFile", "__fxe_native.fs.readFile"},
    };

    for (const auto& spec : namespaces) {
      (void)native->Set(
          ctx, str(iso, spec.name),
          make_placeholder_namespace(iso, ctx, spec.placeholder_name, spec.full_name));
    }

    (void)ctx->Global()->DefineOwnProperty(ctx, str(iso, "__fxe_native"), native,
                                           static_cast<PropertyAttribute>(DontEnum));
  }

} // namespace fxe::runtime
