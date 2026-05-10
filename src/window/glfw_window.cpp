#include "../os/os.hpp"
#include "glfw_window_platform_hooks.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fxe/log.hpp>
#include <fxe/types.hpp>
#include <fxe/window.hpp>

#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if FXE_HAS_GLFW
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>
#endif // FXE_HAS_GLFW

#if FXE_HAS_WGPU
#include <webgpu/webgpu_cpp.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <commctrl.h>
#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_AUTO
#define DWMSBT_AUTO 0
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif
#ifndef DWMSBT_TABBEDWINDOW
#define DWMSBT_TABBEDWINDOW 4
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

#if defined(__APPLE__)
// ----------------------------------------------------------------------------
// Objective-C++ helpers. glfw_window.cpp is compiled with -x objective-c++ on
// Apple (see top-level CMakeLists.txt).
// ----------------------------------------------------------------------------
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#if FXE_HAS_WGPU
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#endif

namespace fxe {
  static void fxe_macos_activate_app() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      NSApplication* app = [NSApplication sharedApplication];
      [app setActivationPolicy:NSApplicationActivationPolicyRegular];
      [app activateIgnoringOtherApps:YES];
    });
  }

  static void fxe_macos_raise_window(void* nswindow_void) {
    NSWindow* nswindow = (__bridge NSWindow*)nswindow_void;
    NSApplication* app = [NSApplication sharedApplication];
    if ([app respondsToSelector:@selector(activate)]) {
      [app performSelector:@selector(activate)];
    } else {
      [app activateIgnoringOtherApps:YES];
    }
    [nswindow makeKeyAndOrderFront:nil];
    [nswindow orderFrontRegardless];
  }

  static bool fxe_macos_read_clipboard_image(clipboard_image& out) {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSArray<Class>* classes = @[ [NSImage class] ];
    NSDictionary* options = @{};
    NSArray* objects = [pasteboard readObjectsForClasses:classes options:options];
    NSImage* image = objects.count > 0 ? objects[0] : nil;
    if (!image)
      return false;

    CGImageRef cg = [image CGImageForProposedRect:nil context:nil hints:nil];
    if (!cg)
      return false;

    const usize width = CGImageGetWidth(cg);
    const usize height = CGImageGetHeight(cg);
    if (width == 0 || height == 0 || width > UINT32_MAX || height > UINT32_MAX ||
        width > (SIZE_MAX / height) / 4u) {
      return false;
    }

    std::vector<u8> pixels(width * height * 4u);
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    if (!color_space)
      return false;
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), width, height, 8, width * 4u, color_space,
        static_cast<CGBitmapInfo>(kCGImageAlphaLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color_space);
    if (!ctx)
      return false;
    CGContextClearRect(ctx, CGRectMake(0, 0, width, height));
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), cg);
    CGContextRelease(ctx);

    out.width = static_cast<u32>(width);
    out.height = static_cast<u32>(height);
    out.data = std::move(pixels);
    return true;
  }

  static bool fxe_macos_write_clipboard_image(const clipboard_image& image) {
    const usize width = image.width;
    const usize height = image.height;
    if (width == 0 || height == 0 || width > (SIZE_MAX / height) / 4u ||
        image.data.size() < width * height * 4u) {
      return false;
    }

    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    if (!color_space)
      return false;
    const usize byte_count = width * height * 4u;
    if (byte_count > static_cast<usize>(std::numeric_limits<CFIndex>::max())) {
      CGColorSpaceRelease(color_space);
      return false;
    }
    CFDataRef data =
        CFDataCreate(kCFAllocatorDefault, image.data.data(), static_cast<CFIndex>(byte_count));
    if (!data) {
      CGColorSpaceRelease(color_space);
      return false;
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider) {
      CGColorSpaceRelease(color_space);
      return false;
    }
    CGImageRef cg =
        CGImageCreate(width, height, 8, 32, width * 4u, color_space,
                      static_cast<CGBitmapInfo>(kCGImageAlphaLast) | kCGBitmapByteOrder32Big,
                      provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(color_space);
    if (!cg)
      return false;

    NSImage* ns_image = [[NSImage alloc]
        initWithCGImage:cg
                   size:NSMakeSize(static_cast<CGFloat>(width), static_cast<CGFloat>(height))];
    CGImageRelease(cg);
    if (!ns_image)
      return false;

    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard writeObjects:@[ ns_image ]] == YES;
  }
#if FXE_HAS_WGPU
  static void* fxe_wgpu_metal_layer_for_window(void* nswindow_void, bool transparent) {
    NSWindow* nswindow = (__bridge NSWindow*)nswindow_void;
    NSView* view = [nswindow contentView];
    CAMetalLayer* metal = [CAMetalLayer layer];
    metal.pixelFormat = MTLPixelFormatBGRA8Unorm;
    metal.framebufferOnly = YES;
    // Avoid the default kCAGravityResize, which stretches the previous frame's
    // pixels across the new layer bounds during a Cocoa live-resize until our
    // framebuffer-size callback drives a fresh frame. Anchoring to the top-left
    // leaves uncovered edge pixels for at most one tick instead of a visible
    // scale-and-snap.
    metal.contentsGravity = kCAGravityTopLeft;
    if (transparent) {
      metal.opaque = NO;
    }
    CGFloat scale = [nswindow backingScaleFactor];
    metal.contentsScale = scale;
    NSSize bounds = view.bounds.size;
    metal.drawableSize = CGSizeMake(bounds.width * scale, bounds.height * scale);
    view.layer = metal;
    view.wantsLayer = YES;
    return (__bridge_retained void*)metal;
  }
#endif // FXE_HAS_WGPU

  // ---------------------------------------------------------------------------
  // FxeContentView — wraps GLFW's content view so we can:
  //   - return self from -hitTest: when the point is inside any drag rect, so
  //     -mouseDown: can call -performWindowDragWithEvent: to drag the window.
  //   - host the CAMetalLayer for transparent / undecorated windows.
  // The GLFW NSView is added as a subview and autoresizes with us, so its
  // mouse-tracking, key-handling, and framebuffer-size bookkeeping keep working
  // for clicks that fall outside drag rects.
  // ---------------------------------------------------------------------------
} // namespace fxe

using FxeMacosDragEmitFn = void (*)(void*, fxe::input_event::kind_t, double, double,
                                    const std::vector<std::string>&);

static std::vector<std::string> fxe_macos_file_paths_from_drag(id<NSDraggingInfo> sender) {
  std::vector<std::string> paths;
  NSPasteboard* pasteboard = [sender draggingPasteboard];
  if (!pasteboard)
    return paths;
  NSDictionary* options = @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
  NSArray<NSURL*>* urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ] options:options];
  paths.reserve(static_cast<usize>(urls.count));
  for (NSURL* url in urls) {
    if (!url.fileURL)
      continue;
    NSString* path = url.path;
    const char* utf8 = path ? path.UTF8String : nullptr;
    if (utf8)
      paths.emplace_back(utf8);
  }
  return paths;
}

@interface FxeContentView : NSView <NSDraggingSource, NSDraggingDestination>
- (void)fxe_setDragRects:(NSArray<NSValue*>*)rects;
- (void)fxe_setDragDestinationOwner:(void*)owner emit:(FxeMacosDragEmitFn)emit;
@end

@implementation FxeContentView {
  NSArray<NSValue*>* _fxeDragRects;
  void* _fxeDragOwner;
  FxeMacosDragEmitFn _fxeDragEmit;
}
- (void)fxe_setDragRects:(NSArray<NSValue*>*)rects {
  _fxeDragRects = [rects copy];
}
- (void)fxe_setDragDestinationOwner:(void*)owner emit:(FxeMacosDragEmitFn)emit {
  _fxeDragOwner = owner;
  _fxeDragEmit = emit;
}
- (void)fxe_emitDragKind:(fxe::input_event::kind_t)kind
                  sender:(id<NSDraggingInfo>)sender
            includePaths:(BOOL)includePaths {
  if (!_fxeDragOwner || !_fxeDragEmit)
    return;
  NSPoint local = [self convertPoint:[sender draggingLocation] fromView:nil];
  const double x = static_cast<double>(local.x);
  const double y = static_cast<double>(self.bounds.size.height - local.y);
  auto paths = includePaths ? fxe_macos_file_paths_from_drag(sender) : std::vector<std::string>{};
  _fxeDragEmit(_fxeDragOwner, kind, x, y, paths);
}
- (BOOL)mouseDownCanMoveWindow {
  return YES;
}
- (NSView*)hitTest:(NSPoint)point {
  // `point` is in the receiver's superview's coordinate system. Convert to
  // window-local first, then to a top-left-origin space matching what JS
  // callers passed to setDragRegion().
  NSPoint windowPoint = [self.superview convertPoint:point toView:nil];
  NSPoint local = [self convertPoint:windowPoint fromView:nil];
  CGFloat h = self.bounds.size.height;
  CGFloat ty = h - local.y;
  for (NSValue* v in _fxeDragRects) {
    NSRect r = [v rectValue];
    if (local.x >= r.origin.x && local.x < r.origin.x + r.size.width && ty >= r.origin.y &&
        ty < r.origin.y + r.size.height) {
      return self;
    }
  }
  return [super hitTest:point];
}
- (void)mouseDown:(NSEvent*)event {
  // Reached only for clicks inside a drag rect (see -hitTest:).
  [self.window performWindowDragWithEvent:event];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  [self fxe_emitDragKind:fxe::input_event::kind_t::drag_enter sender:sender includePaths:YES];
  return NSDragOperationCopy;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
  [self fxe_emitDragKind:fxe::input_event::kind_t::drag_over sender:sender includePaths:YES];
  return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
  [self fxe_emitDragKind:fxe::input_event::kind_t::drag_leave sender:sender includePaths:NO];
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
  (void)sender;
  return YES;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  [self fxe_emitDragKind:fxe::input_event::kind_t::drop_files sender:sender includePaths:YES];
  return YES;
}
- (void)draggingEnded:(id<NSDraggingInfo>)sender {
  [self fxe_emitDragKind:fxe::input_event::kind_t::drag_leave sender:sender includePaths:NO];
}
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
  (void)session;
  (void)context;
  return NSDragOperationCopy;
}
@end

@interface FxeImeBridge : NSObject {
@public
  void* owner;
  void (*emit)(void*, const char*, int, const char*);
}
@end

@implementation FxeImeBridge
@end

static const void* FxeImeBridgeKey = &FxeImeBridgeKey;

static NSString* fxe_string_from_marked_text(id value) {
  if (!value)
    return @"";
  if ([value isKindOfClass:[NSAttributedString class]])
    return [(NSAttributedString*)value string] ?: @"";
  if ([value isKindOfClass:[NSString class]])
    return (NSString*)value;
  return [value description] ?: @"";
}

static void fxe_emit_ime_from_view(NSView* view, NSString* preedit, NSInteger cursor,
                                   NSString* committed) {
  FxeImeBridge* bridge = (FxeImeBridge*)objc_getAssociatedObject(view, FxeImeBridgeKey);
  if (!bridge || !bridge->emit)
    return;
  const char* preedit_utf8 = preedit ? [preedit UTF8String] : "";
  const char* committed_utf8 = committed ? [committed UTF8String] : "";
  bridge->emit(bridge->owner, preedit_utf8 ? preedit_utf8 : "", static_cast<int>(cursor),
               committed_utf8 ? committed_utf8 : "");
}

@interface NSView (FxeImeCompose)
- (void)fxe_setMarkedText:(id)string
            selectedRange:(NSRange)selectedRange
         replacementRange:(NSRange)replacementRange;
- (void)fxe_insertText:(id)string replacementRange:(NSRange)replacementRange;
@end

@implementation NSView (FxeImeCompose)
- (void)fxe_setMarkedText:(id)string
            selectedRange:(NSRange)selectedRange
         replacementRange:(NSRange)replacementRange {
  NSString* preedit = fxe_string_from_marked_text(string);
  NSInteger cursor =
      selectedRange.location == NSNotFound ? 0 : static_cast<NSInteger>(selectedRange.location);
  fxe_emit_ime_from_view(self, preedit, cursor, @"");
  [self fxe_setMarkedText:string selectedRange:selectedRange replacementRange:replacementRange];
}
- (void)fxe_insertText:(id)string replacementRange:(NSRange)replacementRange {
  NSString* committed = fxe_string_from_marked_text(string);
  fxe_emit_ime_from_view(self, @"", 0, committed);
  [self fxe_insertText:string replacementRange:replacementRange];
}
@end

