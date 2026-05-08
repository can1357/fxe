#include "os/a11y.hpp"

#import <AppKit/AppKit.h>

namespace {
  std::shared_ptr<const fxe::os::a11y::snapshot> fxe_accessibility_snapshot_for_view(NSView* view) {
    if (!view)
      return nullptr;
    if (NSWindow* window = view.window) {
      if (auto snap = fxe::os::a11y::snapshot_for_window((__bridge void*)window))
        return snap;
      if (auto snap = fxe::os::a11y::snapshot_for_window(
              reinterpret_cast<void*>(static_cast<uintptr_t>(window.windowNumber))))
        return snap;
    }
    if (auto snap = fxe::os::a11y::snapshot_for_window((__bridge void*)view))
      return snap;
    return nullptr;
  }
}

// FxeContentView (already exists in glfw_window.cpp) implements NSAccessibility
// protocol methods that read the cached snapshot.
//
// For v1, expose a group container with no children while keeping the snapshot
// install path fully wired for follow-up per-node FxeAXElement objects.
//
// TODO(D2 v2): build FxeAXElement instances from snapshot->json walks.
@interface FxeContentView : NSView
@end

@interface FxeContentView (FxeAccessibility) <NSAccessibility>
@end

@implementation FxeContentView (FxeAccessibility)
- (BOOL)isAccessibilityElement {
  return YES;
}

- (NSAccessibilityRole)accessibilityRole {
  (void)fxe_accessibility_snapshot_for_view(self);
  return NSAccessibilityGroupRole;
}

- (NSArray*)accessibilityChildren {
  (void)fxe_accessibility_snapshot_for_view(self);
  return @[];
}
@end
