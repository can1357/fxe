#include <fxe/crash.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dbghelp.h>
#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <fxe/types.hpp>
#include <string>
#include <vector>

namespace fxe::os::crash_detail {
  namespace {
    std::atomic_bool g_installed{false};
    std::atomic_bool g_wrote_dump{false};
    PVOID g_vectored_handler = nullptr;

    std::wstring to_wide(const std::string& value) {
      if (value.empty())
        return {};
      int size =
          MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (size <= 0)
        return {};
      std::wstring out(static_cast<usize>(size), L'\0');
      (void)MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                out.data(), size);
      return out;
    }

    void write_symbol_sidecar(const std::string& dump_path,
                              EXCEPTION_POINTERS* exception_pointers) {
      if (!exception_pointers || !exception_pointers->ContextRecord)
        return;
      HANDLE process = GetCurrentProcess();
      if (!SymInitialize(process, nullptr, TRUE))
        return;

      std::string sidecar = dump_path + \".symbols.txt\";
                            std::wstring sidecar_w = to_wide(sidecar);
      HANDLE file = CreateFileW(sidecar_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        SymCleanup(process);
        return;
      }

      CONTEXT context = *exception_pointers->ContextRecord;
#if defined(_M_X64)
      DWORD machine = IMAGE_FILE_MACHINE_AMD64;
      STACKFRAME64 frame{};
      frame.AddrPC.Offset = context.Rip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Rbp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Rsp;
      frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
      DWORD machine = IMAGE_FILE_MACHINE_I386;
      STACKFRAME64 frame{};
      frame.AddrPC.Offset = context.Eip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Ebp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Esp;
      frame.AddrStack.Mode = AddrModeFlat;
#else
      DWORD machine = 0;
      STACKFRAME64 frame{};
#endif
      char line[1024] = {0};
      DWORD written = 0;
      for (int i = 0; machine != 0 && i < 128; ++i) {
        if (!StackWalk64(machine, process, GetCurrentThread(), &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
          break;
        }
        alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
          int n = std::snprintf(line, sizeof(line), \"0x%llx %s+0x%llx\\n\",
                                static_cast<unsigned long long>(frame.AddrPC.Offset),
                                symbol->Name, static_cast<unsigned long long>(displacement));
          if (n > 0)
            WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
        }
      }
      CloseHandle(file);
      SymCleanup(process);
    }
    bool write_minidump(EXCEPTION_POINTERS* exception_pointers) {
      bool expected = false;
      if (!g_wrote_dump.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;

      std::string final_path = next_dump_path("dmp");
      std::string tmp_path = final_path + ".tmp";
      std::wstring tmp_w = to_wide(tmp_path);
      if (tmp_w.empty())
        return false;

      HANDLE file = CreateFileW(tmp_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        return false;

      MINIDUMP_EXCEPTION_INFORMATION exception_info{};
      exception_info.ThreadId = GetCurrentThreadId();
      exception_info.ExceptionPointers = exception_pointers;
      exception_info.ClientPointers = FALSE;
      crash_options opts = current_options();
      MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                           MiniDumpWithUnloadedModules);
      if (opts.include_full_memory_dump) {
        dump_type =
            static_cast<MINIDUMP_TYPE>(dump_type | MiniDumpWithFullMemory | MiniDumpWithDataSegs);
      }
      BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, dump_type,
                                  exception_pointers ? &exception_info : nullptr, nullptr, nullptr);
      (void)CloseHandle(file);
      if (!ok) {
        DeleteFileW(tmp_w.c_str());
        return false;
      }

      std::wstring final_w = to_wide(final_path);
      if (final_w.empty()) {
        DeleteFileW(tmp_w.c_str());
        return false;
      }
      if (!MoveFileExW(tmp_w.c_str(), final_w.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_w.c_str());
        return false;
      }

      write_symbol_sidecar(final_path, exception_pointers);
      record_last_dump_path(final_path);
      (void)upload_last_dump_if_requested(final_path);
      return true;
    }

    LONG WINAPI vectored_exception_handler(EXCEPTION_POINTERS* exception_pointers) {
      if (exception_pointers && exception_pointers->ExceptionRecord) {
        DWORD code = exception_pointers->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
            code == EXCEPTION_DATATYPE_MISALIGNMENT || code == EXCEPTION_FLT_DIVIDE_BY_ZERO ||
            code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
            code == EXCEPTION_STACK_OVERFLOW) {
          (void)write_minidump(exception_pointers);
        }
      }
      return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception_pointers) {
      (void)write_minidump(exception_pointers);
      return EXCEPTION_CONTINUE_SEARCH;
    }
  } // namespace

  bool platform_install_handlers(const crash_options&) {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      return true;

    g_vectored_handler = AddVectoredExceptionHandler(1, vectored_exception_handler);
    if (!g_vectored_handler) {
      g_installed.store(false, std::memory_order_release);
      return false;
    }
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    return true;
  }

  bool platform_self_test() noexcept {
    bool wrote = false;
    __try {
      __debugbreak();
    } __except ((wrote = write_minidump(GetExceptionInformation())) ? EXCEPTION_EXECUTE_HANDLER
                                                                    : EXCEPTION_CONTINUE_SEARCH) {
    }
    return wrote;
  }
} // namespace fxe::os::crash_detail