namespace fxe {
  static void* fxe_macos_install_drag_view(void* nswindow_void) {
    NSWindow* nswindow = (__bridge NSWindow*)nswindow_void;
    NSView* old = [nswindow contentView];
    if ([old isKindOfClass:[FxeContentView class]]) {
      [(FxeContentView*)old registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
      // Already wrapped (idempotent — caller may invoke this on every show).
      return (__bridge void*)old;
    }
    FxeContentView* wrap = [[FxeContentView alloc] initWithFrame:old.frame];
    wrap.autoresizesSubviews = YES;
    [wrap registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    old.frame = wrap.bounds;
    old.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [wrap addSubview:old];
    [nswindow setContentView:wrap];
    return (__bridge_retained void*)wrap;
  }

  static void fxe_macos_install_drag_destination(void* wrap_view_ptr, void* owner,
                                                 FxeMacosDragEmitFn emit) {
    if (!wrap_view_ptr)
      return;
    FxeContentView* wrap = (__bridge FxeContentView*)wrap_view_ptr;
    [wrap registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    [wrap fxe_setDragDestinationOwner:owner emit:emit];
  }

  using fxe_macos_ime_emit_fn = void (*)(void*, const char*, int, const char*);

  static void fxe_macos_swizzle_ime_method(Class cls, SEL original_sel, SEL replacement_sel) {
    Method original = class_getInstanceMethod(cls, original_sel);
    Method replacement = class_getInstanceMethod([NSView class], replacement_sel);
    if (!original || !replacement)
      return;
    if (!class_addMethod(cls, replacement_sel, method_getImplementation(replacement),
                         method_getTypeEncoding(original))) {
      return;
    }
    Method installed = class_getInstanceMethod(cls, replacement_sel);
    method_exchangeImplementations(original, installed);
  }

  [[maybe_unused]] static void fxe_macos_install_ime_bridge(void* nswindow_void, void* owner,
                                                            fxe_macos_ime_emit_fn emit) {
    NSWindow* nswindow = (__bridge NSWindow*)nswindow_void;
    NSView* view = [nswindow contentView];
    if ([view isKindOfClass:[FxeContentView class]] && view.subviews.count > 0)
      view = view.subviews[0];
    if (!view)
      return;

    static NSMutableSet<NSString*>* swizzled = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      swizzled = [[NSMutableSet alloc] init];
    });

    Class cls = [view class];
    NSString* class_name = NSStringFromClass(cls);
    @synchronized(swizzled) {
      if (![swizzled containsObject:class_name]) {
        fxe_macos_swizzle_ime_method(cls, @selector(setMarkedText:selectedRange:replacementRange:),
                                     @selector(fxe_setMarkedText:selectedRange:replacementRange:));
        fxe_macos_swizzle_ime_method(cls, @selector(insertText:replacementRange:),
                                     @selector(fxe_insertText:replacementRange:));
        [swizzled addObject:class_name];
      }
    }

    FxeImeBridge* bridge = [FxeImeBridge new];
    bridge->owner = owner;
    bridge->emit = emit;
    objc_setAssociatedObject(view, FxeImeBridgeKey, bridge, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }

  static void fxe_macos_apply_transparent(void* nswindow_void) {
    NSWindow* ns = (__bridge NSWindow*)nswindow_void;
    [ns setOpaque:NO];
    [ns setBackgroundColor:[NSColor clearColor]];
    [ns setHasShadow:YES];
  }
  static bool fxe_macos_set_content_protection(void* nswindow_void, bool enabled) {
    NSWindow* ns = (__bridge NSWindow*)nswindow_void;
    if (!ns)
      return false;
    [ns setSharingType:enabled ? NSWindowSharingNone : NSWindowSharingReadOnly];
    return true;
  }

  static NSVisualEffectMaterial fxe_macos_vibrancy_material(const char* kind) {
    if (!kind)
      return NSVisualEffectMaterialUnderWindowBackground;
    if (std::strcmp(kind, "sidebar") == 0)
      return NSVisualEffectMaterialSidebar;
    if (std::strcmp(kind, "titlebar") == 0)
      return NSVisualEffectMaterialTitlebar;
    if (std::strcmp(kind, "menu") == 0)
      return NSVisualEffectMaterialMenu;
    return NSVisualEffectMaterialUnderWindowBackground;
  }

  static void* fxe_macos_set_visual_effect(void* nswindow_void, void* effect_view_ptr,
                                           const char* kind, bool blur_enabled) {
    NSWindow* ns = (__bridge NSWindow*)nswindow_void;
    if (!ns)
      return nullptr;
    NSView* content = [ns contentView];
    if (!content)
      return nullptr;

    NSVisualEffectView* effect = (__bridge NSVisualEffectView*)effect_view_ptr;
    const bool enabled = blur_enabled || kind != nullptr;
    if (!enabled) {
      if (effect) {
        [effect removeFromSuperview];
        CFRelease(effect_view_ptr);
      }
      return nullptr;
    }

    if (!effect) {
      effect = [[NSVisualEffectView alloc] initWithFrame:content.bounds];
      effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
      effect.state = NSVisualEffectStateActive;
      [content addSubview:effect positioned:NSWindowBelow relativeTo:nil];
      effect_view_ptr = (__bridge_retained void*)effect;
    }
    effect.frame = content.bounds;
    effect.material = fxe_macos_vibrancy_material(kind);
    effect.hidden = NO;
    [ns setOpaque:NO];
    [ns setBackgroundColor:[NSColor clearColor]];
    return effect_view_ptr;
  }

  static void fxe_macos_release_visual_effect(void* effect_view_ptr) {
    if (!effect_view_ptr)
      return;
    NSVisualEffectView* effect = (__bridge_transfer NSVisualEffectView*)effect_view_ptr;
    [effect removeFromSuperview];
  }
  static CGImageRef fxe_macos_create_cg_image_from_rgba(const u8* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0)
      return nullptr;
    const usize w = static_cast<usize>(width);
    const usize h = static_cast<usize>(height);
    if (w > (SIZE_MAX / h) / 4u)
      return nullptr;

    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    if (!color_space)
      return nullptr;
    const usize byte_count = w * h * 4u;
    if (byte_count > static_cast<usize>(std::numeric_limits<CFIndex>::max())) {
      CGColorSpaceRelease(color_space);
      return nullptr;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, rgba, static_cast<CFIndex>(byte_count));
    if (!data) {
      CGColorSpaceRelease(color_space);
      return nullptr;
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider) {
      CGColorSpaceRelease(color_space);
      return nullptr;
    }
    CGImageRef image =
        CGImageCreate(w, h, 8, 32, w * 4u, color_space,
                      static_cast<CGBitmapInfo>(kCGImageAlphaLast) | kCGBitmapByteOrder32Big,
                      provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(color_space);
    return image;
  }

  static void fxe_macos_set_movable_by_background(void* nswindow_void, bool movable) {
    NSWindow* ns = (__bridge NSWindow*)nswindow_void;
    [ns setMovableByWindowBackground:movable ? YES : NO];
  }
  static void fxe_macos_set_traffic_light_position(void* nswindow_void, int x, int y) {
    NSWindow* ns = (__bridge NSWindow*)nswindow_void;
    if (!ns)
      return;
    NSButton* close = [ns standardWindowButton:NSWindowCloseButton];
    NSButton* mini = [ns standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [ns standardWindowButton:NSWindowZoomButton];
    if (!close || !mini || !zoom)
      return;
    NSView* superview = close.superview;
    if (!superview)
      return;

    const NSPoint close_origin = close.frame.origin;
    const CGFloat mini_dx = mini.frame.origin.x - close_origin.x;
    const CGFloat zoom_dx = zoom.frame.origin.x - close_origin.x;
    const CGFloat cocoa_y =
        superview.bounds.size.height - static_cast<CGFloat>(y) - close.frame.size.height;
    [close setFrameOrigin:NSMakePoint(static_cast<CGFloat>(x), cocoa_y)];
    [mini setFrameOrigin:NSMakePoint(static_cast<CGFloat>(x) + mini_dx, cocoa_y)];
    [zoom setFrameOrigin:NSMakePoint(static_cast<CGFloat>(x) + zoom_dx, cocoa_y)];
  }

  static NSImage* fxe_macos_create_ns_image_from_rgba(const u8* rgba, int width, int height) {
    CGImageRef cg = fxe_macos_create_cg_image_from_rgba(rgba, width, height);
    if (!cg)
      return nil;
    NSImage* image = [[NSImage alloc]
        initWithCGImage:cg
                   size:NSMakeSize(static_cast<CGFloat>(width), static_cast<CGFloat>(height))];
    CGImageRelease(cg);
    return image;
  }
  static void fxe_macos_set_window_icon(const u8* rgba, int width, int height) {
    NSImage* image = fxe_macos_create_ns_image_from_rgba(rgba, width, height);
    if (!image)
      return;
    [[NSApplication sharedApplication] setApplicationIconImage:image];
  }

  static NSImage* fxe_macos_drag_icon(const drag_payload& payload) {
    const image_data* source = nullptr;
    if (payload.icon && !payload.icon->data.empty())
      source = &*payload.icon;
    else if (payload.image && !payload.image->data.empty())
      source = &*payload.image;
    if (source) {
      NSImage* image = fxe_macos_create_ns_image_from_rgba(
          source->data.data(), static_cast<int>(source->width), static_cast<int>(source->height));
      if (image)
        return image;
    }
    NSImage* image = [NSImage imageNamed:NSImageNameMultipleDocuments];
    if (image)
      return image;
    image = [[NSImage alloc] initWithSize:NSMakeSize(32, 32)];
    [image lockFocus];
    [[NSColor selectedControlColor] setFill];
    NSRectFill(NSMakeRect(4, 4, 24, 24));
    [image unlockFocus];
    return image;
  }

  static bool fxe_macos_start_drag(void* nswindow_void, const drag_payload& payload) {
    NSWindow* ns = (__bridge NSWindow*)nswindow_void;
    if (!ns)
      return false;
    NSView* view = [ns contentView];
    if (!view)
      return false;
    NSEvent* event = [NSApp currentEvent];
    if (!event)
      return false;

    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
    NSImage* icon = fxe_macos_drag_icon(payload);
    NSPoint point = [view convertPoint:[event locationInWindow] fromView:nil];
    NSRect frame = NSMakeRect(point.x, point.y, std::max<CGFloat>(icon.size.width, 1.0),
                              std::max<CGFloat>(icon.size.height, 1.0));

    for (const auto& file : payload.files) {
      NSString* path = [NSString stringWithUTF8String:file.c_str()];
      if (!path)
        continue;
      NSURL* url = [NSURL fileURLWithPath:path];
      if (!url)
        continue;
      NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
      [item setDraggingFrame:frame contents:icon];
      [items addObject:item];
    }

    if (payload.text && !payload.html && !payload.image) {
      NSString* text = [NSString stringWithUTF8String:payload.text->c_str()];
      if (text) {
        NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:text];
        [item setDraggingFrame:frame contents:icon];
        [items addObject:item];
      }
    }

    if (payload.html || payload.image) {
      NSPasteboardItem* pasteboard_item = [[NSPasteboardItem alloc] init];
      BOOL wrote = NO;
      if (payload.text) {
        NSString* text = [NSString stringWithUTF8String:payload.text->c_str()];
        if (text)
          wrote = [pasteboard_item setString:text forType:NSPasteboardTypeString] || wrote;
      }
      if (payload.html) {
        NSData* html_data = [NSData dataWithBytes:payload.html->data()
                                           length:static_cast<NSUInteger>(payload.html->size())];
        if (html_data)
          wrote = [pasteboard_item setData:html_data forType:NSPasteboardTypeHTML] || wrote;
      }
      if (payload.image && !payload.image->data.empty()) {
        NSImage* image = fxe_macos_create_ns_image_from_rgba(
            payload.image->data.data(), static_cast<int>(payload.image->width),
            static_cast<int>(payload.image->height));
        NSData* image_data = image ? [image TIFFRepresentation] : nil;
        if (image_data)
          wrote = [pasteboard_item setData:image_data forType:NSPasteboardTypeTIFF] || wrote;
      }
      if (wrote) {
        NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:pasteboard_item];
        [item setDraggingFrame:frame contents:icon];
        [items addObject:item];
      }
    }

    if (items.count == 0)
      return false;
    [view beginDraggingSessionWithItems:items event:event source:(id<NSDraggingSource>)view];
    return true;
  }

  static void fxe_macos_set_drag_rects(void* wrap_view_ptr, const std::vector<math::ivec4>& rects) {
    if (!wrap_view_ptr)
      return;
    FxeContentView* wrap = (__bridge FxeContentView*)wrap_view_ptr;
    NSMutableArray<NSValue*>* arr = [NSMutableArray arrayWithCapacity:rects.size()];
    for (const auto& r : rects) {
      NSRect nr = NSMakeRect(CGFloat(r.x), CGFloat(r.y), CGFloat(r.z), CGFloat(r.w));
      [arr addObject:[NSValue valueWithRect:nr]];
    }
    [wrap fxe_setDragRects:arr];
  }

  static void fxe_macos_release_drag_view(void* wrap_view_ptr) {
    if (!wrap_view_ptr)
      return;
    // Rebalance the +1 retain from __bridge_retained.
    FxeContentView* wrap = (__bridge_transfer FxeContentView*)wrap_view_ptr;
    (void)wrap;
  }
} // namespace fxe
#endif // __APPLE__

namespace fxe {
#if FXE_HAS_GLFW

