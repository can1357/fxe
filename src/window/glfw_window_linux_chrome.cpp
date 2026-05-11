#include <fxe/log.hpp>
#include <fxe/types.hpp>

#if defined(__linux__) && !defined(__APPLE__)

struct GLFWwindow;

#if FXE_HAS_GLFW
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

#include <dlfcn.h>
#include <mutex>

#if defined(GLFW_EXPOSE_NATIVE_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif
#endif

namespace fxe {
#if FXE_HAS_GLFW
  namespace {
    void warn_once(bool& warned, const char* message) {
      if (!warned) {
        warned = true;
        FXE_WARN("window", "{}", message);
      }
    }

#if defined(GLFW_EXPOSE_NATIVE_X11)
    struct x11_api {
      using xintern_atom_fn = Atom (*)(Display*, const char*, Bool);
      using xchange_property_fn = int (*)(Display*, Window, Atom, Atom, int, int,
                                          const unsigned char*, int);
      using xdelete_property_fn = int (*)(Display*, Window, Atom);
      using xflush_fn = int (*)(Display*);

      void* handle = nullptr;
      xintern_atom_fn xintern_atom = nullptr;
      xchange_property_fn xchange_property = nullptr;
      xdelete_property_fn xdelete_property = nullptr;
      xflush_fn xflush = nullptr;
    };

    const x11_api* load_x11_api() {
      static std::once_flag once;
      static x11_api api{};
      std::call_once(once, [] {
        api.handle = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!api.handle)
          api.handle = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
        if (!api.handle)
          return;
        api.xintern_atom =
            reinterpret_cast<x11_api::xintern_atom_fn>(dlsym(api.handle, "XInternAtom"));
        api.xchange_property =
            reinterpret_cast<x11_api::xchange_property_fn>(dlsym(api.handle, "XChangeProperty"));
        api.xdelete_property =
            reinterpret_cast<x11_api::xdelete_property_fn>(dlsym(api.handle, "XDeleteProperty"));
        api.xflush = reinterpret_cast<x11_api::xflush_fn>(dlsym(api.handle, "XFlush"));
        if (!api.xintern_atom || !api.xchange_property || !api.xdelete_property || !api.xflush) {
          dlclose(api.handle);
          api = {};
        }
      });
      return api.handle ? &api : nullptr;
    }

    bool linux_clear_gtk_frame_extents_x11(GLFWwindow* window, const x11_api& api) {
      auto* display = glfwGetX11Display();
      const auto xwindow = glfwGetX11Window(window);
      if (!display || xwindow == 0)
        return false;
      const Atom atom = api.xintern_atom(display, "_GTK_FRAME_EXTENTS", False);
      if (atom == None)
        return false;
      api.xdelete_property(display, xwindow, atom);
      api.xflush(display);
      return true;
    }
#endif
  } // namespace

  bool linux_set_gtk_frame_extents(GLFWwindow* window, i32 left, i32 right, i32 top, i32 bottom) {
    if (!window)
      return false;

#if defined(GLFW_PLATFORM_WAYLAND)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
      static bool warned_wayland_gtk_shell = false;
      warn_once(warned_wayland_gtk_shell, "fxe.window: setGtkFrameExtents requires a Wayland "
                                          "gtk-shell handshake that GLFW does not expose");
      return false;
    }
#endif

#if defined(GLFW_PLATFORM_X11) && defined(GLFW_EXPOSE_NATIVE_X11)
    if (glfwGetPlatform() != GLFW_PLATFORM_X11)
      return false;

    const x11_api* api = load_x11_api();
    if (!api) {
      static bool warned_missing_x11 = false;
      warn_once(warned_missing_x11,
                "fxe.window: setGtkFrameExtents could not load libX11 for the X11 backend");
      return false;
    }

    if (left == 0 && right == 0 && top == 0 && bottom == 0)
      return linux_clear_gtk_frame_extents_x11(window, *api);

    auto* display = glfwGetX11Display();
    const auto xwindow = glfwGetX11Window(window);
    if (!display || xwindow == 0)
      return false;

    const Atom atom = api->xintern_atom(display, "_GTK_FRAME_EXTENTS", False);
    if (atom == None)
      return false;

    const unsigned long extents[4] = {
        static_cast<unsigned long>(left),
        static_cast<unsigned long>(right),
        static_cast<unsigned long>(top),
        static_cast<unsigned long>(bottom),
    };
    api->xchange_property(display, xwindow, atom, XA_CARDINAL, 32, PropModeReplace,
                          reinterpret_cast<const unsigned char*>(extents), 4);
    api->xflush(display);
    return true;
#else
    static_cast<void>(left);
    static_cast<void>(right);
    static_cast<void>(top);
    static_cast<void>(bottom);
    return false;
#endif
  }
#else
  bool linux_set_gtk_frame_extents(GLFWwindow* window, i32 left, i32 right, i32 top, i32 bottom) {
    static_cast<void>(window);
    static_cast<void>(left);
    static_cast<void>(right);
    static_cast<void>(top);
    static_cast<void>(bottom);
    return false;
  }
#endif
} // namespace fxe

#endif
