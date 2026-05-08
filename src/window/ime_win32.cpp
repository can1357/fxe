// v0 IMM32 IME bridge for GLFW Win32 windows.
// Limitations: TSF integration is deferred; this covers composition/result text
// through WM_IME_* only. Extend by adding TSF-aware focus/candidate handling if
// IMM32 parity is not sufficient.

#include "../os/os.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <imm.h>
#include <windows.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fxe::os {
  namespace {
    struct win32_ime_bridge {
      void* owner = nullptr;
      win32_ime_emit_fn emit = nullptr;
      WNDPROC original_wndproc = nullptr;
    };

    std::mutex& bridge_mutex() {
      static std::mutex mu;
      return mu;
    }

    std::unordered_map<HWND, win32_ime_bridge>& bridge_map() {
      static std::unordered_map<HWND, win32_ime_bridge> map;
      return map;
    }

    std::string utf8_from_utf16(const wchar_t* value, int length) {
      if (!value || length <= 0)
        return {};
      const int needed =
          WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
      if (needed <= 0)
        return {};
      std::string out(static_cast<usize>(needed), '\0');
      if (WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), needed, nullptr, nullptr) <= 0)
        return {};
      return out;
    }

    std::string read_composition_utf8(HIMC himc, DWORD which) {
      if (!himc)
        return {};
      const LONG bytes = ImmGetCompositionStringW(himc, which, nullptr, 0);
      if (bytes <= 0)
        return {};
      std::vector<wchar_t> buffer(static_cast<usize>(bytes) / sizeof(wchar_t));
      const LONG copied = ImmGetCompositionStringW(himc, which, buffer.data(), bytes);
      if (copied <= 0)
        return {};
      return utf8_from_utf16(buffer.data(), static_cast<int>(copied / sizeof(wchar_t)));
    }

    int read_cursor_pos(HIMC himc) {
      if (!himc)
        return 0;
      const LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
      return cursor < 0 ? 0 : static_cast<int>(cursor);
    }

    LRESULT CALLBACK ime_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
      win32_ime_bridge bridge;
      {
        std::lock_guard<std::mutex> lock(bridge_mutex());
        auto it = bridge_map().find(hwnd);
        if (it != bridge_map().end())
          bridge = it->second;
      }
      if (!bridge.original_wndproc)
        return DefWindowProcW(hwnd, msg, wp, lp);

      switch (msg) {
      case WM_IME_STARTCOMPOSITION:
        if (bridge.emit)
          bridge.emit(bridge.owner, "", 0, "");
        break;
      case WM_IME_COMPOSITION: {
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
          if ((lp & GCS_COMPSTR) != 0 && bridge.emit) {
            std::string preedit = read_composition_utf8(himc, GCS_COMPSTR);
            bridge.emit(bridge.owner, preedit.c_str(), read_cursor_pos(himc), "");
          }
          if ((lp & GCS_RESULTSTR) != 0 && bridge.emit) {
            std::string committed = read_composition_utf8(himc, GCS_RESULTSTR);
            bridge.emit(bridge.owner, "", 0, committed.c_str());
          }
          ImmReleaseContext(hwnd, himc);
        }
        break;
      }
      case WM_IME_ENDCOMPOSITION:
        if (bridge.emit)
          bridge.emit(bridge.owner, "", 0, "");
        break;
      default:
        break;
      }

      return CallWindowProcW(bridge.original_wndproc, hwnd, msg, wp, lp);
    }
  } // namespace

  void install_win32_ime_bridge(void* hwnd_void, void* owner, win32_ime_emit_fn emit) {
    HWND hwnd = static_cast<HWND>(hwnd_void);
    if (!hwnd || !emit)
      return;

    std::lock_guard<std::mutex> lock(bridge_mutex());
    auto& bridges = bridge_map();
    auto it = bridges.find(hwnd);
    if (it != bridges.end()) {
      it->second.owner = owner;
      it->second.emit = emit;
      return;
    }

    SetLastError(0);
    LONG_PTR previous =
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ime_wndproc));
    if (previous == 0 && GetLastError() != 0)
      return;

    bridges.emplace(hwnd,
                    win32_ime_bridge{
                        .owner = owner,
                        .emit = emit,
                        .original_wndproc =
                            previous != 0 ? reinterpret_cast<WNDPROC>(previous) : DefWindowProcW,
                    });
  }
} // namespace fxe::os
#endif