  static void warn_once(bool& warned, const char* message) {
    if (!warned) {
      warned = true;
      FXE_WARN("window", "{}", message);
    }
  }

  // ----------------------------------------------------------------------------
  // GLFW init/terminate refcount. Multi-window phase 3 will create several
  // windows; only the last window destruction should tear down GLFW.
  // ----------------------------------------------------------------------------
  static std::atomic<int>& glfw_window_refcount() {
    static std::atomic<int> rc{0};
    return rc;
  }
  static void glfw_acquire() {
    if (glfw_window_refcount().fetch_add(1, std::memory_order_acq_rel) == 0) {
      if (!glfwInit())
        throw std::runtime_error("glfwInit failed");
    }
  }
  static void glfw_release() {
    if (glfw_window_refcount().fetch_sub(1, std::memory_order_acq_rel) == 1) {
      glfwTerminate();
    }
  }

  // ----------------------------------------------------------------------------
  // Monitor helpers (free functions in fxe::).
  // ----------------------------------------------------------------------------
  struct monitor_change_observer_state {
    std::mutex mutex;
    std::function<void()> callback;
    std::atomic<bool> pending{false};
    bool glfw_callback_installed = false;
  };

  monitor_change_observer_state& monitor_change_state() {
    static monitor_change_observer_state state;
    return state;
  }

  static void dispatch_monitor_change_observer() {
    auto& state = monitor_change_state();
    state.pending.store(false, std::memory_order_release);
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      callback = state.callback;
    }
    if (callback)
      callback();
  }

  static void glfw_monitor_change_callback(GLFWmonitor* /*monitor*/, int /*event*/) {
    auto& state = monitor_change_state();
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      callback = state.callback;
    }
    if (!callback)
      return;
    if (state.pending.exchange(true, std::memory_order_acq_rel))
      return;
    fxe::os::post_main_thread_dispatch([] { dispatch_monitor_change_observer(); });
  }

  static monitor_info make_monitor_info(GLFWmonitor* m, GLFWmonitor* primary) {
    monitor_info out{};
    if (!m)
      return out;
    if (const char* n = glfwGetMonitorName(m); n)
      out.name = n;
    glfwGetMonitorPos(m, &out.x, &out.y);
    glfwGetMonitorWorkarea(m, &out.work_x, &out.work_y, &out.work_width, &out.work_height);
    glfwGetMonitorContentScale(m, &out.scale_x, &out.scale_y);
    if (const GLFWvidmode* mode = glfwGetVideoMode(m); mode) {
      out.width = mode->width;
      out.height = mode->height;
      out.refresh_hz = mode->refreshRate;
    }
    out.primary = (m == primary);
    return out;
  }

  std::vector<monitor_info> list_monitors() {
    std::vector<monitor_info> out;
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    if (!mons || count <= 0)
      return out;
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    out.reserve(static_cast<usize>(count));
    for (int i = 0; i < count; ++i)
      out.push_back(make_monitor_info(mons[i], primary));
    return out;
  }

  monitor_info primary_monitor() {
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    return make_monitor_info(m, m);
  }

  void install_monitor_change_observer(std::function<void()> cb) {
    auto& state = monitor_change_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callback = std::move(cb);
    state.pending.store(false, std::memory_order_release);
    if (state.glfw_callback_installed)
      return;
    glfw_acquire();
    glfwSetMonitorCallback(glfw_monitor_change_callback);
    state.glfw_callback_installed = true;
  }

  void uninstall_monitor_change_observer() {
    auto& state = monitor_change_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callback = {};
    state.pending.store(false, std::memory_order_release);
    if (!state.glfw_callback_installed)
      return;
    glfwSetMonitorCallback(nullptr);
    state.glfw_callback_installed = false;
    glfw_release();
  }

  // ----------------------------------------------------------------------------
  // glfw_window
  // ----------------------------------------------------------------------------
  class glfw_window final : public window {
  public:
    explicit glfw_window(const window_desc& desc) : title_(desc.title) {
      glfw_acquire();
#if defined(__APPLE__)
      fxe_macos_activate_app();
#endif
      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
      glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
      glfwWindowHint(GLFW_DECORATED, desc.decorated ? GLFW_TRUE : GLFW_FALSE);
      glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, desc.transparent ? GLFW_TRUE : GLFW_FALSE);
      glfwWindowHint(GLFW_FLOATING, desc.always_on_top ? GLFW_TRUE : GLFW_FALSE);
      glfwWindowHint(GLFW_MAXIMIZED, desc.maximized ? GLFW_TRUE : GLFW_FALSE);

      handle_ =
          glfwCreateWindow(int(desc.width), int(desc.height), title_.c_str(), nullptr, nullptr);
      if (!handle_) {
        glfw_release();
        throw std::runtime_error("glfwCreateWindow failed");
      }

      glfwSetWindowUserPointer(handle_, this);

      // Apply size limits & position.
      min_size_limit_ =
          (desc.min_width > 0 || desc.min_height > 0)
              ? std::optional<math::ivec2>{{desc.min_width > 0 ? desc.min_width : 0,
                                            desc.min_height > 0 ? desc.min_height : 0}}
              : std::nullopt;
      max_size_limit_ =
          (desc.max_width > 0 || desc.max_height > 0)
              ? std::optional<math::ivec2>{{desc.max_width > 0 ? desc.max_width : 0,
                                            desc.max_height > 0 ? desc.max_height : 0}}
              : std::nullopt;
      apply_size_limits();
      if (desc.x != INT_MIN && desc.y != INT_MIN) {
        glfwSetWindowPos(handle_, desc.x, desc.y);
      } else {
        // Debug nudge for macOS unbundled-binary placement quirks.
        glfwSetWindowPos(handle_, 100, 100);
      }

      install_callbacks();

      redraw_requested_.store(true, std::memory_order_release);
      transparent_ = desc.transparent;
      decorated_ = desc.decorated;

      if (desc.visible) {
        glfwShowWindow(handle_);
        glfwFocusWindow(handle_);
#if defined(__APPLE__)
        fxe_macos_raise_window((__bridge void*)glfwGetCocoaWindow(handle_));
#endif
      }

#if defined(__APPLE__)
      // Wrap the GLFW content view so set_drag_region() can hit-test even
      // for decorated opaque windows; this also lets the metal layer attach
      // to our wrapper and survives toggling decorations.
      {
        void* ns = (__bridge void*)glfwGetCocoaWindow(handle_);
        fxe_macos_install_ime_bridge(
            ns, this, [](void* owner, const char* preedit, int cursor, const char* committed) {
              static_cast<glfw_window*>(owner)->push_compose_event(preedit, cursor, committed);
            });
        if (transparent_)
          fxe_macos_apply_transparent(ns);
        if (!decorated_)
          fxe_macos_set_movable_by_background(ns, true);
        wrap_view_ = fxe_macos_install_drag_view(ns);
        fxe_macos_install_drag_destination(wrap_view_, this,
                                           [](void* owner, input_event::kind_t kind, double x,
                                              double y, const std::vector<std::string>& paths) {
                                             static_cast<glfw_window*>(owner)->push_drag_event(
                                                 kind, x, y, paths);
                                           });
        install_macos_gesture_hooks(
            wrap_view_ ? wrap_view_ : (__bridge void*)[glfwGetCocoaWindow(handle_) contentView],
            this);
        if (!pending_drag_rects_.empty()) {
          fxe_macos_set_drag_rects(wrap_view_, pending_drag_rects_);
          pending_drag_rects_.clear();
        }
      }
#elif defined(_WIN32)
      {
        HWND hwnd = glfwGetWin32Window(handle_);
        fxe::os::install_win32_ime_bridge(
            hwnd, this, [](void* owner, const char* p, int c, const char* k) {
              static_cast<glfw_window*>(owner)->push_compose_event(p, c, k);
            });
        install_win32_pointer_hooks(hwnd, this);
      }
      install_win32_drop_target();
#elif defined(__linux__) && FXE_OS_DBUS
      {
        (void)fxe::os::install_linux_ime_bridge(
            this, [](void* owner, const char* p, int c, const char* k) {
              static_cast<glfw_window*>(owner)->push_compose_event(p, c, k);
            });
      }
#else
      // GLFW/X11 and GLFW/Wayland expose file paths only at Xdnd drop time; they do not
      // surface native drag enter/position/leave events for us to forward.
#endif
    }

    ~glfw_window() override {
      for (auto*& c : cursors_) {
        if (c) {
          glfwDestroyCursor(c);
          c = nullptr;
        }
      }
      if (custom_cursor_) {
        glfwDestroyCursor(custom_cursor_);
        custom_cursor_ = nullptr;
      }
#if defined(_WIN32)
      uninstall_win32_drop_target();
      clear_win32_icon();
      uninstall_win32_subclass();
#endif
#if defined(__APPLE__)
      if (visual_effect_view_) {
        fxe_macos_release_visual_effect(visual_effect_view_);
        visual_effect_view_ = nullptr;
      }
      if (wrap_view_) {
        fxe_macos_release_drag_view(wrap_view_);
        wrap_view_ = nullptr;
      }
#endif
      if (handle_)
        glfwDestroyWindow(handle_);
      handle_ = nullptr;
      glfw_release();
    }

    void poll() override {
      glfwPollEvents();
    }
    void wait_events() override {
      glfwWaitEvents();
    }
    void wait_events_timeout(double seconds) override {
      if (seconds <= 0.0)
        glfwPollEvents();
      else
        glfwWaitEventsTimeout(seconds);
    }
    void post_redraw() override {
      redraw_requested_.store(true, std::memory_order_release);
      glfwPostEmptyEvent();
    }
    void post_message(std::string channel, std::vector<std::vector<u8>> args) override {
      input_event ev{};
      ev.kind = input_event::kind_t::message;
      ev.message_channel = std::move(channel);
      ev.message_args_serialised = std::move(args);
      push_event(std::move(ev));
    }
    bool take_redraw_request() override {
      return redraw_requested_.exchange(false, std::memory_order_acq_rel);
    }
    bool peek_redraw_request() const override {
      return redraw_requested_.load(std::memory_order_acquire);
    }
    void set_redraw_handler(redraw_handler handler) override {
      redraw_handler_ = std::move(handler);
    }
    std::vector<input_event> drain_input_events() override {
      std::lock_guard<std::mutex> lock(input_mutex_);
      std::vector<input_event> out;
      out.swap(injected_events_);
      return out;
    }
    void inject_gesture_event(input_event ev) {
      push_event(std::move(ev));
    }
    void close() override {
      glfwSetWindowShouldClose(handle_, GLFW_TRUE);
    }
    bool should_close() const override {
      return glfwWindowShouldClose(handle_) != 0;
    }
    math::uvec2 framebuffer_size() const override {
      int w = 0, h = 0;
      glfwGetFramebufferSize(handle_, &w, &h);
      return {w, h};
    }
    void set_dpi_scale_override(std::optional<float> scale) override {
      if (scale && std::isfinite(*scale) && *scale > 0.0f) {
        dpi_scale_override_ = *scale;
      } else {
        dpi_scale_override_.reset();
      }
    }
    [[nodiscard]] float dpi_scale() const override {
      if (dpi_scale_override_)
        return *dpi_scale_override_;
      float scale_x = 1.0f;
      float scale_y = 1.0f;
      glfwGetWindowContentScale(handle_, &scale_x, &scale_y);
      const float scale = scale_x > scale_y ? scale_x : scale_y;
      return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }
    [[nodiscard]] bool has_dpi_scale_override() const override {
      return dpi_scale_override_.has_value();
    }
    void set_vsync(bool enabled) override {
#if defined(__APPLE__) && FXE_HAS_WGPU
      if (metal_layer_) {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer_;
        layer.displaySyncEnabled = enabled ? YES : NO;
      }
#else
      (void)enabled;
#endif
    }

    void* native_handle() const override {
#if defined(__APPLE__)
      return (__bridge void*)glfwGetCocoaWindow(handle_);
#elif defined(_WIN32)
      return reinterpret_cast<void*>(glfwGetWin32Window(handle_));
#else
#if defined(GLFW_PLATFORM_WAYLAND)
      if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return reinterpret_cast<void*>(glfwGetWaylandWindow(handle_));
      }
