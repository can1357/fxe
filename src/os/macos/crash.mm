#include <fxe/crash.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/exc.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" boolean_t exc_server(mach_msg_header_t* request, mach_msg_header_t* reply);

namespace {
  mach_port_t g_exception_port = MACH_PORT_NULL;
  std::atomic_bool g_installed{false};
  std::atomic_bool g_signal_handling{false};

  void destroy_exception_port() {
    if (g_exception_port == MACH_PORT_NULL)
      return;
    (void)mach_port_mod_refs(mach_task_self(), g_exception_port, MACH_PORT_RIGHT_SEND, -1);
    (void)mach_port_mod_refs(mach_task_self(), g_exception_port, MACH_PORT_RIGHT_RECEIVE, -1);
    g_exception_port = MACH_PORT_NULL;
  }

  void append_symbol(std::ostringstream& out, const char* prefix, uintptr_t address) {
    Dl_info info{};
    out << prefix << "=0x" << std::hex << address << std::dec;
    if (dladdr(reinterpret_cast<void*>(address), &info) != 0) {
      if (info.dli_fname)
        out << " image=" << info.dli_fname;
      if (info.dli_sname) {
        auto symbol_addr = reinterpret_cast<uintptr_t>(info.dli_saddr);
        out << " symbol=" << info.dli_sname << "+0x" << std::hex << (address - symbol_addr)
            << std::dec;
      }
    }
    out << "\n";
  }

  void append_image_list(std::ostringstream& out) {
    out << "images:\n";
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
      const mach_header* header = _dyld_get_image_header(i);
      const char* name = _dyld_get_image_name(i);
      intptr_t slide = _dyld_get_image_vmaddr_slide(i);
      out << "  [" << i << "] base=0x" << std::hex << reinterpret_cast<uintptr_t>(header)
          << " slide=0x" << slide << std::dec << " path=" << (name ? name : "") << "\n";
    }
  }

  void append_local_backtrace(std::ostringstream& out) {
    void* frames[64] = {};
    int count = backtrace(frames, 64);
    out << "handler-backtrace:\n";
    for (int i = 0; i < count; ++i)
      append_symbol(out, "  frame", reinterpret_cast<uintptr_t>(frames[i]));
  }

  void append_thread_state(std::ostringstream& out, thread_t thread) {
#if defined(__aarch64__)
    arm_thread_state64_t state{};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(thread, ARM_THREAD_STATE64,
                                        reinterpret_cast<thread_state_t>(&state), &count);
    if (kr == KERN_SUCCESS) {
      out << "pc=0x" << std::hex << state.__pc << "\n";
      out << "sp=0x" << state.__sp << "\n";
      out << "fp=0x" << state.__fp << std::dec << "\n";
      append_symbol(out, "crashing-frame", static_cast<uintptr_t>(state.__pc));

      struct frame_record {
        uint64_t fp;
        uint64_t lr;
      };
      out << "crashing-thread-backtrace:\n";
      append_symbol(out, "  frame", static_cast<uintptr_t>(state.__pc));
      uint64_t fp = state.__fp;
      for (int i = 0; i < 64 && fp != 0; ++i) {
        frame_record record{};
        vm_size_t copied = 0;
        kr = vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(fp), sizeof(record),
                               reinterpret_cast<vm_address_t>(&record), &copied);
        if (kr != KERN_SUCCESS || copied != sizeof(record) || record.lr == 0 || record.fp <= fp)
          break;
        append_symbol(out, "  frame", static_cast<uintptr_t>(record.lr));
        fp = record.fp;
      }

      vm_address_t sp = static_cast<vm_address_t>(state.__sp);
      char stack[4096] = {0};
      vm_size_t copied = 0;
      kr = vm_read_overwrite(mach_task_self(), sp, sizeof(stack), reinterpret_cast<vm_address_t>(stack),
                             &copied);
      if (kr == KERN_SUCCESS && copied > 0) {
        out << "stack-bytes=" << copied << "\n";
        out.write(stack, static_cast<std::streamsize>(copied));
        out << "\n";
      }
    }
#elif defined(__x86_64__)
    x86_thread_state64_t state{};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(thread, x86_THREAD_STATE64,
                                        reinterpret_cast<thread_state_t>(&state), &count);
    if (kr == KERN_SUCCESS) {
      out << "rip=0x" << std::hex << state.__rip << "\n";
      out << "rsp=0x" << state.__rsp << "\n";
      out << "rbp=0x" << state.__rbp << std::dec << "\n";
      append_symbol(out, "crashing-frame", static_cast<uintptr_t>(state.__rip));

      struct frame_record {
        uint64_t fp;
        uint64_t ret;
      };
      out << "crashing-thread-backtrace:\n";
      append_symbol(out, "  frame", static_cast<uintptr_t>(state.__rip));
      uint64_t fp = state.__rbp;
      for (int i = 0; i < 64 && fp != 0; ++i) {
        frame_record record{};
        vm_size_t copied = 0;
        kr = vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(fp), sizeof(record),
                               reinterpret_cast<vm_address_t>(&record), &copied);
        if (kr != KERN_SUCCESS || copied != sizeof(record) || record.ret == 0 || record.fp <= fp)
          break;
        append_symbol(out, "  frame", static_cast<uintptr_t>(record.ret));
        fp = record.fp;
      }

      vm_address_t sp = static_cast<vm_address_t>(state.__rsp);
      char stack[4096] = {0};
      vm_size_t copied = 0;
      kr = vm_read_overwrite(mach_task_self(), sp, sizeof(stack), reinterpret_cast<vm_address_t>(stack),
                             &copied);
      if (kr == KERN_SUCCESS && copied > 0) {
        out << "stack-bytes=" << copied << "\n";
        out.write(stack, static_cast<std::streamsize>(copied));
        out << "\n";
      }
    }
