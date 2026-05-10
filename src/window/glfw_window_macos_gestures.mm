#include "glfw_window_platform_hooks.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace fxe {
  namespace {
    constexpr const char* kSubclassPrefix = "FxeGestureHook_";
    static const void* kOwnerKey = &kOwnerKey;
    constexpr float kDegreesToRadians = 0.01745329251994329577f;

    input_event::scroll_phase_t map_event_phase(NSEventPhase phase, bool momentum) {
      switch (phase) {
      case NSEventPhaseBegan:
        return momentum ? input_event::scroll_phase_t::momentum_began
                        : input_event::scroll_phase_t::began;
      case NSEventPhaseChanged:
        return momentum ? input_event::scroll_phase_t::momentum_changed
                        : input_event::scroll_phase_t::changed;
      case NSEventPhaseEnded:
      case NSEventPhaseCancelled:
        return momentum ? input_event::scroll_phase_t::momentum_ended
                        : input_event::scroll_phase_t::ended;
      default:
        return input_event::scroll_phase_t::none;
      }
    }

    input_event::scroll_phase_t gesture_phase(NSEvent* event) {
      auto phase = map_event_phase(event.phase, false);
      if (phase != input_event::scroll_phase_t::none)
        return phase;
      if (event.momentumPhase != NSEventPhaseNone)
        return map_event_phase(event.momentumPhase, true);
      return input_event::scroll_phase_t::changed;
    }

    input_event::kind_t pinch_kind(input_event::scroll_phase_t phase) {
      switch (phase) {
      case input_event::scroll_phase_t::began:
        return input_event::kind_t::gesture_pinch_begin;
      case input_event::scroll_phase_t::ended:
      case input_event::scroll_phase_t::momentum_ended:
        return input_event::kind_t::gesture_pinch_end;
      default:
        return input_event::kind_t::gesture_pinch_change;
      }
    }

    input_event::kind_t rotate_kind(input_event::scroll_phase_t phase) {
      switch (phase) {
      case input_event::scroll_phase_t::began:
        return input_event::kind_t::gesture_rotate_begin;
      case input_event::scroll_phase_t::ended:
      case input_event::scroll_phase_t::momentum_ended:
        return input_event::kind_t::gesture_rotate_end;
      default:
        return input_event::kind_t::gesture_rotate_change;
      }
    }

    glfw_window* owner_for_view(NSView* view) {
      id owner = objc_getAssociatedObject(view, kOwnerKey);
      return owner ? static_cast<glfw_window*>([owner pointerValue]) : nullptr;
    }

    NSPoint event_point_in_view(NSView* view, NSEvent* event) {
      return [view convertPoint:event.locationInWindow fromView:nil];
    }

    void emit_pinch(NSView* self, NSEvent* event) {
      auto* owner = owner_for_view(self);
      if (!owner)
        return;
      input_event ev{};
      ev.scroll_phase = gesture_phase(event);
      ev.kind = pinch_kind(ev.scroll_phase);
      ev.magnification = static_cast<float>(event.magnification);
      const NSPoint point = event_point_in_view(self, event);
      ev.x = point.x;
      ev.y = point.y;
      glfw_window_inject_gesture_event(reinterpret_cast<window*>(owner), std::move(ev));
    }

    void emit_rotate(NSView* self, NSEvent* event) {
      auto* owner = owner_for_view(self);
      if (!owner)
        return;
      input_event ev{};
      ev.scroll_phase = gesture_phase(event);
      ev.kind = rotate_kind(ev.scroll_phase);
      ev.rotation_radians = static_cast<float>(event.rotation) * kDegreesToRadians;
      const NSPoint point = event_point_in_view(self, event);
      ev.x = point.x;
      ev.y = point.y;
      glfw_window_inject_gesture_event(reinterpret_cast<window*>(owner), std::move(ev));
    }

    void emit_swipe(NSView* self, NSEvent* event) {
      auto* owner = owner_for_view(self);
      if (!owner)
        return;
      input_event ev{};
      ev.kind = input_event::kind_t::gesture_swipe;
      ev.scroll_phase = gesture_phase(event);
      ev.swipe_dx = static_cast<int>(std::lround(event.deltaX));
      ev.swipe_dy = static_cast<int>(std::lround(event.deltaY));
      const NSPoint point = event_point_in_view(self, event);
      ev.x = point.x;
      ev.y = point.y;
      glfw_window_inject_gesture_event(reinterpret_cast<window*>(owner), std::move(ev));
    }

    bool emit_precise_scroll(NSView* self, NSEvent* event) {
      auto* owner = owner_for_view(self);
      if (!owner)
        return false;
      const bool precision = event.hasPreciseScrollingDeltas;
      const auto phase = map_event_phase(event.phase, false);
      const auto momentum_phase = map_event_phase(event.momentumPhase, true);
      if (!precision && phase == input_event::scroll_phase_t::none &&
          momentum_phase == input_event::scroll_phase_t::none) {
        return false;
      }
      input_event ev{};
      ev.kind = input_event::kind_t::mouse_wheel;
      ev.precision = precision;
      ev.scroll_phase = momentum_phase != input_event::scroll_phase_t::none ? momentum_phase : phase;
      ev.dx = event.scrollingDeltaX;
      ev.dy = event.scrollingDeltaY;
      const NSPoint point = event_point_in_view(self, event);
      ev.x = point.x;
      ev.y = point.y;
      glfw_window_inject_gesture_event(reinterpret_cast<window*>(owner), std::move(ev));
      return true;
    }

    void call_super(NSView* self, SEL sel, NSEvent* event) {
      struct objc_super super_info {
        .receiver = self,
        .super_class = class_getSuperclass(object_getClass(self)),
      };
      auto fn = reinterpret_cast<void (*)(struct objc_super*, SEL, NSEvent*)>(objc_msgSendSuper);
      fn(&super_info, sel, event);
    }

    void fxe_magnifyWithEvent(id self, SEL _cmd, NSEvent* event) {
      emit_pinch((NSView*)self, event);
      call_super((NSView*)self, _cmd, event);
    }

    void fxe_rotateWithEvent(id self, SEL _cmd, NSEvent* event) {
      emit_rotate((NSView*)self, event);
      call_super((NSView*)self, _cmd, event);
    }

    void fxe_swipeWithEvent(id self, SEL _cmd, NSEvent* event) {
      emit_swipe((NSView*)self, event);
      call_super((NSView*)self, _cmd, event);
    }

    void fxe_scrollWheel(id self, SEL _cmd, NSEvent* event) {
      if (emit_precise_scroll((NSView*)self, event))
        return;
      call_super((NSView*)self, _cmd, event);
    }

    Class gesture_subclass_for(Class base) {
      if (!base)
        return Nil;
      const char* base_name = class_getName(base);
      if (base_name && std::string_view(base_name).starts_with(kSubclassPrefix))
        return base;
      static std::mutex mu;
      static std::unordered_map<Class, Class> cache;
      std::lock_guard<std::mutex> lock(mu);
      if (auto it = cache.find(base); it != cache.end())
        return it->second;
      std::string name = std::string(kSubclassPrefix) + class_getName(base);
      Class subclass = objc_lookUpClass(name.c_str());
      if (!subclass) {
        subclass = objc_allocateClassPair(base, name.c_str(), 0);
        if (!subclass)
          return Nil;
        class_addMethod(subclass, @selector(magnifyWithEvent:), (IMP)fxe_magnifyWithEvent, "v@:@");
        class_addMethod(subclass, @selector(rotateWithEvent:), (IMP)fxe_rotateWithEvent, "v@:@");
        class_addMethod(subclass, @selector(swipeWithEvent:), (IMP)fxe_swipeWithEvent, "v@:@");
        class_addMethod(subclass, @selector(scrollWheel:), (IMP)fxe_scrollWheel, "v@:@");
        objc_registerClassPair(subclass);
      }
      cache.emplace(base, subclass);
      return subclass;
    }
  } // namespace

  void install_macos_gesture_hooks(void* nsview, glfw_window* w) {
    NSView* view = (__bridge NSView*)nsview;
    if (!view || !w)
      return;
    Class subclass = gesture_subclass_for(object_getClass(view));
    if (!subclass)
      return;
    if (object_getClass(view) != subclass)
      object_setClass(view, subclass);
    objc_setAssociatedObject(view, kOwnerKey, [NSValue valueWithPointer:w], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
} // namespace fxe

#endif