#endif
      return reinterpret_cast<void*>(glfwGetX11Window(handle_));
#endif
    }

    GLFWwindow* native() const noexcept {
      return handle_;
    }

#if defined(__APPLE__) && FXE_HAS_WGPU
    // Cache the CAMetalLayer* created in make_wgpu_surface so we can update its
    // backgroundColor (the colour shown by Core Animation for any layer pixels
    // not yet covered by a freshly presented frame, which is what causes a
    // brief flash during live resize).
    void set_metal_layer_handle(void* layer) noexcept {
      metal_layer_ = layer;
    }
    void* metal_layer_handle() const noexcept {
      return metal_layer_;
    }
    void set_surface_background_color(float r, float g, float b, float a) override {
      if (!metal_layer_)
        return;
      CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer_;
      CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      CGFloat comps[4] = {static_cast<CGFloat>(r), static_cast<CGFloat>(g), static_cast<CGFloat>(b),
                          static_cast<CGFloat>(a)};
      CGColorRef col = CGColorCreate(cs, comps);
      // The bounds-change animation has zero duration on Metal layers, but the
      // backgroundColor change itself can be implicitly animated. Wrap in a
      // CATransaction with actions disabled so the new colour is visible
      // synchronously, before the next compositor commit.
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      layer.backgroundColor = col;
      [CATransaction commit];
      CGColorRelease(col);
      CGColorSpaceRelease(cs);
    }