#else
    (void)thread;
#endif
  }

  void signal_handler(int signal_number, siginfo_t* info, void*) noexcept {
    bool expected = false;
    if (!g_signal_handling.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      signal(signal_number, SIG_DFL);
      raise(signal_number);
      return;
    }
    fxe::os::crash_detail::write_signal_dump(signal_number, info ? info->si_addr : nullptr, "dmp");
    if (std::getenv("FXE_CRASH_SELF_TEST_CHILD"))
      _exit(0);
    signal(signal_number, SIG_DFL);
    raise(signal_number);
  }

  bool install_signal(int signal_number) {
    struct sigaction action{};
    action.sa_sigaction = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    return sigaction(signal_number, &action, nullptr) == 0;
  }

  void mach_exception_thread() {
    pthread_setname_np("fxe-crash-handler");
    for (;;) {
      alignas(mach_msg_header_t) char request_buffer[4096] = {0};
      alignas(mach_msg_header_t) char reply_buffer[4096] = {0};
      auto* request = reinterpret_cast<mach_msg_header_t*>(request_buffer);
      auto* reply = reinterpret_cast<mach_msg_header_t*>(reply_buffer);
      kern_return_t kr = mach_msg(request, MACH_RCV_MSG, 0, sizeof(request_buffer),
                                  g_exception_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
      if (kr != KERN_SUCCESS)
        continue;
      if (exc_server(request, reply)) {
        (void)mach_msg(reply, MACH_SEND_MSG, reply->msgh_size, 0, MACH_PORT_NULL,
                       MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
      }
    }
  }
} // namespace

extern "C" kern_return_t catch_exception_raise(mach_port_t, mach_port_t thread, mach_port_t task,
                                               exception_type_t exception, exception_data_t code,
                                               mach_msg_type_number_t code_count) {
  std::ostringstream out;
  out << "fxe crash dump\nformat=macos-mach-snapshot-symbolicated\n";
  out << "exception=" << exception << "\n";
  out << "task=" << task << "\n";
  out << "thread=" << thread << "\n";
  for (mach_msg_type_number_t i = 0; i < code_count; ++i)
    out << "code[" << i << "]=" << code[i] << "\n";
  append_thread_state(out, thread);
  append_image_list(out);
  append_local_backtrace(out);
  (void)fxe::os::crash_detail::write_dump_text("dmp", out.str());
  return KERN_FAILURE;
}

extern "C" kern_return_t catch_exception_raise_state(mach_port_t, exception_type_t,
                                                     const exception_data_t, mach_msg_type_number_t,
                                                     int*, const thread_state_t,
                                                     mach_msg_type_number_t, thread_state_t,
                                                     mach_msg_type_number_t*) {
  return KERN_FAILURE;
}

extern "C" kern_return_t
catch_exception_raise_state_identity(mach_port_t, mach_port_t, mach_port_t, exception_type_t,
                                     exception_data_t, mach_msg_type_number_t, int*, thread_state_t,
                                     mach_msg_type_number_t, thread_state_t,
                                     mach_msg_type_number_t*) {
  return KERN_FAILURE;
}

namespace fxe::os::crash_detail {
  bool platform_install_handlers(const crash_options&) {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      return true;

    kern_return_t kr =
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_exception_port);
    if (kr != KERN_SUCCESS) {
      g_installed.store(false, std::memory_order_release);
      return false;
    }
    kr = mach_port_insert_right(mach_task_self(), g_exception_port, g_exception_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
      destroy_exception_port();
      g_installed.store(false, std::memory_order_release);
      return false;
    }

    exception_mask_t mask =
        EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_CRASH;
    kr = task_set_exception_ports(mach_task_self(), mask, g_exception_port, EXCEPTION_DEFAULT,
                                  THREAD_STATE_NONE);
    if (kr != KERN_SUCCESS) {
      destroy_exception_port();
      g_installed.store(false, std::memory_order_release);
      return false;
    }

    std::thread(mach_exception_thread).detach();

    bool signals_ok = true;
    signals_ok = install_signal(SIGSEGV) && signals_ok;
    signals_ok = install_signal(SIGBUS) && signals_ok;
    signals_ok = install_signal(SIGFPE) && signals_ok;
    signals_ok = install_signal(SIGABRT) && signals_ok;
    signals_ok = install_signal(SIGTRAP) && signals_ok;
    return signals_ok;
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
