#include <fxe/crash.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <string>

#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fxe::os::crash_detail {
  namespace {
    std::atomic_bool g_installed{false};
    std::atomic_bool g_handling{false};

    struct lite_header {
      char magic[8];
      std::uint32_t version;
      std::uint32_t signal_number;
      std::uint64_t pid;
      std::uint64_t crashing_tid;
      std::uint64_t fault_address;
    };

    struct record_header {
      std::uint32_t type;
      std::uint32_t size;
    };

    enum record_type : std::uint32_t {
      record_thread = 1,
      record_maps = 2,
      record_frame = 3,
      record_note = 4,
    };

    struct thread_record {
      std::int64_t tid;
      std::uint64_t pc;
      std::uint64_t sp;
      std::uint64_t fp;
      std::uint32_t flags;
    };

    struct frame_record {
      std::int64_t tid;
      std::uint64_t pc;
      std::uint64_t image_base;
      std::uint64_t symbol_address;
      char image[192];
      char symbol[192];
    };

    void write_exact(int fd, const void* data, std::size_t size) noexcept {
      const auto* bytes = static_cast<const char*>(data);
      while (size > 0) {
        ssize_t written = write(fd, bytes, size);
        if (written <= 0)
          return;
        bytes += written;
        size -= static_cast<std::size_t>(written);
      }
    }

    void write_record(int fd, record_type type, const void* data, std::uint32_t size) noexcept {
      record_header header{static_cast<std::uint32_t>(type), size};
      write_exact(fd, &header, sizeof(header));
      if (size > 0)
        write_exact(fd, data, size);
    }

    void copy_cstr(char* dst, std::size_t dst_size, const char* src) noexcept {
      if (!dst || dst_size == 0)
        return;
      if (!src) {
        dst[0] = '\0';
        return;
      }
      std::snprintf(dst, dst_size, "%s", src);
    }

    pid_t current_tid() noexcept {
      return static_cast<pid_t>(syscall(SYS_gettid));
    }

    void registers_from_context(void* raw_context, std::uint64_t& pc, std::uint64_t& sp,
                                std::uint64_t& fp) noexcept {
      pc = 0;
      sp = 0;
      fp = 0;
      auto* context = static_cast<ucontext_t*>(raw_context);
      if (!context)
        return;
#if defined(__x86_64__)
      pc = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_RIP]);
      sp = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_RSP]);
      fp = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
      pc = static_cast<std::uint64_t>(context->uc_mcontext.pc);
      sp = static_cast<std::uint64_t>(context->uc_mcontext.sp);
      fp = static_cast<std::uint64_t>(context->uc_mcontext.regs[29]);
#else
      (void)context;