#endif

    // -------- new API --------
    void set_title(std::string_view t) override {
      title_ = std::string(t);
      glfwSetWindowTitle(handle_, title_.c_str());
    }
    std::string get_title() const override {
      return title_;
    }
    std::string title() const override {
      return get_title();
    }
    void set_size(int w, int h) override {
      glfwSetWindowSize(handle_, w, h);
    }
    void set_position(int x, int y) override {
      glfwSetWindowPos(handle_, x, y);
    }
    math::ivec2 position() const override {
      int x = 0, y = 0;
      glfwGetWindowPos(handle_, &x, &y);
      return {x, y};
    }
    math::uvec2 content_size() const override {
      int w = 0, h = 0;
      glfwGetWindowSize(handle_, &w, &h);
      return {w, h};
    }
    math::ivec4 get_bounds() const override {
      const auto p = position();
      const auto sz = content_size();
      return {p.x, p.y, static_cast<int>(sz.x), static_cast<int>(sz.y)};
    }
    void set_min_size(int w, int h) override {
      min_size_limit_ = (w > 0 || h > 0)
                            ? std::optional<math::ivec2>{{w > 0 ? w : 0, h > 0 ? h : 0}}
                            : std::nullopt;
      apply_size_limits();
    }
    void set_max_size(int w, int h) override {
      max_size_limit_ = (w > 0 || h > 0)
                            ? std::optional<math::ivec2>{{w > 0 ? w : 0, h > 0 ? h : 0}}
                            : std::nullopt;
      apply_size_limits();
    }
    std::optional<math::ivec2> min_size() const override {
      return min_size_limit_;
    }
    std::optional<math::ivec2> max_size() const override {
      return max_size_limit_;
    }
    std::optional<math::ivec2> get_min_size() const override {
      return min_size_limit_;
    }
    std::optional<math::ivec2> get_max_size() const override {
      return max_size_limit_;
    }
    void set_opacity(float a) override {
      glfwSetWindowOpacity(handle_, a);
    }
    float opacity() const override {
      return glfwGetWindowOpacity(handle_);
    }
    void set_always_on_top(bool on) override {
      glfwSetWindowAttrib(handle_, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE);
    }
    bool is_always_on_top() const override {
      return glfwGetWindowAttrib(handle_, GLFW_FLOATING) == GLFW_TRUE;
    }
    void set_resizable(bool on) override {
      glfwSetWindowAttrib(handle_, GLFW_RESIZABLE, on ? GLFW_TRUE : GLFW_FALSE);
    }
    bool is_resizable_actual() const override {
      return glfwGetWindowAttrib(handle_, GLFW_RESIZABLE) == GLFW_TRUE;
    }
    bool is_resizable() const override {
      return is_resizable_actual();
    }
    void set_decorated(bool on) override {
      decorated_ = on;
      glfwSetWindowAttrib(handle_, GLFW_DECORATED, on ? GLFW_TRUE : GLFW_FALSE);
#if defined(__APPLE__)
      fxe_macos_set_movable_by_background((__bridge void*)glfwGetCocoaWindow(handle_), !on);
#endif
    }
    bool is_decorated() const override {
      return glfwGetWindowAttrib(handle_, GLFW_DECORATED) == GLFW_TRUE;
    }
    void set_title_bar_style(title_bar_style style) override {
      const bool native_decorated = (style == title_bar_style::default_);
      set_decorated(native_decorated);
#if defined(__APPLE__)
      if (style == title_bar_style::hidden_inset || style == title_bar_style::custom_buttons) {
        warn_once(warned_title_bar_style_, "fxe.window: title-bar traffic-light layout is not "
                                           "implemented; using frameless GLFW window");
      }
#else
      if (style == title_bar_style::hidden_inset || style == title_bar_style::custom_buttons) {
        warn_once(warned_title_bar_style_, "fxe.window: platform title-bar button layout is not "
                                           "implemented; using frameless GLFW window");
      }
#endif
    }
    bool set_traffic_light_position(int x, int y) override {
#if defined(__APPLE__)
      fxe_macos_set_traffic_light_position((__bridge void*)glfwGetCocoaWindow(handle_), x, y);
      return true;
#elif defined(_WIN32)
      caption_button_offset_ = {x, y};
      if (handle_) {
        HWND hwnd = glfwGetWin32Window(handle_);
        if (hwnd) {
          SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
      }
      return true;
#else
      (void)x;
      (void)y;
      return false;
#endif
    }
    bool set_window_controls_overlay(bool enabled) override {
#if defined(_WIN32)
      window_controls_overlay_ = enabled;
      if (enabled)
        install_win32_subclass();
      else
        uninstall_win32_subclass();
      if (handle_) {
        HWND hwnd = glfwGetWin32Window(handle_);
        if (hwnd) {
          SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
      }
      return true;
#else
      if (enabled) {
        warn_once(warned_unsupported_window_controls_overlay_,
                  "fxe.window: setWindowControlsOverlay is unsupported on this GLFW backend");
      }
      return false;
#endif
    }
    bool set_vibrancy(const char* kind) override {
      vibrancy_kind_ = kind ? std::string(kind) : std::string{};
#if defined(__APPLE__)
      if (!handle_)
        return false;
      visual_effect_view_ = fxe_macos_set_visual_effect(
          (__bridge void*)glfwGetCocoaWindow(handle_), visual_effect_view_,
          vibrancy_kind_.empty() ? nullptr : vibrancy_kind_.c_str(), blur_behind_);
      return true;
#elif defined(_WIN32)
      if (!handle_)
        return false;
      apply_win32_vibrancy(vibrancy_kind_.empty() ? nullptr : vibrancy_kind_.c_str());
      return true;
#else
      if (kind) {
        warn_once(warned_unsupported_vibrancy_,
                  "fxe.window: setVibrancy is unsupported on this GLFW backend");
      }
      return false;
#endif
    }
    bool set_blur_behind(bool enabled) override {
      blur_behind_ = enabled;
#if defined(__APPLE__)
      if (!handle_)
        return false;
      visual_effect_view_ = fxe_macos_set_visual_effect(
          (__bridge void*)glfwGetCocoaWindow(handle_), visual_effect_view_,
          vibrancy_kind_.empty() ? nullptr : vibrancy_kind_.c_str(), blur_behind_);
      return true;
#elif defined(_WIN32)
      if (!handle_)
        return false;
      apply_win32_blur_behind(enabled);
      return true;
#else
      if (enabled) {
        warn_once(warned_unsupported_blur_behind_,
                  "fxe.window: setBlurBehind is unsupported on this GLFW backend");
      }
      return false;
#endif
    }
    [[nodiscard]] bool is_transparent() const override {
      return glfwGetWindowAttrib(handle_, GLFW_TRANSPARENT_FRAMEBUFFER) == GLFW_TRUE;
    }

    void set_drag_region(const std::vector<math::ivec4>& rects) override {
      drag_rects_ = rects;
#if defined(__APPLE__)
      if (wrap_view_) {
        fxe_macos_set_drag_rects(wrap_view_, rects);
      } else {
        pending_drag_rects_ = rects;
      }
#elif defined(_WIN32)
      install_win32_subclass();
#else
      // Stored for callers/backends that can query native drag regions later.
#endif
    }

    bool start_drag(const drag_payload& payload) override {
#if defined(__APPLE__)
      return fxe_macos_start_drag((__bridge void*)glfwGetCocoaWindow(handle_), payload);
#elif defined(_WIN32)
      return start_win32_drag(payload);
#else
      (void)payload;
      return false;
#endif
    }
    void set_visible(bool on) override {
      if (on)
        glfwShowWindow(handle_);
      else
        glfwHideWindow(handle_);
    }
    bool set_icon(const u8* rgba, int w, int h) override {
#if defined(__APPLE__)
      if (rgba && w > 0 && h > 0)
        fxe_macos_set_window_icon(rgba, w, h);
      return rgba && w > 0 && h > 0;
#else
      if (!rgba || w <= 0 || h <= 0) {
        glfwSetWindowIcon(handle_, 0, nullptr);
#if defined(_WIN32)
        clear_win32_icon();
#endif
        return true;
      }
      GLFWimage img{};
      img.width = w;
      img.height = h;
      img.pixels = const_cast<unsigned char*>(rgba);
      glfwSetWindowIcon(handle_, 1, &img);
#if defined(_WIN32)
      // GLFW consumes RGBA here and performs its native Win32 conversion. The fallback below
      // converts to BGRA only for the GDI HICON path used to refresh the taskbar/class icon.
      set_win32_icon_from_rgba(rgba, w, h);
#endif
      return true;
#endif
    }
    void minimize() override {
      glfwIconifyWindow(handle_);
    }
    void maximize() override {
      glfwMaximizeWindow(handle_);
    }
    void restore() override {
      glfwRestoreWindow(handle_);
    }
    void focus() override {
      glfwFocusWindow(handle_);
#if defined(__APPLE__)
      fxe_macos_raise_window((__bridge void*)glfwGetCocoaWindow(handle_));
#endif
    }
    void request_attention() override {
      glfwRequestWindowAttention(handle_);
    }
    void center() override {
      monitor_info pm = primary_monitor();
      int ww = 0, wh = 0;
      glfwGetWindowSize(handle_, &ww, &wh);
      int x = pm.work_x + (pm.work_width - ww) / 2;
      int y = pm.work_y + (pm.work_height - wh) / 2;
      glfwSetWindowPos(handle_, x, y);
    }
    bool is_focused() const override {
      return glfwGetWindowAttrib(handle_, GLFW_FOCUSED) != 0;
    }
    bool is_minimized() const override {
      return glfwGetWindowAttrib(handle_, GLFW_ICONIFIED) != 0;
    }
    bool is_maximized() const override {
      return glfwGetWindowAttrib(handle_, GLFW_MAXIMIZED) != 0;
    }
    bool is_visible() const override {
      return glfwGetWindowAttrib(handle_, GLFW_VISIBLE) != 0;
    }
    void set_fullscreen(bool on, fullscreen_mode mode = fullscreen_mode::borderless,
                        int monitor_index = -1) override {
      if (!on && !fullscreen_)
        return;
      if (on && fullscreen_) {
        if (mode == fullscreen_mode_ && monitor_index == fullscreen_monitor_index_)
          return;
        set_fullscreen(false);
      }
      if (on) {
        glfwGetWindowPos(handle_, &saved_x_, &saved_y_);
        glfwGetWindowSize(handle_, &saved_w_, &saved_h_);
        saved_decorated_ = glfwGetWindowAttrib(handle_, GLFW_DECORATED) == GLFW_TRUE;
        saved_floating_ = glfwGetWindowAttrib(handle_, GLFW_FLOATING) == GLFW_TRUE;
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        GLFWmonitor* m = nullptr;
        if (monitor_index >= 0 && mons && monitor_index < count)
          m = mons[monitor_index];
        else
          m = glfwGetPrimaryMonitor();
        if (!m)
          return;
        const GLFWvidmode* video_mode = glfwGetVideoMode(m);
        if (!video_mode)
          return;
        fullscreen_mode_ = mode;
        fullscreen_monitor_index_ = monitor_index;
        if (mode == fullscreen_mode::exclusive) {
          glfwSetWindowMonitor(handle_, m, 0, 0, video_mode->width, video_mode->height,
                               video_mode->refreshRate);
        } else {
          int x = 0;
          int y = 0;
          int work_width = 0;
          int work_height = 0;
          glfwGetMonitorWorkarea(m, &x, &y, &work_width, &work_height);
          (void)work_width;
          (void)work_height;
          glfwSetWindowMonitor(handle_, nullptr, x, y, video_mode->width, video_mode->height, 0);
          glfwSetWindowAttrib(handle_, GLFW_DECORATED, GLFW_FALSE);
          glfwSetWindowAttrib(handle_, GLFW_FLOATING, GLFW_TRUE);
#if defined(__APPLE__)
          fxe_macos_set_movable_by_background((__bridge void*)glfwGetCocoaWindow(handle_), true);
#endif
        }
        fullscreen_ = true;
        return;
      }
      int x = saved_x_;
      int y = saved_y_;
      int w = saved_w_;
      int h = saved_h_;
      if (w <= 0 || h <= 0) {
        w = 1280;
        h = 720;
        x = 100;
        y = 100;
      }
      glfwSetWindowMonitor(handle_, nullptr, x, y, w, h, 0);
      glfwSetWindowAttrib(handle_, GLFW_DECORATED, saved_decorated_ ? GLFW_TRUE : GLFW_FALSE);
      glfwSetWindowAttrib(handle_, GLFW_FLOATING, saved_floating_ ? GLFW_TRUE : GLFW_FALSE);
#if defined(__APPLE__)
      fxe_macos_set_movable_by_background((__bridge void*)glfwGetCocoaWindow(handle_),
                                          !saved_decorated_);
#endif
      decorated_ = saved_decorated_;
      fullscreen_ = false;
      fullscreen_mode_ = fullscreen_mode::borderless;
      fullscreen_monitor_index_ = -1;
    }
    bool is_fullscreen() const override {
      return fullscreen_;
    }
    void set_cursor(cursor_kind kind) override {
      auto idx = static_cast<usize>(kind);
      if (kind == cursor_kind::hidden) {
        glfwSetInputMode(handle_, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        return;
      }
      glfwSetInputMode(handle_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      if (!cursors_[idx])
        cursors_[idx] = create_cursor(kind);
      if (cursors_[idx])
        glfwSetCursor(handle_, cursors_[idx]);
      else
        glfwSetCursor(handle_, nullptr);
    }
    bool set_cursor_image(const u8* rgba, int w, int h, int hot_x, int hot_y) override {
      if (!rgba || w <= 0 || h <= 0)
        return false;
      if (hot_x < 0 || hot_x >= w || hot_y < 0 || hot_y >= h)
        return false;
      GLFWimage img{};
      img.width = w;
      img.height = h;
      img.pixels = const_cast<unsigned char*>(rgba);
      GLFWcursor* c = glfwCreateCursor(&img, hot_x, hot_y);
      if (!c)
        return false;
      if (custom_cursor_)
        glfwDestroyCursor(custom_cursor_);
      custom_cursor_ = c;
      glfwSetInputMode(handle_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      glfwSetCursor(handle_, custom_cursor_);
      return true;
    }
    void clear_cursor_image() override {
      if (custom_cursor_) {
        glfwDestroyCursor(custom_cursor_);
        custom_cursor_ = nullptr;
      }
      glfwSetCursor(handle_, nullptr);
    }
    void set_cursor_visible(bool on) override {
      glfwSetInputMode(handle_, GLFW_CURSOR, on ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
    }
    void set_cursor_pos(double x, double y) override {
      glfwSetCursorPos(handle_, x, y);
    }
    math::dvec2 cursor_pos() const override {
      double x = 0, y = 0;
      glfwGetCursorPos(handle_, &x, &y);
      return {x, y};
    }
    void set_cursor_lock(bool on) override {
      glfwSetInputMode(handle_, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
    void set_raw_mouse_motion(bool enabled) override {
      if (glfwRawMouseMotionSupported())
        glfwSetInputMode(handle_, GLFW_RAW_MOUSE_MOTION, enabled ? GLFW_TRUE : GLFW_FALSE);
    }
    [[nodiscard]] bool is_raw_mouse_motion_supported() const override {
      return glfwRawMouseMotionSupported() == GLFW_TRUE;
    }
    bool set_content_protection(bool enabled) override {
#if defined(__APPLE__)
      const bool applied = handle_ && fxe_macos_set_content_protection(
                                          (__bridge void*)glfwGetCocoaWindow(handle_), enabled);
#elif defined(_WIN32)
      const bool applied =
          handle_ && fxe_win32_set_content_protection(glfwGetWin32Window(handle_), enabled);
#else
      static std::once_flag warned_once;
      std::call_once(warned_once, [] {
        FXE_WARN("window.linux", "setContentProtection unsupported on this platform");
      });
      (void)enabled;
      const bool applied = false;
#endif
      if (!applied)
        return false;
      content_protection_ = enabled;
      return true;
    }
    [[nodiscard]] bool is_content_protection_enabled() const override {
      return content_protection_;
    }
    std::string clipboard_text() const override {
      const char* s = glfwGetClipboardString(handle_);
      return s ? std::string(s) : std::string{};
    }
    void set_clipboard_text(std::string_view t) override {
      std::string z(t);
      glfwSetClipboardString(handle_, z.c_str());
    }

    bool read_clipboard_image(clipboard_image& out) const override {
#if defined(__APPLE__)
      return fxe_macos_read_clipboard_image(out);
#else
      (void)out;
      return false;
#endif
    }
    bool set_clipboard_image(const clipboard_image& image) override {
#if defined(__APPLE__)
      return fxe_macos_write_clipboard_image(image);
#else
      (void)image;
      return false;
#endif
    }

    std::optional<std::string> clipboard_html() const override {
      return os::clipboard_get_html();
    }
    bool set_clipboard_html(std::string_view html) override {
      return os::clipboard_set_html(html);
    }
    std::optional<std::string> clipboard_rtf() const override {
      return os::clipboard_get_rtf();
    }
    bool set_clipboard_rtf(std::string_view rtf) override {
      return os::clipboard_set_rtf(rtf);
    }
    std::optional<std::vector<u8>> clipboard_mime(std::string_view mime) const override {
      return os::clipboard_get_mime(mime);
    }
    bool set_clipboard_mime(std::string_view mime, const std::vector<u8>& bytes) override {
      return os::clipboard_set_mime(mime, bytes);
    }

  private:
#if defined(_WIN32)
    enum class fxe_win32_accent_state : int {
      disabled = 0,
      enable_gradient = 1,
      enable_transparent_gradient = 2,
      enable_blur_behind = 3,
      enable_acrylic_blur_behind = 4,
      enable_host_backdrop = 5,
    };

    struct fxe_win32_accent_policy {
      int accent_state = 0;
      int accent_flags = 0;
      DWORD gradient_color = 0;
      int animation_id = 0;
    };

    struct fxe_win32_window_composition_attribute_data {
      DWORD attribute = 0;
      PVOID data = nullptr;
      SIZE_T size_of_data = 0;
    };

    using fxe_set_window_composition_attribute =
        BOOL(WINAPI*)(HWND, fxe_win32_window_composition_attribute_data*);

    static DWORD win32_backdrop_type_for_kind(const char* kind) {
      if (!kind || kind[0] == '\0')
        return DWMSBT_NONE;
      if (std::strcmp(kind, "sidebar") == 0)
        return DWMSBT_MAINWINDOW;
      if (std::strcmp(kind, "titlebar") == 0)
        return DWMSBT_TRANSIENTWINDOW;
      if (std::strcmp(kind, "menu") == 0)
        return DWMSBT_TABBEDWINDOW;
      return DWMSBT_MAINWINDOW;
    }

    static bool win32_set_accent_policy(HWND hwnd, const char* kind) {
      HMODULE user32 = GetModuleHandleW(L"user32");
      if (!user32)
        return false;
      auto set_window_composition_attribute =
          reinterpret_cast<fxe_set_window_composition_attribute>(
              GetProcAddress(user32, "SetWindowCompositionAttribute"));
      if (!set_window_composition_attribute)
        return false;

      const bool enabled = kind && kind[0] != '\0';
      fxe_win32_accent_policy accent{};
      if (enabled) {
        accent.accent_state = static_cast<int>(
            std::strcmp(kind, "menu") == 0 ? fxe_win32_accent_state::enable_acrylic_blur_behind
                                           : fxe_win32_accent_state::enable_blur_behind);
        accent.gradient_color = 0x99000000;
      } else {
        accent.accent_state = static_cast<int>(fxe_win32_accent_state::disabled);
      }

      fxe_win32_window_composition_attribute_data data{};
      data.attribute = 19; // WCA_ACCENT_POLICY
      data.data = &accent;
      data.size_of_data = sizeof(accent);
      return set_window_composition_attribute(hwnd, &data) != FALSE;
    }
    static bool fxe_win32_set_content_protection(HWND hwnd, bool enabled) {
      if (!hwnd)
        return false;
      return SetWindowDisplayAffinity(hwnd, enabled ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != 0;
    }

    static HICON win32_create_hicon_from_rgba(const u8* rgba, int width, int height) {
      if (!rgba || width <= 0 || height <= 0)
        return nullptr;
      const usize w = static_cast<usize>(width);
      const usize h = static_cast<usize>(height);
      if (w > (SIZE_MAX / h) / 4u)
        return nullptr;
      if (width > std::numeric_limits<LONG>::max() || height > std::numeric_limits<LONG>::max())
        return nullptr;

      BITMAPV5HEADER bi{};
      bi.bV5Size = sizeof(BITMAPV5HEADER);
      bi.bV5Width = static_cast<LONG>(width);
      bi.bV5Height = -static_cast<LONG>(height);
      bi.bV5Planes = 1;
      bi.bV5BitCount = 32;
      bi.bV5Compression = BI_BITFIELDS;
      bi.bV5RedMask = 0x00ff0000;
      bi.bV5GreenMask = 0x0000ff00;
      bi.bV5BlueMask = 0x000000ff;
      bi.bV5AlphaMask = 0xff000000;

      void* bits = nullptr;
      HDC dc = GetDC(nullptr);
      HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                       &bits, nullptr, 0);
      if (dc)
        ReleaseDC(nullptr, dc);
      if (!color || !bits) {
        if (color)
          DeleteObject(color);
        return nullptr;
      }

      auto* bgra = static_cast<u8*>(bits);
      for (usize i = 0; i < w * h; ++i) {
        bgra[i * 4u + 0u] = rgba[i * 4u + 2u];
        bgra[i * 4u + 1u] = rgba[i * 4u + 1u];
        bgra[i * 4u + 2u] = rgba[i * 4u + 0u];
        bgra[i * 4u + 3u] = rgba[i * 4u + 3u];
      }

      const usize mask_stride = ((w + 15u) / 16u) * 2u;
      std::vector<u8> mask_bits(mask_stride * h, 0);
      HBITMAP mask = CreateBitmap(width, height, 1, 1, mask_bits.data());
      if (!mask) {
        DeleteObject(color);
        return nullptr;
      }

      ICONINFO ii{};
      ii.fIcon = TRUE;
      ii.hbmMask = mask;
      ii.hbmColor = color;
      HICON icon = CreateIconIndirect(&ii);
      DeleteObject(mask);
      DeleteObject(color);
      return icon;
    }

    static LRESULT CALLBACK win32_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                UINT_PTR subclass_id, DWORD_PTR ref_data) {
      (void)subclass_id;
      auto* self = reinterpret_cast<glfw_window*>(ref_data);
      if (!self)
        return DefSubclassProc(hwnd, msg, wp, lp);

      switch (msg) {
      case WM_NCCALCSIZE:
        if (self->window_controls_overlay_)
          return 0;
        break;
      case WM_NCHITTEST:
        return self->win32_hit_test(hwnd, wp, lp);
      case WM_NCACTIVATE:
        if (self->window_controls_overlay_)
          return TRUE;
        break;
      case WM_DESTROY: {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        self->win32_subclassed_ = false;
        self->win32_subclass_id_ = 0;
        RemoveWindowSubclass(hwnd, &win32_subclass_proc, subclass_id);
        return result;
      }
      default:
        break;
      }
      return DefSubclassProc(hwnd, msg, wp, lp);
    }

    LRESULT win32_hit_test(HWND hwnd, WPARAM wp, LPARAM lp) const {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (!ScreenToClient(hwnd, &pt))
        return DefSubclassProc(hwnd, WM_NCHITTEST, wp, lp);

      if (window_controls_overlay_ && win32_point_in_caption_buttons(hwnd, pt))
        return HTCLIENT;

      for (const auto& r : drag_rects_) {
        if (pt.x >= r.x && pt.x < r.x + r.z && pt.y >= r.y && pt.y < r.y + r.w)
          return HTCAPTION;
      }

      if (window_controls_overlay_)
        return HTCLIENT;
      return DefSubclassProc(hwnd, WM_NCHITTEST, wp, lp);
    }

    bool win32_point_in_caption_buttons(HWND hwnd, POINT pt) const {
      RECT client{};
      if (!GetClientRect(hwnd, &client))
        return false;
      const int button_w = GetSystemMetrics(SM_CXSIZE);
      const int button_h = GetSystemMetrics(SM_CYSIZE);
      const int group_w = button_w * 3;
      const int left = client.right - group_w - caption_button_offset_.x;
      const int top = caption_button_offset_.y;
      return pt.x >= left && pt.x < left + group_w && pt.y >= top && pt.y < top + button_h;
    }

    void install_win32_subclass() {
      if (win32_subclassed_)
        return;
      HWND hwnd = glfwGetWin32Window(handle_);
      if (!hwnd)
        return;
      win32_subclass_id_ = reinterpret_cast<UINT_PTR>(this);
      if (SetWindowSubclass(hwnd, &win32_subclass_proc, win32_subclass_id_,
                            reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
        win32_subclass_id_ = 0;
        warn_once(warned_win32_subclass_, "fxe.window: SetWindowSubclass failed");
        return;
      }
      win32_subclassed_ = true;
    }

    void uninstall_win32_subclass() {
      if (!win32_subclassed_)
        return;
      HWND hwnd = handle_ ? glfwGetWin32Window(handle_) : nullptr;
      if (hwnd)
        RemoveWindowSubclass(hwnd, &win32_subclass_proc, win32_subclass_id_);
      win32_subclassed_ = false;
      win32_subclass_id_ = 0;
    }

    void apply_win32_blur_behind(bool enabled) {
      HWND hwnd = glfwGetWin32Window(handle_);
      if (!hwnd)
        return;

      RECT client{};
      HRGN region = nullptr;
      if (enabled && GetClientRect(hwnd, &client))
        region = CreateRectRgn(client.left, client.top, client.right, client.bottom);

      DWM_BLURBEHIND blur{};
      blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
      blur.fEnable = enabled ? TRUE : FALSE;
      blur.hRgnBlur = enabled ? region : nullptr;
      HRESULT hr = DwmEnableBlurBehindWindow(hwnd, &blur);
      if (region)
        DeleteObject(region);
      if (FAILED(hr) && enabled) {
        warn_once(warned_win32_blur_, "fxe.window: DwmEnableBlurBehindWindow failed");
      }
    }

    void apply_win32_vibrancy(const char* kind) {
      HWND hwnd = glfwGetWin32Window(handle_);
      if (!hwnd)
        return;

      const bool enabled = kind && kind[0] != '\0';
      if (enabled && transparent_) {
        warn_once(warned_win32_transparent_vibrancy_,
                  "fxe.window: Win32 vibrancy requested on a transparent window; using accent "
                  "fallback when available");
      }

      BOOL dark = TRUE;
      (void)DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

      bool applied = false;
      if (!transparent_) {
        DWORD backdrop = win32_backdrop_type_for_kind(kind);
        HRESULT hr =
            DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
        applied = SUCCEEDED(hr);
      }

      if (applied) {
        (void)win32_set_accent_policy(hwnd, nullptr);
        return;
      }

      if (win32_set_accent_policy(hwnd, kind))
        return;

      if (enabled) {
        warn_once(warned_win32_vibrancy_,
                  "fxe.window: Win32 vibrancy is unavailable on this Windows version");
      }
    }

    static std::wstring win32_utf8_to_wide(const std::string& s) {
      if (s.empty())
        return {};
      int count = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
      if (count <= 0)
        return {};
      std::wstring out(static_cast<usize>(count), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), count);
      return out;
    }

    static std::string win32_wide_to_utf8(const wchar_t* value) {
      if (!value || value[0] == L'\0')
        return {};
      int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
      if (count <= 1)
        return {};
      std::string out(static_cast<usize>(count - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), count, nullptr, nullptr);
      return out;
    }

    static std::vector<std::string> win32_paths_from_data_object(IDataObject* data) {
      std::vector<std::string> paths;
      if (!data)
        return paths;
      FORMATETC format{};
      format.cfFormat = CF_HDROP;
      format.dwAspect = DVASPECT_CONTENT;
      format.lindex = -1;
      format.tymed = TYMED_HGLOBAL;
      STGMEDIUM medium{};
      if (FAILED(data->GetData(&format, &medium)))
        return paths;
      if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
        HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        paths.reserve(static_cast<usize>(count));
        for (UINT i = 0; i < count; ++i) {
          const UINT chars = DragQueryFileW(drop, i, nullptr, 0);
          if (chars == 0)
            continue;
          std::wstring wide(static_cast<usize>(chars + 1), L'\0');
          if (DragQueryFileW(drop, i, wide.data(), chars + 1) == 0)
            continue;
          auto utf8 = win32_wide_to_utf8(wide.c_str());
          if (!utf8.empty())
            paths.push_back(std::move(utf8));
        }
      }
      ReleaseStgMedium(&medium);
      return paths;
    }

    static HGLOBAL win32_hglobal_from_wstring(const std::wstring& text) {
      const usize bytes = (text.size() + 1u) * sizeof(wchar_t);
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (!h)
        return nullptr;
      void* dst = GlobalLock(h);
      if (!dst) {
        GlobalFree(h);
        return nullptr;
      }
      std::memcpy(dst, text.c_str(), bytes);
      GlobalUnlock(h);
      return h;
    }

    static HGLOBAL win32_hglobal_from_bytes(const void* data, usize bytes, bool nul_terminate) {
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes + (nul_terminate ? 1u : 0u));
      if (!h)
        return nullptr;
      void* dst = GlobalLock(h);
      if (!dst) {
        GlobalFree(h);
        return nullptr;
      }
      if (bytes > 0 && data)
        std::memcpy(dst, data, bytes);
      if (nul_terminate)
        static_cast<u8*>(dst)[bytes] = 0;
      GlobalUnlock(h);
      return h;
    }

    static HGLOBAL win32_hglobal_from_string(std::string_view text) {
      return win32_hglobal_from_bytes(text.data(), text.size(), true);
    }

    static std::string win32_cf_html_from_fragment(std::string_view html) {
      constexpr std::string_view header = "Version:1.0\r\n"
                                          "StartHTML:0000000000\r\n"
                                          "EndHTML:0000000000\r\n"
                                          "StartFragment:0000000000\r\n"
                                          "EndFragment:0000000000\r\n";
      constexpr std::string_view start_marker = "<!--StartFragment-->";
      constexpr std::string_view end_marker = "<!--EndFragment-->";
      std::string out;
      out.reserve(header.size() + start_marker.size() + html.size() + end_marker.size());
      out.append(header);
      const usize start_html = out.size();
      out.append(start_marker);
      const usize start_fragment = out.size();
      out.append(html);
      const usize end_fragment = out.size();
      out.append(end_marker);
      const usize end_html = out.size();
      auto write = [&](const char* key, usize value) {
        usize pos = out.find(key);
        if (pos == std::string::npos)
          return;
        pos += std::strlen(key);
        char buf[11] = {};
        std::snprintf(buf, sizeof(buf), "%010zu", value);
        out.replace(pos, 10, buf, 10);
      };
      write("StartHTML:", start_html);
      write("EndHTML:", end_html);
      write("StartFragment:", start_fragment);
      write("EndFragment:", end_fragment);
      return out;
    }

    static HGLOBAL win32_hglobal_from_cf_html(std::string_view html) {
      auto cf_html = win32_cf_html_from_fragment(html);
      return win32_hglobal_from_string(cf_html);
    }

    static HGLOBAL win32_cf_dib_from_image(const image_data& image) {
      const usize width = image.width;
      const usize height = image.height;
      if (width == 0 || height == 0 || width > static_cast<usize>(INT32_MAX) ||
          height > static_cast<usize>(INT32_MAX) || width > (SIZE_MAX / height) / 4u ||
          image.data.size() < width * height * 4u)
        return nullptr;
      const usize pixel_bytes = width * height * 4u;
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixel_bytes);
      if (!h)
        return nullptr;
      auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(h));
      if (!header) {
        GlobalFree(h);
        return nullptr;
      }
      std::memset(header, 0, sizeof(BITMAPINFOHEADER));
      header->biSize = sizeof(BITMAPINFOHEADER);
      header->biWidth = static_cast<LONG>(width);
      header->biHeight = -static_cast<LONG>(height);
      header->biPlanes = 1;
      header->biBitCount = 32;
      header->biCompression = BI_RGB;
      header->biSizeImage = static_cast<DWORD>(pixel_bytes);
      auto* dst = reinterpret_cast<u8*>(header + 1);
      for (usize i = 0; i < width * height; ++i) {
        dst[i * 4u + 0u] = image.data[i * 4u + 2u];
        dst[i * 4u + 1u] = image.data[i * 4u + 1u];
        dst[i * 4u + 2u] = image.data[i * 4u + 0u];
        dst[i * 4u + 3u] = image.data[i * 4u + 3u];
      }
      GlobalUnlock(h);
      return h;
    }

    static HGLOBAL win32_hdrop_from_files(const std::vector<std::string>& files) {
      std::vector<std::wstring> wide_files;
      usize chars = 1;
      for (const auto& file : files) {
        auto wide = win32_utf8_to_wide(file);
        if (wide.empty())
          continue;
        chars += wide.size() + 1u;
        wide_files.push_back(std::move(wide));
      }
      if (wide_files.empty())
        return nullptr;
      const usize bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (!h)
        return nullptr;
      auto* drop = static_cast<DROPFILES*>(GlobalLock(h));
      if (!drop) {
        GlobalFree(h);
        return nullptr;
      }
      std::memset(drop, 0, bytes);
      drop->pFiles = sizeof(DROPFILES);
      drop->fWide = TRUE;
      auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
      for (const auto& wide : wide_files) {
        std::memcpy(cursor, wide.c_str(), wide.size() * sizeof(wchar_t));
        cursor += wide.size() + 1u;
      }
      GlobalUnlock(h);
      return h;
    }

    static bool win32_duplicate_hglobal(HGLOBAL source, STGMEDIUM* out) {
      SIZE_T bytes = GlobalSize(source);
      HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (!copy)
        return false;
      void* src = GlobalLock(source);
      void* dst = GlobalLock(copy);
      if (!src || !dst) {
        if (src)
          GlobalUnlock(source);
        if (dst)
          GlobalUnlock(copy);
        GlobalFree(copy);
        return false;
      }
      std::memcpy(dst, src, bytes);
      GlobalUnlock(source);
      GlobalUnlock(copy);
      out->tymed = TYMED_HGLOBAL;
      out->hGlobal = copy;
      out->pUnkForRelease = nullptr;
      return true;
    }

    class win32_data_object final : public IDataObject {
    public:
      ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&ref_count_);
      }
      ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0)
          delete this;
        return count;
      }
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
          return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
          *out = static_cast<IDataObject*>(this);
          AddRef();
          return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
      }
      bool add_hglobal(CLIPFORMAT format, HGLOBAL hglobal) {
        if (!hglobal)
          return false;
        entry e{};
        e.format.cfFormat = format;
        e.format.dwAspect = DVASPECT_CONTENT;
        e.format.lindex = -1;
        e.format.tymed = TYMED_HGLOBAL;
        e.medium.tymed = TYMED_HGLOBAL;
        e.medium.hGlobal = hglobal;
        entries_.push_back(e);
        return true;
      }
      HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium)
          return E_INVALIDARG;
        for (auto& entry : entries_) {
          if (entry.format.cfFormat == format->cfFormat && (format->tymed & TYMED_HGLOBAL))
            return win32_duplicate_hglobal(entry.medium.hGlobal, medium) ? S_OK : STG_E_MEDIUMFULL;
        }
        return DV_E_FORMATETC;
      }
      HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return E_NOTIMPL;
      }
      HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format)
          return E_INVALIDARG;
        for (auto& entry : entries_) {
          if (entry.format.cfFormat == format->cfFormat && (format->tymed & TYMED_HGLOBAL))
            return S_OK;
        }
        return DV_E_FORMATETC;
      }
      HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out)
          out->ptd = nullptr;
        return E_NOTIMPL;
      }
      HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
      }
      HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override {
        return E_NOTIMPL;
      }
      HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }
      HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }
      HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

    private:
      struct entry {
        FORMATETC format{};
        STGMEDIUM medium{};
      };
      ~win32_data_object() {
        for (auto& entry : entries_)
          ReleaseStgMedium(&entry.medium);
      }
      LONG ref_count_ = 1;
      std::vector<entry> entries_;
    };

    class win32_drop_source final : public IDropSource {
    public:
      ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&ref_count_);
      }
      ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0)
          delete this;
        return count;
      }
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
          return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
          *out = static_cast<IDropSource*>(this);
          AddRef();
          return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
      }
      HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape_pressed, DWORD key_state) override {
        if (escape_pressed)
          return DRAGDROP_S_CANCEL;
        if ((key_state & MK_LBUTTON) == 0)
          return DRAGDROP_S_DROP;
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
      }

    private:
      ~win32_drop_source() = default;
      LONG ref_count_ = 1;
    };

    class win32_drop_target final : public IDropTarget {
    public:
      explicit win32_drop_target(glfw_window* owner) : owner_(owner) {}

      ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&ref_count_);
      }
      ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0)
          delete this;
        return count;
      }
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
          return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
          *out = static_cast<IDropTarget*>(this);
          AddRef();
          return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
      }
      HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt,
                                          DWORD* effect) override {
        paths_ = glfw_window::win32_paths_from_data_object(data);
        emit(input_event::kind_t::drag_enter, pt, paths_);
        set_effect(effect);
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        emit(input_event::kind_t::drag_over, pt, paths_);
        set_effect(effect);
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (owner_)
          owner_->push_drag_event(input_event::kind_t::drag_leave, 0.0, 0.0, {});
        paths_.clear();
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        (void)pt;
        auto paths = glfw_window::win32_paths_from_data_object(data);
        if (paths.empty())
          paths = paths_;
        if (owner_ && !paths.empty()) {
          input_event ev{};
          ev.kind = input_event::kind_t::drop_files;
          ev.paths = paths;
          owner_->push_event(std::move(ev));
        }
        paths_.clear();
        set_effect(effect);
        return S_OK;
      }

    private:
      ~win32_drop_target() = default;

      static void set_effect(DWORD* effect) {
        if (effect)
          *effect = DROPEFFECT_COPY;
      }

      void emit(input_event::kind_t kind, POINTL pt, const std::vector<std::string>& paths) {
        if (!owner_)
          return;
        HWND hwnd = glfwGetWin32Window(owner_->handle_);
        POINT client{pt.x, pt.y};
        if (hwnd)
          (void)ScreenToClient(hwnd, &client);
        owner_->push_drag_event(kind, static_cast<double>(client.x), static_cast<double>(client.y),
                                paths);
      }

      LONG ref_count_ = 1;
      glfw_window* owner_ = nullptr;
      std::vector<std::string> paths_;
    };

    bool start_win32_drag(const drag_payload& payload) {
      auto* data = new win32_data_object();
      bool has_payload = false;
      has_payload =
          data->add_hglobal(CF_HDROP, win32_hdrop_from_files(payload.files)) || has_payload;
      if (payload.text) {
        auto wide = win32_utf8_to_wide(*payload.text);
        has_payload =
            data->add_hglobal(CF_UNICODETEXT, win32_hglobal_from_wstring(wide)) || has_payload;
      }
      if (payload.html) {
        UINT html_format = RegisterClipboardFormatW(L"HTML Format");
        has_payload = data->add_hglobal(static_cast<CLIPFORMAT>(html_format),
                                        win32_hglobal_from_cf_html(*payload.html)) ||
                      has_payload;
      }
      if (payload.image) {
        has_payload =
            data->add_hglobal(CF_DIB, win32_cf_dib_from_image(*payload.image)) || has_payload;
      }
      if (!has_payload) {
        data->Release();
        return false;
      }

      HRESULT init_hr = OleInitialize(nullptr);
      if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
        data->Release();
        return false;
      }

      auto* source = new win32_drop_source();
      DWORD effect = DROPEFFECT_NONE;
      HRESULT drag_hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
      source->Release();
      data->Release();
      if (SUCCEEDED(init_hr))
        OleUninitialize();
      return drag_hr == DRAGDROP_S_DROP || drag_hr == DRAGDROP_S_CANCEL;
    }

    void install_win32_drop_target() {
      if (win32_drop_target_)
        return;
      HWND hwnd = glfwGetWin32Window(handle_);
      if (!hwnd)
        return;

      HRESULT init_hr = OleInitialize(nullptr);
      if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
        warn_once(warned_win32_drop_target_, "fxe.window: OleInitialize failed for drag/drop");
        return;
      }
      win32_ole_initialized_ = SUCCEEDED(init_hr);

      auto* target = new win32_drop_target(this);
      HRESULT register_hr = RegisterDragDrop(hwnd, target);
      if (FAILED(register_hr)) {
        target->Release();
        if (win32_ole_initialized_) {
          OleUninitialize();
          win32_ole_initialized_ = false;
        }
        if (register_hr != DRAGDROP_E_ALREADYREGISTERED)
          warn_once(warned_win32_drop_target_, "fxe.window: RegisterDragDrop failed");
        return;
      }
      win32_drop_target_ = target;
    }

    void uninstall_win32_drop_target() {
      HWND hwnd = handle_ ? glfwGetWin32Window(handle_) : nullptr;
      if (win32_drop_target_ && hwnd)
        (void)RevokeDragDrop(hwnd);
      if (win32_drop_target_) {
        win32_drop_target_->Release();
        win32_drop_target_ = nullptr;
      }
      if (win32_ole_initialized_) {
        OleUninitialize();
        win32_ole_initialized_ = false;
      }
    }

    void clear_win32_icon() {
      HWND hwnd = handle_ ? glfwGetWin32Window(handle_) : nullptr;
      if (hwnd) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, 0);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
        SetClassLongPtrW(hwnd, GCLP_HICON, 0);
        SetClassLongPtrW(hwnd, GCLP_HICONSM, 0);
      }
      if (win32_icon_) {
        DestroyIcon(win32_icon_);
        win32_icon_ = nullptr;
      }
    }

    void set_win32_icon_from_rgba(const u8* rgba, int width, int height) {
      HWND hwnd = glfwGetWin32Window(handle_);
      if (!hwnd)
        return;
      HICON icon = win32_create_hicon_from_rgba(rgba, width, height);
      if (!icon)
        return;

      HICON old = win32_icon_;
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
      SetClassLongPtrW(hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(icon));
      SetClassLongPtrW(hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(icon));
      win32_icon_ = icon;
      if (old)
        DestroyIcon(old);
    }
