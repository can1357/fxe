#include "glfw_window_platform_hooks.hpp"

#ifdef _WIN32

// Scope: WM_POINTER-backed precision scroll phase + pen/touch pointer motion.
// Full multi-contact pinch/rotate recognition is intentionally deferred.

#define WIN32_LEAN_AND_MEAN
#include <commctrl.h>
#include <windows.h>
#include <windowsx.h>

#include <mutex>
#include <unordered_map>
#include <utility>

namespace fxe {
  namespace {
    struct pointer_state {
      glfw_window* owner = nullptr;
      std::unordered_map<UINT32, POINT> last_points;
    };

    std::mutex& pointer_mutex() {
      static std::mutex mu;
      return mu;
    }

    std::unordered_map<HWND, pointer_state>& pointer_states() {
      static std::unordered_map<HWND, pointer_state> states;
      return states;
    }

    input_event::scroll_phase_t pointer_phase(UINT32 flags) {
      if ((flags & POINTER_FLAG_DOWN) != 0)
        return input_event::scroll_phase_t::began;
      if ((flags & POINTER_FLAG_UP) != 0)
        return input_event::scroll_phase_t::ended;
      if ((flags & POINTER_FLAG_UPDATE) != 0)
        return input_event::scroll_phase_t::changed;
      return input_event::scroll_phase_t::none;
    }

    bool pointer_client_point(HWND hwnd, const POINTER_INFO& info, POINT& out) {
      out = info.ptPixelLocation;
      return ScreenToClient(hwnd, &out) != FALSE;
    }

    void emit_pointer_move(HWND hwnd, UINT32 pointer_id, const POINTER_INFO& info,
                           glfw_window* owner) {
      if (!owner)
        return;
      if (info.pointerType != PT_TOUCH && info.pointerType != PT_PEN)
        return;
      POINT client{};
      if (!pointer_client_point(hwnd, info, client))
        return;

      POINT previous = client;
      {
        std::lock_guard<std::mutex> lock(pointer_mutex());
        auto& state = pointer_states()[hwnd];
        state.owner = owner;
        if (auto it = state.last_points.find(pointer_id); it != state.last_points.end())
          previous = it->second;
        state.last_points[pointer_id] = client;
        if ((info.pointerFlags & POINTER_FLAG_UP) != 0)
          state.last_points.erase(pointer_id);
      }

      input_event ev{};
      ev.kind = input_event::kind_t::mouse_move;
      ev.x = client.x;
      ev.y = client.y;
      ev.dx = client.x - previous.x;
      ev.dy = client.y - previous.y;
      glfw_window_inject_gesture_event(reinterpret_cast<window*>(owner), std::move(ev));
    }

    void emit_pointer_wheel(HWND hwnd, WPARAM wp, bool horizontal, glfw_window* owner) {
      if (!owner)
        return;
      POINTER_INFO info{};
      const UINT32 pointer_id = GET_POINTERID_WPARAM(wp);
      if (!GetPointerInfo(pointer_id, &info))
        return;
      POINT client{};
      if (!pointer_client_point(hwnd, info, client))
        return;

      input_event ev{};
      ev.kind = input_event::kind_t::mouse_wheel;
      ev.x = client.x;
      ev.y = client.y;
      ev.precision = true;
      ev.scroll_phase = pointer_phase(info.pointerFlags);
      const double delta = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
      if (horizontal)
        ev.dx = delta;
      else
        ev.dy = delta;
      glfw_window_inject_gesture_event(reinterpret_cast<window*>(owner), std::move(ev));
    }

    LRESULT CALLBACK pointer_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR subclass_id, DWORD_PTR ref_data) {
      (void)subclass_id;
      auto* owner = reinterpret_cast<glfw_window*>(ref_data);
      switch (msg) {
      case WM_POINTERDOWN:
      case WM_POINTERUPDATE:
      case WM_POINTERUP: {
        POINTER_INFO info{};
        const UINT32 pointer_id = GET_POINTERID_WPARAM(wp);
        if (GetPointerInfo(pointer_id, &info))
          emit_pointer_move(hwnd, pointer_id, info, owner);
        break;
      }
      case WM_POINTERWHEEL:
        emit_pointer_wheel(hwnd, wp, false, owner);
        return 0;
      case WM_POINTERHWHEEL:
        emit_pointer_wheel(hwnd, wp, true, owner);
        return 0;
      case WM_NCDESTROY: {
        {
          std::lock_guard<std::mutex> lock(pointer_mutex());
          pointer_states().erase(hwnd);
        }
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        RemoveWindowSubclass(hwnd, pointer_subclass_proc, subclass_id);
        return result;
      }
      default:
        break;
      }
      return DefSubclassProc(hwnd, msg, wp, lp);
    }
  } // namespace

  void install_win32_pointer_hooks(void* hwnd_void, glfw_window* w) {
    HWND hwnd = static_cast<HWND>(hwnd_void);
    if (!hwnd || !w)
      return;
    {
      std::lock_guard<std::mutex> lock(pointer_mutex());
      pointer_states()[hwnd].owner = w;
    }
    constexpr UINT_PTR kSubclassId = 0x46584547u;
    (void)SetWindowSubclass(hwnd, pointer_subclass_proc, kSubclassId,
                            reinterpret_cast<DWORD_PTR>(w));
  }
} // namespace fxe

#endif