#endif
    }

    void write_thread_records(int fd, pid_t crashing_tid, std::uint64_t pc, std::uint64_t sp,
                              std::uint64_t fp) noexcept {
      DIR* dir = opendir("/proc/self/task");
      if (!dir)
        return;
      while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.')
          continue;
        char* end = nullptr;
        long tid = std::strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || tid <= 0)
          continue;
        thread_record record{};
        record.tid = tid;
        if (tid == crashing_tid) {
          record.pc = pc;
          record.sp = sp;
          record.fp = fp;
          record.flags = 1;
        }
        write_record(fd, record_thread, &record, sizeof(record));
      }
      closedir(dir);
    }

    void write_maps_record(int fd) noexcept {
      int maps = open("/proc/self/maps", O_RDONLY);
      if (maps < 0)
        return;
      char buffer[16384];
      ssize_t n = read(maps, buffer, sizeof(buffer));
      if (n > 0)
        write_record(fd, record_maps, buffer, static_cast<std::uint32_t>(n));
      close(maps);
    }

    void write_frame_record(int fd, pid_t tid, void* address) noexcept {
      frame_record record{};
      record.tid = tid;
      record.pc = reinterpret_cast<std::uint64_t>(address);
      Dl_info info{};
      if (dladdr(address, &info) != 0) {
        record.image_base = reinterpret_cast<std::uint64_t>(info.dli_fbase);
        record.symbol_address = reinterpret_cast<std::uint64_t>(info.dli_saddr);
        copy_cstr(record.image, sizeof(record.image), info.dli_fname);
        copy_cstr(record.symbol, sizeof(record.symbol), info.dli_sname);
      }
      write_record(fd, record_frame, &record, sizeof(record));
    }

    void write_current_backtrace(int fd, pid_t tid, std::uint64_t pc) noexcept {
      if (pc != 0)
        write_frame_record(fd, tid, reinterpret_cast<void*>(pc));
      void* frames[64] = {};
      int count = backtrace(frames, 64);
      for (int i = 0; i < count; ++i)
        write_frame_record(fd, tid, frames[i]);
    }

    void write_linux_lite_dump(int fd, int signal_number, siginfo_t* info, void* context) noexcept {
      pid_t tid = current_tid();
      std::uint64_t pc = 0;
      std::uint64_t sp = 0;
      std::uint64_t fp = 0;
      registers_from_context(context, pc, sp, fp);

      lite_header header{{'F', 'X', 'E', 'L', 'M', 'D', 'P', '1'},
                         1,
                         static_cast<std::uint32_t>(signal_number),
                         static_cast<std::uint64_t>(getpid()),
                         static_cast<std::uint64_t>(tid),
                         reinterpret_cast<std::uint64_t>(info ? info->si_addr : nullptr)};
      write_exact(fd, &header, sizeof(header));
      const char note[] = "format=linux-minidump-lite; records are typed little-endian payloads";
      write_record(fd, record_note, note, sizeof(note));
      write_thread_records(fd, tid, pc, sp, fp);
      write_maps_record(fd);
      write_current_backtrace(fd, tid, pc);
    }

    void linux_signal_handler(int signal_number, siginfo_t* info, void* context) noexcept {
      bool expected = false;
      if (!g_handling.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        signal(signal_number, SIG_DFL);
        raise(signal_number);
        return;
      }

      char path[4096] = {0};
      if (signal_next_dump_path("dmp", path, sizeof(path))) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
          write_linux_lite_dump(fd, signal_number, info, context);
          (void)fsync(fd);
          (void)close(fd);
        }
      }

      if (std::getenv("FXE_CRASH_SELF_TEST_CHILD"))
        _exit(0);
      signal(signal_number, SIG_DFL);
      raise(signal_number);
    }

    bool install_one(int signal_number) {
      struct sigaction action{};
      action.sa_sigaction = linux_signal_handler;
      sigemptyset(&action.sa_mask);
      action.sa_flags = SA_SIGINFO | SA_RESETHAND;
      return sigaction(signal_number, &action, nullptr) == 0;
    }
  } // namespace

  bool platform_install_handlers(const crash_options&) {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      return true;

    struct rlimit core_limit{};
    core_limit.rlim_cur = RLIM_INFINITY;
    core_limit.rlim_max = RLIM_INFINITY;
    (void)setrlimit(RLIMIT_CORE, &core_limit);

    bool ok = true;
    ok = install_one(SIGSEGV) && ok;
    ok = install_one(SIGBUS) && ok;
    ok = install_one(SIGFPE) && ok;
    ok = install_one(SIGABRT) && ok;
    ok = install_one(SIGILL) && ok;
    ok = install_one(SIGTRAP) && ok;
    if (!ok)
      g_installed.store(false, std::memory_order_release);
    return ok;
  }

  bool platform_self_test() noexcept {
    pid_t child = fork();
    if (child < 0)
      return false;
    if (child == 0) {
      setenv("FXE_CRASH_SELF_TEST_CHILD", "1", 1);
      raise(SIGTRAP);
      _exit(2);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0)
      return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
} // namespace fxe::os::crash_detail