#endif

    void push_event(input_event ev) {
      ev.modifiers = mods_.load(std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(input_mutex_);
        injected_events_.push_back(std::move(ev));
      }
      redraw_requested_.store(true, std::memory_order_release);
      glfwPostEmptyEvent();
    }

    void push_compose_event(const char* preedit, int cursor, const char* committed) {
      input_event ev{};
      ev.kind = input_event::kind_t::compose;
      ev.preedit = preedit ? preedit : "";
      ev.cursor = cursor;
      ev.committed = committed ? committed : "";
      push_event(std::move(ev));
    }

    void push_drag_event(input_event::kind_t kind, double x, double y,
                         const std::vector<std::string>& paths) {
      input_event ev{};
      ev.kind = kind;
      ev.x = x;
      ev.y = y;
      ev.paths = paths;
      push_event(std::move(ev));
    }

    void apply_size_limits() {
      int mnw = (min_size_limit_ && min_size_limit_->x > 0) ? min_size_limit_->x : GLFW_DONT_CARE;
      int mnh = (min_size_limit_ && min_size_limit_->y > 0) ? min_size_limit_->y : GLFW_DONT_CARE;
      int mxw = (max_size_limit_ && max_size_limit_->x > 0) ? max_size_limit_->x : GLFW_DONT_CARE;
      int mxh = (max_size_limit_ && max_size_limit_->y > 0) ? max_size_limit_->y : GLFW_DONT_CARE;
      glfwSetWindowSizeLimits(handle_, mnw, mnh, mxw, mxh);
    }

    static GLFWcursor* create_cursor(cursor_kind kind) {
      auto try_shape = [](int shape) -> GLFWcursor* { return glfwCreateStandardCursor(shape); };
      switch (kind) {
      case cursor_kind::arrow:
        return try_shape(GLFW_ARROW_CURSOR);
      case cursor_kind::ibeam:
        return try_shape(GLFW_IBEAM_CURSOR);
      case cursor_kind::crosshair:
        return try_shape(GLFW_CROSSHAIR_CURSOR);
      case cursor_kind::hand:
        return try_shape(GLFW_HAND_CURSOR);
      case cursor_kind::hresize:
        return try_shape(GLFW_HRESIZE_CURSOR);
      case cursor_kind::vresize:
        return try_shape(GLFW_VRESIZE_CURSOR);
      case cursor_kind::all_resize: {
#ifdef GLFW_RESIZE_ALL_CURSOR
        if (auto* c = try_shape(GLFW_RESIZE_ALL_CURSOR))
          return c;
#endif
        return try_shape(GLFW_ARROW_CURSOR);
      }
      case cursor_kind::nesw_resize: {
#ifdef GLFW_RESIZE_NESW_CURSOR
        if (auto* c = try_shape(GLFW_RESIZE_NESW_CURSOR))
          return c;
#endif
        return try_shape(GLFW_ARROW_CURSOR);
      }
      case cursor_kind::nwse_resize: {
#ifdef GLFW_RESIZE_NWSE_CURSOR
        if (auto* c = try_shape(GLFW_RESIZE_NWSE_CURSOR))
          return c;
#endif
        return try_shape(GLFW_ARROW_CURSOR);
      }
      case cursor_kind::not_allowed: {
#ifdef GLFW_NOT_ALLOWED_CURSOR
        if (auto* c = try_shape(GLFW_NOT_ALLOWED_CURSOR))
          return c;
#endif
        return try_shape(GLFW_ARROW_CURSOR);
      }
      case cursor_kind::hidden:
        return nullptr;
      }
      return nullptr;
    }

    void install_callbacks() {
      // Mark dirty on framebuffer changes / refresh / size / scale, plus push
      // synthesized input events.
      glfwSetFramebufferSizeCallback(handle_, [](GLFWwindow* w, int, int) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        self->redraw_requested_.store(true, std::memory_order_release);
        // macOS Cocoa keeps `glfwWaitEvents` blocked inside the live-resize
        // modal loop; the only way to keep the window painted while the user
        // is still dragging is to drive a frame from this callback.
        if (self->redraw_handler_)
          self->redraw_handler_();
      });
      glfwSetWindowRefreshCallback(handle_, [](GLFWwindow* w) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        self->redraw_requested_.store(true, std::memory_order_release);
        if (self->redraw_handler_)
          self->redraw_handler_();
      });
      glfwSetWindowSizeCallback(handle_, [](GLFWwindow* w, int width, int height) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::window_resize;
        ev.width = width;
        ev.height = height;
        self->push_event(ev);
      });
      glfwSetWindowContentScaleCallback(handle_, [](GLFWwindow* w, float sx, float sy) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::window_scale;
        ev.scale_x = sx;
        ev.scale_y = sy;
        self->push_event(ev);
      });
      glfwSetWindowPosCallback(handle_, [](GLFWwindow* w, int x, int y) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::window_move;
        ev.pos_x = x;
        ev.pos_y = y;
        self->push_event(ev);
      });
      glfwSetWindowFocusCallback(handle_, [](GLFWwindow* w, int focused) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = focused ? input_event::kind_t::window_focus : input_event::kind_t::window_blur;
        self->push_event(ev);
      });
      glfwSetWindowIconifyCallback(handle_, [](GLFWwindow* w, int iconified) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind =
            iconified ? input_event::kind_t::window_iconify : input_event::kind_t::window_restore;
        self->push_event(ev);
      });
      glfwSetWindowMaximizeCallback(handle_, [](GLFWwindow* w, int maxed) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind =
            maxed ? input_event::kind_t::window_maximize : input_event::kind_t::window_unmaximize;
        self->push_event(ev);
      });
      glfwSetWindowCloseCallback(handle_, [](GLFWwindow* w) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::window_close;
        self->push_event(ev);
      });
      glfwSetCursorEnterCallback(handle_, [](GLFWwindow* w, int entered) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = entered ? input_event::kind_t::cursor_enter : input_event::kind_t::cursor_leave;
        self->push_event(ev);
      });
      glfwSetCursorPosCallback(handle_, [](GLFWwindow* w, double x, double y) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::mouse_move;
        ev.x = x;
        ev.y = y;
        if (self->cursor_seen_) {
          ev.dx = x - self->last_cursor_x_;
          ev.dy = y - self->last_cursor_y_;
        }
        self->last_cursor_x_ = x;
        self->last_cursor_y_ = y;
        self->cursor_seen_ = true;
        self->push_event(ev);
      });
      glfwSetMouseButtonCallback(handle_, [](GLFWwindow* w, int button, int action, int mods) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        self->mods_.store(mods, std::memory_order_relaxed);
        input_event ev{};
        ev.kind = (action == GLFW_PRESS) ? input_event::kind_t::mouse_button_down
                                         : input_event::kind_t::mouse_button_up;
        // Map left=0, right=1, middle=2; pass through others as-is.
        switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:
          ev.button = 0;
          break;
        case GLFW_MOUSE_BUTTON_RIGHT:
          ev.button = 1;
          break;
        case GLFW_MOUSE_BUTTON_MIDDLE:
          ev.button = 2;
          break;
        default:
          ev.button = button;
          break;
        }
        ev.x = self->last_cursor_x_;
        ev.y = self->last_cursor_y_;
        self->push_event(ev);
      });
      glfwSetScrollCallback(handle_, [](GLFWwindow* w, double xoff, double yoff) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::mouse_wheel;
        ev.dx = xoff;
        ev.dy = yoff;
        ev.x = self->last_cursor_x_;
        ev.y = self->last_cursor_y_;
        ev.scroll_phase = input_event::scroll_phase_t::none;
        ev.precision = false;
        self->push_event(ev);
      });
      glfwSetKeyCallback(handle_, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        if (action == GLFW_REPEAT) {
          // Treat repeats as additional key_down events.
        }
        self->mods_.store(mods, std::memory_order_relaxed);
        input_event ev{};
        ev.kind =
            (action == GLFW_RELEASE) ? input_event::kind_t::key_up : input_event::kind_t::key_down;
        ev.key = key;
        ev.scancode = scancode;
        self->push_event(ev);
      });
      glfwSetCharCallback(handle_, [](GLFWwindow* w, unsigned codepoint) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::key_char;
        ev.codepoint = codepoint;
        self->push_event(ev);
      });
      glfwSetDropCallback(handle_, [](GLFWwindow* w, int count, const char** paths) {
        auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(w));
        if (!self)
          return;
        input_event ev{};
        ev.kind = input_event::kind_t::drop_files;
        ev.paths.reserve(static_cast<usize>(count));
        for (int i = 0; i < count; ++i)
          if (paths[i])
            ev.paths.emplace_back(paths[i]);
        self->push_event(ev);
      });
    }

    GLFWwindow* handle_ = nullptr;
    std::atomic<bool> redraw_requested_{true};
    redraw_handler redraw_handler_;
    std::array<GLFWcursor*, 11> cursors_{};
    GLFWcursor* custom_cursor_ = nullptr;
    std::optional<math::ivec2> min_size_limit_;
    std::optional<math::ivec2> max_size_limit_;
    int saved_x_ = 100, saved_y_ = 100, saved_w_ = 1280, saved_h_ = 720;
    std::optional<float> dpi_scale_override_;
    bool fullscreen_ = false;
    fullscreen_mode fullscreen_mode_ = fullscreen_mode::borderless;
    int fullscreen_monitor_index_ = -1;
    bool saved_decorated_ = true;
    bool saved_floating_ = false;
    bool content_protection_ = false;
    std::atomic<int> mods_{0};
    double last_cursor_x_ = 0.0, last_cursor_y_ = 0.0;
    bool cursor_seen_ = false;
    std::vector<math::ivec4> drag_rects_;
    bool warned_title_bar_style_ = false;
    std::string vibrancy_kind_;
    bool blur_behind_ = false;
    bool warned_unsupported_window_controls_overlay_ = false;
    std::mutex input_mutex_;
    std::string title_;
#if !defined(__APPLE__) && !defined(_WIN32)
    bool warned_unsupported_vibrancy_ = false;
    bool warned_unsupported_blur_behind_ = false;
#endif
#if defined(__APPLE__)
    void* wrap_view_ = nullptr;
    void* metal_layer_ = nullptr; // CAMetalLayer*, retained by NSView; we cache the raw ptr.
    void* visual_effect_view_ = nullptr;
    std::vector<math::ivec4> pending_drag_rects_;
#elif defined(_WIN32)
    UINT_PTR win32_subclass_id_ = 0;
    bool win32_subclassed_ = false;
    bool window_controls_overlay_ = false;
    bool warned_win32_subclass_ = false;
    bool warned_win32_blur_ = false;
    bool warned_win32_vibrancy_ = false;
    bool warned_win32_transparent_vibrancy_ = false;
    bool warned_win32_drop_target_ = false;
    math::ivec2 caption_button_offset_{0, 0};
    HICON win32_icon_ = nullptr;
    win32_drop_target* win32_drop_target_ = nullptr;
    bool win32_ole_initialized_ = false;
#endif
    bool transparent_ = false;
    bool decorated_ = true;
  };

#else // !FXE_HAS_GLFW

  std::vector<monitor_info> list_monitors() {
    return {};
  }
  monitor_info primary_monitor() {
    return {};
  }

  void install_monitor_change_observer(std::function<void()> /*cb*/) {}

  void uninstall_monitor_change_observer() {}

  class stub_window final : public window {
  public:
    explicit stub_window(const window_desc& desc) : size_{desc.width, desc.height} {}
    void poll() override {}
    void wait_events() override {}
    void wait_events_timeout(double) override {}
    void post_redraw() override {
      redraw_requested_.store(true, std::memory_order_release);
    }
    bool peek_redraw_request() const override {
      return redraw_requested_.load(std::memory_order_acquire);
    }
    bool take_redraw_request() override {
      return redraw_requested_.exchange(false, std::memory_order_acq_rel);
    }
    void close() override {
      closed_ = true;
    }
    bool should_close() const override {
      return closed_;
    }
    math::uvec2 framebuffer_size() const override {
      return size_;
    }
    void set_dpi_scale_override(std::optional<float> scale) override {
      if (scale && std::isfinite(*scale) && *scale > 0.0f) {
        dpi_scale_override_ = *scale;
      } else {
        dpi_scale_override_.reset();
      }
    }
    [[nodiscard]] float dpi_scale() const override {
      return dpi_scale_override_.value_or(1.0f);
    }
    [[nodiscard]] bool has_dpi_scale_override() const override {
      return dpi_scale_override_.has_value();
    }
    void set_vsync(bool) override {}
    void* native_handle() const override {
      return nullptr;
    }

  private:
    math::uvec2 size_{};
    bool closed_ = false;
    std::optional<float> dpi_scale_override_;
    std::atomic<bool> redraw_requested_{true};
  };

#endif

  std::unique_ptr<window> create_window(const window_desc& desc) {
#if FXE_HAS_GLFW
    return std::make_unique<glfw_window>(desc);
#else
    return std::make_unique<stub_window>(desc);
#endif
  }
  bool fxe_supports_native_gestures() {
#if defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
  }

#if FXE_HAS_WGPU
  // ---------------------------------------------------------------------------
  // make_wgpu_surface
  // ---------------------------------------------------------------------------
  wgpu::Surface make_wgpu_surface(window& w, const wgpu::Instance& instance) {
    void* native = w.native_handle();
    wgpu::SurfaceDescriptor desc{};

#if defined(__APPLE__)
    wgpu::SurfaceSourceMetalLayer chain{};
    void* metal = fxe_wgpu_metal_layer_for_window(native, w.is_transparent());
    chain.layer = metal;
#if FXE_HAS_GLFW
    if (auto* gw = dynamic_cast<glfw_window*>(&w))
      gw->set_metal_layer_handle(metal);
#endif
    desc.nextInChain = &chain;
    return const_cast<wgpu::Instance&>(instance).CreateSurface(&desc);
#elif defined(_WIN32)
    wgpu::SurfaceSourceWindowsHWND chain{};
    chain.hinstance = ::GetModuleHandleW(nullptr);
    chain.hwnd = native;
    desc.nextInChain = &chain;
    return const_cast<wgpu::Instance&>(instance).CreateSurface(&desc);
#else
#if FXE_HAS_GLFW && defined(GLFW_PLATFORM_WAYLAND)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
      wgpu::SurfaceSourceWaylandSurface chain{};
      chain.display = glfwGetWaylandDisplay();
      chain.surface = native;
      desc.nextInChain = &chain;
      return const_cast<wgpu::Instance&>(instance).CreateSurface(&desc);
    }
#endif
#if FXE_HAS_GLFW
    wgpu::SurfaceSourceXlibWindow chain{};
    chain.display = glfwGetX11Display();
    chain.window = static_cast<u32>(reinterpret_cast<std::uintptr_t>(native));
    desc.nextInChain = &chain;
    return const_cast<wgpu::Instance&>(instance).CreateSurface(&desc);
#else
    (void)native;
    return {};
#endif
#endif
  }
#endif // FXE_HAS_WGPU
} // namespace fxe
