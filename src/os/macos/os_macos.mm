// macOS implementation of fxe::os.
// Compiled with -x objective-c++ -fobjc-arc (see CMakeLists.txt integration).
//
// All AppKit interaction must happen on the main thread; helpers that may be
// called off-main wrap their bodies in dispatch_sync(dispatch_get_main_queue()).

#include "../os.hpp"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <mach-o/dyld.h>
#include <mutex>
#include <queue>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <fxe/types.hpp>

namespace fxe::os {
  void dispatch_notification_response(int id, const std::string& action_id,
                                      std::optional<std::string> input);
}

namespace {
  std::mutex g_system_change_observer_mu;
  std::function<void(const char*)> g_system_change_cb;
  bool g_system_change_observer_installed = false;

  void emit_system_change(const char* kind) {
    if (!kind || *kind == '\0')
      return;
    std::function<void(const char*)> cb;
    {
      std::lock_guard<std::mutex> lock(g_system_change_observer_mu);
      cb = g_system_change_cb;
    }
    if (!cb)
      return;
    if ([NSThread isMainThread]) {
      cb(kind);
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      std::function<void(const char*)> main_cb;
      {
        std::lock_guard<std::mutex> lock(g_system_change_observer_mu);
        main_cb = g_system_change_cb;
      }
      if (main_cb)
        main_cb(kind);
    });
  }
}

@interface FxeSystemObserver : NSObject
+ (instancetype)shared;
- (void)appearanceChanged:(NSNotification*)notification;
- (void)accessibilityDisplayOptionsChanged:(NSNotification*)notification;
- (void)systemColorsChanged:(NSNotification*)notification;
@end

@implementation FxeSystemObserver
+ (instancetype)shared {
  static FxeSystemObserver* observer = [[FxeSystemObserver alloc] init];
  return observer;
}

- (void)appearanceChanged:(NSNotification*)notification {
  (void)notification;
  emit_system_change("colorScheme");
}

- (void)accessibilityDisplayOptionsChanged:(NSNotification*)notification {
  (void)notification;
  emit_system_change("prefersReducedMotion");
  emit_system_change("prefersHighContrast");
}

- (void)systemColorsChanged:(NSNotification*)notification {
  (void)notification;
  emit_system_change("accentColor");
}
@end




@interface FxeNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation FxeNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  (void)center;
  NSString* requestId = response.notification.request.identifier;
  static NSString* prefix = @"fxe-notif-";
  if ([requestId hasPrefix:prefix]) {
    NSInteger parsed = [[requestId substringFromIndex:[prefix length]] integerValue];
    std::optional<std::string> input;
    if ([response isKindOfClass:[UNTextInputNotificationResponse class]]) {
      NSString* text = [static_cast<UNTextInputNotificationResponse*>(response) userText];
      if (text) {
        const char* raw = [text UTF8String];
        input = raw ? std::string(raw) : std::string();
      }
    }
    const char* rawAction = [response.actionIdentifier UTF8String];
    fxe::os::dispatch_notification_response(static_cast<int>(parsed),
                                            rawAction ? std::string(rawAction) : std::string(),
                                            std::move(input));
  }
  completionHandler();
}
@end

namespace fxe::os {
  namespace {
    // ---- Main-thread dispatch queue ---------------------------------------
    std::mutex g_dispatch_mu;
    std::queue<std::function<void()>> g_dispatch_q;

    inline void run_on_main_sync_void(void (^block)(void)) {
      if ([NSThread isMainThread]) {
        block();
        return;
      }
      dispatch_sync(dispatch_get_main_queue(), block);
    }

    inline NSString* ns(std::string_view s) {
      return [[NSString alloc] initWithBytes:s.data()
                                      length:s.size()
                                    encoding:NSUTF8StringEncoding];
    }

    inline std::string from_ns(NSString* s) {
      if (!s)
        return {};
      const char* c = [s UTF8String];
      return c ? std::string(c) : std::string();
    }

    NSString* pasteboard_type_for_mime(std::string_view mime) {
      NSString* mime_string = ns(mime);
      if (mime_string.length == 0)
        return nil;
      if (@available(macOS 11.0, *)) {
        UTType* type = [UTType typeWithMIMEType:mime_string];
        if (type)
          return type.identifier;
      } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CFStringRef uti = UTTypeCreatePreferredIdentifierForTag(
            kUTTagClassMIMEType, (__bridge CFStringRef)mime_string, nullptr);
#pragma clang diagnostic pop
        if (uti)
          return CFBridgingRelease(uti);
      }
      return mime_string;
    }

    NSData* data_from_bytes(const u8* bytes, usize size) {
      if (size == 0)
        return [NSData data];
      if (!bytes || size > static_cast<usize>(NSIntegerMax))
        return nil;
      return [NSData dataWithBytes:bytes length:static_cast<NSUInteger>(size)];
    }

    std::vector<u8> bytes_from_nsdata(NSData* data) {
      if (!data)
        return {};
      const NSUInteger length = data.length;
      std::vector<u8> out(static_cast<usize>(length));
      if (length > 0)
        std::memcpy(out.data(), data.bytes, static_cast<usize>(length));
      return out;
    }

    struct active_bookmark_access {
      std::string blob;
      NSURL* url = nil;
    };

    std::mutex g_bookmark_mu;
    std::unordered_map<usize, std::vector<active_bookmark_access>> g_active_bookmarks;

    usize bookmark_blob_hash(std::string_view blob) {
      return std::hash<std::string_view>{}(blob);
    }

    NSData* bookmark_data_from_base64(std::string_view blob) {
      return [[NSData alloc] initWithBase64EncodedString:ns(blob) options:0];
    }

    std::string bookmark_path_from_url(NSURL* url) {
      std::string path = from_ns([url path]);
      if (path == "/private/tmp")
        return "/tmp";
      return path;
    }
  } // namespace

  // ---- Paths --------------------------------------------------------------
  std::string get_path(std::string_view kind) {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSURL* url = nil;
    if (kind == "userData") {
      url = [[fm URLsForDirectory:NSApplicationSupportDirectory
                        inDomains:NSUserDomainMask] firstObject];
    } else if (kind == "documents") {
      url = [[fm URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask] firstObject];
    } else if (kind == "downloads") {
      url = [[fm URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask] firstObject];
    } else if (kind == "temp") {
      return from_ns(NSTemporaryDirectory());
    } else if (kind == "home") {
      return from_ns(NSHomeDirectory());
    }
    return url ? from_ns([url path]) : std::string{};
  }
  bool system_prefers_reduced_motion() {
    @autoreleasepool {
      @try {
        return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion] == YES;
      } @catch (NSException*) {
        return false;
      }
    }
  }

  bool system_prefers_high_contrast() {
    @autoreleasepool {
      @try {
        return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldIncreaseContrast] == YES;
      } @catch (NSException*) {
        return false;
      }
    }
  }

  double system_font_scale() {
    @autoreleasepool {
      @try {
        // macOS does not expose a straightforward system text scaling factor in AppKit.
        return 1.0;
      } @catch (NSException*) {
        return 1.0;
      }
    }
  }

  std::string system_color_scheme() {
    @autoreleasepool {
      @try {
        NSAppearance* appearance = NSApp ? [NSApp effectiveAppearance] : nil;
        if (appearance) {
          NSString* match = [appearance
              bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
          if ([match isEqualToString:NSAppearanceNameDarkAqua])
            return "dark";
          if ([match isEqualToString:NSAppearanceNameAqua])
            return "light";
        }

        NSString* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
        if ([style caseInsensitiveCompare:@"Dark"] == NSOrderedSame)
          return "dark";
        return "light";
      } @catch (NSException*) {
        return "no-preference";
      }
    }
  }

  std::string system_accent_color() {
    @autoreleasepool {
      @try {
        if (@available(macOS 10.14, *)) {
          NSColor* accent = [NSColor controlAccentColor];
          if (!accent)
            return {};
          NSColor* rgb = [accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
          if (!rgb)
            return {};
          CGFloat r = 0;
          CGFloat g = 0;
          CGFloat b = 0;
          CGFloat a = 0;
          [rgb getRed:&r green:&g blue:&b alpha:&a];
          int r8 = static_cast<int>(std::clamp(static_cast<double>(r), 0.0, 1.0) * 255.0 + 0.5);
          int g8 = static_cast<int>(std::clamp(static_cast<double>(g), 0.0, 1.0) * 255.0 + 0.5);
          int b8 = static_cast<int>(std::clamp(static_cast<double>(b), 0.0, 1.0) * 255.0 + 0.5);
          NSString* hex = [NSString stringWithFormat:@"%02x%02x%02x", r8, g8, b8];
          return from_ns(hex);
        }
        return {};
      } @catch (NSException*) {
        return {};
      }
    }
  }

  bool install_system_change_observer(std::function<void(const char* kind)> cb) {
    {
      std::lock_guard<std::mutex> lock(g_system_change_observer_mu);
      g_system_change_cb = std::move(cb);
    }
    __block bool installed = false;
    run_on_main_sync_void(^{
      @autoreleasepool {
        @try {
          if (!g_system_change_observer_installed) {
            FxeSystemObserver* observer = [FxeSystemObserver shared];
            [[NSDistributedNotificationCenter defaultCenter]
                addObserver:observer
                   selector:@selector(appearanceChanged:)
                       name:@"AppleInterfaceThemeChangedNotification"
                     object:nil];
            [[[NSWorkspace sharedWorkspace] notificationCenter]
                addObserver:observer
                   selector:@selector(accessibilityDisplayOptionsChanged:)
                       name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                     object:nil];
            [[NSNotificationCenter defaultCenter]
                addObserver:observer
                   selector:@selector(systemColorsChanged:)
                       name:NSSystemColorsDidChangeNotification
                     object:nil];
            g_system_change_observer_installed = true;
          }
          installed = true;
        } @catch (NSException*) {
          installed = false;
        }
      }
    });
    return installed;
  }





  bool open_external(std::string_view url) {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u)
      return false;
    return [[NSWorkspace sharedWorkspace] openURL:u];
  }

  bool show_item_in_folder(std::string_view path) {
    NSString* p = ns(path);
    return [[NSWorkspace sharedWorkspace] selectFile:p inFileViewerRootedAtPath:@""];
  }

  void beep() {
    NSBeep();
  }

  bool trash_item(std::string_view path) {
    NSURL* u = [NSURL fileURLWithPath:ns(path)];
    NSError* err = nil;
    BOOL ok = [[NSFileManager defaultManager] trashItemAtURL:u resultingItemURL:nil error:&err];
    return ok == YES;
  }

  std::optional<std::string> bookmark_persist(std::string_view path) {
    NSURL* url = [NSURL fileURLWithPath:ns(path)];
    if (!url)
      return std::nullopt;

    NSError* error = nil;
    NSData* data = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                 includingResourceValuesForKeys:nil
                                  relativeToURL:nil
                                          error:&error];
    if (!data) {
      std::string dot_path(path);
      if (dot_path.empty() || dot_path.back() == '/')
        dot_path += ".";
      else
        dot_path += "/.";
      NSURL* dot_url = [NSURL fileURLWithPath:ns(dot_path)];
      if (dot_url) {
        error = nil;
        data = [dot_url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                 includingResourceValuesForKeys:nil
                                  relativeToURL:nil
                                          error:&error];
      }
    }
    if (!data)
      return std::nullopt;

    NSString* encoded = [data base64EncodedStringWithOptions:0];
    std::string out = from_ns(encoded);
    return out.empty() ? std::nullopt : std::optional<std::string>{std::move(out)};
  }

  std::optional<std::pair<std::string, bool>> bookmark_resolve(std::string_view blob) {
    NSData* data = bookmark_data_from_base64(blob);
    if (!data)
      return std::nullopt;

    BOOL stale = NO;
    NSError* error = nil;
    NSURL* url = [NSURL URLByResolvingBookmarkData:data
                                           options:NSURLBookmarkResolutionWithSecurityScope
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:&error];
    if (!url)
      return std::nullopt;

    std::string path = bookmark_path_from_url(url);
    if (path.empty())
      return std::nullopt;
    return std::make_pair(std::move(path), stale == YES);
  }

  bool bookmark_start_access(std::string_view blob) {
    NSData* data = bookmark_data_from_base64(blob);
    if (!data)
      return false;

    BOOL stale = NO;
    NSError* error = nil;
    NSURL* url = [NSURL URLByResolvingBookmarkData:data
                                           options:NSURLBookmarkResolutionWithSecurityScope
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:&error];
    if (!url)
      return false;

    if ([url startAccessingSecurityScopedResource] != YES)
      return false;

    std::lock_guard<std::mutex> lock(g_bookmark_mu);
    g_active_bookmarks[bookmark_blob_hash(blob)].push_back(
        active_bookmark_access{std::string(blob), url});
    return true;
  }

  void bookmark_stop_access(std::string_view blob) {
    NSURL* url = nil;
    {
      std::lock_guard<std::mutex> lock(g_bookmark_mu);
      auto it = g_active_bookmarks.find(bookmark_blob_hash(blob));
      if (it == g_active_bookmarks.end())
        return;
      auto& entries = it->second;
      std::string needle(blob);
      for (auto rit = entries.rbegin(); rit != entries.rend(); ++rit) {
        if (rit->blob != needle)
          continue;
        url = rit->url;
        entries.erase(std::next(rit).base());
        break;
      }
      if (entries.empty())
        g_active_bookmarks.erase(it);
    }

    if (url)
      [url stopAccessingSecurityScopedResource];
  }

  // ---- Single-instance lock (BSD flock on a per-app-id file) -------------
  namespace {
    std::atomic<int> g_lock_fd{-1};
    std::mutex g_lock_mu;

    bool read_lock_pid(const std::string& lock_path, pid_t& pid) {
      int fd = ::open(lock_path.c_str(), O_RDONLY);
      if (fd < 0)
        return false;
      char buffer[64]{};
      ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
      ::close(fd);
      if (n <= 0)
        return false;
      char* end = nullptr;
      errno = 0;
      long parsed = std::strtol(buffer, &end, 10);
      if (errno != 0 || end == buffer || parsed <= 0)
        return false;
      pid = static_cast<pid_t>(parsed);
      return true;
    }

    bool pid_is_gone(pid_t pid) {
      if (pid <= 0)
        return false;
      if (::kill(pid, 0) == 0)
        return false;
      return errno == ESRCH;
    }

    bool write_lock_pid(int fd) {
      std::string payload = std::to_string(static_cast<long>(::getpid())) + "\n";
      if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0)
        return false;
      const char* data = payload.data();
      usize left = payload.size();
      while (left > 0) {
        ssize_t n = ::write(fd, data, left);
        if (n < 0) {
          if (errno == EINTR)
            continue;
          return false;
        }
        data += n;
        left -= static_cast<usize>(n);
      }
      return true;
    }
  } // namespace
  bool request_single_instance_lock(std::string_view app_id) {
    std::lock_guard<std::mutex> guard_lock(g_lock_mu);
    if (g_lock_fd.load() >= 0)
      return true;
    std::string base = get_path("userData");
    if (base.empty())
      return false;
    std::string dir = base + "/" + std::string(app_id);
    [[NSFileManager defaultManager] createDirectoryAtPath:ns(dir)
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];

    std::string lock = dir + "/.lock";
    std::string guard = dir + "/.lock.guard";
    int guard_fd = ::open(guard.c_str(), O_RDWR | O_CREAT, 0600);
    if (guard_fd < 0)
      return false;
    if (::flock(guard_fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(guard_fd);
      return false;
    }

    pid_t recorded = 0;
    if (read_lock_pid(lock, recorded) && pid_is_gone(recorded))
      ::unlink(lock.c_str());

    int fd = ::open(lock.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
      ::close(guard_fd);
      return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd);
      ::close(guard_fd);
      return false;
    }
    if (!write_lock_pid(fd)) {
      ::close(fd);
      ::close(guard_fd);
      return false;
    }
    g_lock_fd.store(fd);
    ::close(guard_fd);
    return true;
  }

  void set_badge_count(int n) {
    run_on_main_sync_void(^{
      NSDockTile* tile = [NSApp dockTile];
      if (n <= 0) {
        [tile setBadgeLabel:nil];
      } else {
        [tile setBadgeLabel:[NSString stringWithFormat:@"%d", n]];
      }
    });
  }

  void relaunch() {
    NSURL* exe = [[NSBundle mainBundle] executableURL];
    if (!exe) {
      // Unbundled fallback: argv[0] via _NSGetExecutablePath would be cleaner,
      // but for unbundled tools we just exec the running executable.
      char buf[1024];
      u32 sz = sizeof(buf);
      // _NSGetExecutablePath comes from <mach-o/dyld.h>.
      if (_NSGetExecutablePath(buf, &sz) == 0)
        exe = [NSURL fileURLWithPath:@(buf)];
    }
    if (!exe) {
      std::_Exit(0);
    }
    NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
    cfg.createsNewApplicationInstance = YES;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:exe
                                          configuration:cfg
                                      completionHandler:nil];
    std::_Exit(0);
  }

  namespace app {
    bool add_recent_document(std::string_view path) {
      if (path.empty())
        return false;
      __block bool ok = false;
      NSString* p = ns(path);
      run_on_main_sync_void(^{
        NSURL* url = [NSURL fileURLWithPath:p];
        if (!url)
          return;
        [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url];
        ok = true;
      });
      return ok;
    }

    std::vector<std::string> recent_documents() {
      __block std::vector<std::string> out;
      run_on_main_sync_void(^{
        NSArray<NSURL*>* urls =
            [[NSDocumentController sharedDocumentController] recentDocumentURLs];
        out.reserve([urls count]);
        for (NSURL* url in urls) {
          NSString* path = [url path];
          if (path)
            out.emplace_back(from_ns(path));
        }
      });
      return out;
    }

    bool clear_recent_documents() {
      run_on_main_sync_void(^{
        [[NSDocumentController sharedDocumentController] clearRecentDocuments:nil];
      });
      return true;
    }
  } // namespace app

  // ---- Dialogs ------------------------------------------------------------
  namespace {
    void apply_filters(NSSavePanel* panel, const std::vector<dialog_filter>& filters) {
      if (filters.empty())
        return;
      NSMutableArray<NSString*>* exts = [NSMutableArray array];
      for (auto& f : filters)
        for (auto& e : f.extensions)
          [exts addObject:ns(e)];
      if (exts.count > 0) {
        if (@available(macOS 11.0, *)) {
          // UTType-based API would be cleaner but adds a framework dep. The
          // legacy allowedFileTypes still works through 14.x.
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [panel setAllowedFileTypes:exts];
#pragma clang diagnostic pop
      }
    }
  } // namespace

  std::vector<std::string> show_open_dialog(const open_dialog_options& opts) {
    __block std::vector<std::string> out;
    run_on_main_sync_void(^{
      NSOpenPanel* p = [NSOpenPanel openPanel];
      if (!opts.title.empty())
        [p setTitle:ns(opts.title)];
      if (!opts.default_path.empty())
        [p setDirectoryURL:[NSURL fileURLWithPath:ns(opts.default_path)]];
      [p setCanChooseFiles:!opts.directories];
      [p setCanChooseDirectories:opts.directories];
      [p setAllowsMultipleSelection:opts.multiple];
      apply_filters(p, opts.filters);
      if ([p runModal] == NSModalResponseOK) {
        for (NSURL* u in [p URLs])
          out.emplace_back(from_ns([u path]));
      }
    });
    return out;
  }

  std::optional<std::string> show_save_dialog(const save_dialog_options& opts) {
    __block std::optional<std::string> out;
    run_on_main_sync_void(^{
      NSSavePanel* p = [NSSavePanel savePanel];
      if (!opts.title.empty())
        [p setTitle:ns(opts.title)];
      if (!opts.default_path.empty())
        [p setDirectoryURL:[NSURL fileURLWithPath:ns(opts.default_path)]];
      apply_filters(p, opts.filters);
      if ([p runModal] == NSModalResponseOK) {
        out = from_ns([[p URL] path]);
      }
    });
    return out;
  }

  int show_message_box(const message_box_options& opts) {
    __block int idx = -1;
    run_on_main_sync_void(^{
      NSAlert* a = [[NSAlert alloc] init];
      if (!opts.title.empty())
        [a setMessageText:ns(opts.title)];
      else if (!opts.message.empty())
        [a setMessageText:ns(opts.message)];
      if (!opts.title.empty() && !opts.message.empty())
        [a setInformativeText:opts.detail.empty() ? ns(opts.message) : ns(opts.detail)];
      else if (!opts.detail.empty())
        [a setInformativeText:ns(opts.detail)];
      if (opts.type == "warning")
        [a setAlertStyle:NSAlertStyleWarning];
      else if (opts.type == "error")
        [a setAlertStyle:NSAlertStyleCritical];
      else
        [a setAlertStyle:NSAlertStyleInformational];
      if (opts.buttons.empty()) {
        [a addButtonWithTitle:@"OK"];
      } else {
        for (auto& b : opts.buttons)
          [a addButtonWithTitle:ns(b)];
      }
      NSModalResponse r = [a runModal];
      idx = static_cast<int>(r - NSAlertFirstButtonReturn);
    });
    return idx;
  }

  // ---- Notifications ------------------------------------------------------
  namespace {
    std::mutex g_notif_mu;
    std::atomic<int> g_notif_seq{0};
    std::unordered_map<int, std::function<void()>> g_notif_cbs;
    std::unordered_map<int, std::function<void(const std::string&, std::optional<std::string>)>>
        g_notif_action_cbs;

    bool app_is_bundled() {
      return [[NSBundle mainBundle] bundleIdentifier] != nil;
    }

    FxeNotificationDelegate* notification_delegate() {
      static FxeNotificationDelegate* delegate = [[FxeNotificationDelegate alloc] init];
      return delegate;
    }

    void dispatch_notification_click(int id) {
      std::function<void()> cb;
      {
        std::lock_guard<std::mutex> g(g_notif_mu);
        auto it = g_notif_cbs.find(id);
        if (it != g_notif_cbs.end()) {
          cb = std::move(it->second);
          g_notif_cbs.erase(it);
        }
        g_notif_action_cbs.erase(id);
      }
      if (cb)
        post_main_thread_dispatch(std::move(cb));
    }

    void clear_notification_callbacks(int id) {
      std::lock_guard<std::mutex> g(g_notif_mu);
      g_notif_cbs.erase(id);
      g_notif_action_cbs.erase(id);
    }

    void dispatch_notification_action(int id, std::string action_id,
                                      std::optional<std::string> input) {
      std::function<void(const std::string&, std::optional<std::string>)> cb;
      {
        std::lock_guard<std::mutex> g(g_notif_mu);
        auto it = g_notif_action_cbs.find(id);
        if (it != g_notif_action_cbs.end()) {
          cb = std::move(it->second);
          g_notif_action_cbs.erase(it);
        }
        g_notif_cbs.erase(id);
      }
      if (cb) {
        post_main_thread_dispatch(
            [cb = std::move(cb), action_id = std::move(action_id),
             input = std::move(input)]() mutable { cb(action_id, std::move(input)); });
      }
    }
  } // namespace

  void dispatch_notification_response(int id, const std::string& action_id,
                                      std::optional<std::string> input) {
    if (id <= 0)
      return;
    if (action_id == std::string([UNNotificationDismissActionIdentifier UTF8String])) {
      clear_notification_callbacks(id);
      return;
    }
    if (action_id == std::string([UNNotificationDefaultActionIdentifier UTF8String])) {
      dispatch_notification_click(id);
      return;
    }
    dispatch_notification_action(id, action_id, std::move(input));
  }

  int show_notification(const notification_options& opts) {
    int id = ++g_notif_seq;
    if (app_is_bundled()) {
      // Modern UNUserNotificationCenter path. Requires authorization, which we
      // request synchronously-best-effort and proceed regardless of the answer.
      run_on_main_sync_void(^{
        UNUserNotificationCenter* c = [UNUserNotificationCenter currentNotificationCenter];
        c.delegate = notification_delegate();
        [c requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                         completionHandler:^(BOOL, NSError*){
                         }];
        UNMutableNotificationContent* ct = [[UNMutableNotificationContent alloc] init];
        if (!opts.title.empty())
          ct.title = ns(opts.title);
        if (!opts.body.empty())
          ct.body = ns(opts.body);
        if (opts.attachment_path && !opts.attachment_path->empty())
          ct.sound = [UNNotificationSound soundNamed:ns(*opts.attachment_path)];
        NSMutableArray<UNNotificationAttachment*>* attachments = [NSMutableArray array];
        if (opts.image_path && !opts.image_path->empty()) {
          NSError* error = nil;
          UNNotificationAttachment* image = [UNNotificationAttachment
              attachmentWithIdentifier:@"image"
                                   URL:[NSURL fileURLWithPath:ns(*opts.image_path)]
                               options:nil
                                 error:&error];
          if (image)
            [attachments addObject:image];
        }
        if ([attachments count] > 0)
          ct.attachments = attachments;
        if (!opts.actions.empty()) {
          NSMutableArray<UNNotificationAction*>* actions = [NSMutableArray array];
          for (const auto& action : opts.actions) {
            if (action.id.empty() || action.title.empty())
              continue;
            NSString* actionId = ns(action.id);
            NSString* title = ns(action.title);
            if (action.kind == notification_action_kind::input) {
              [actions addObject:[UNTextInputNotificationAction
                                     actionWithIdentifier:actionId
                                                    title:title
                                                  options:UNNotificationActionOptionForeground
                                     textInputButtonTitle:@"Send"
                                     textInputPlaceholder:@""]];
            } else {
              [actions addObject:[UNNotificationAction
                                     actionWithIdentifier:actionId
                                                    title:title
                                                  options:UNNotificationActionOptionForeground]];
            }
          }
          if ([actions count] > 0) {
            NSString* categoryId = [NSString stringWithFormat:@"fxe-notif-category-%d", id];
            UNNotificationCategory* category = [UNNotificationCategory
                categoryWithIdentifier:categoryId
                               actions:actions
                     intentIdentifiers:@[]
                               options:UNNotificationCategoryOptionCustomDismissAction];
            [c setNotificationCategories:[NSSet setWithObject:category]];
            ct.categoryIdentifier = categoryId;
          }
        }
        NSString* identifier = [NSString stringWithFormat:@"fxe-notif-%d", id];
        UNNotificationRequest* req = [UNNotificationRequest requestWithIdentifier:identifier
                                                                          content:ct
                                                                          trigger:nil];
        [c addNotificationRequest:req
            withCompletionHandler:^(NSError*){
            }];
      });
    } else {
      // Unbundled fallback: synchronous NSAlert. Fire the click handler when
      // the user dismisses with the primary button.
      message_box_options m;
      m.title = opts.title;
      m.message = opts.body;
      std::vector<notification_action> fallback_actions;
      for (const auto& action : opts.actions) {
        if (!action.title.empty()) {
          m.buttons.push_back(action.title);
          fallback_actions.push_back(action);
        }
      }
      if (m.buttons.empty())
        m.buttons = {"OK"};
      int selected = show_message_box(m);
      post_main_thread_dispatch([id, selected, actions = std::move(fallback_actions)] {
        if (selected >= 0 && selected < static_cast<int>(actions.size()) &&
            !actions[static_cast<usize>(selected)].id.empty()) {
          dispatch_notification_action(id, actions[static_cast<usize>(selected)].id, std::nullopt);
          return;
        }
        dispatch_notification_click(id);
      });
    }
    return id;
  }

  int show_notification(
      const notification_options& opts,
      std::function<void(const std::string& action_id, std::optional<std::string> input)>
          on_action) {
    int id = show_notification(opts);
    if (id > 0 && on_action)
      on_notification_action(id, std::move(on_action));
    return id;
  }

  void on_notification_click(int id, std::function<void()> cb) {
    if (id <= 0)
      return;
    std::lock_guard<std::mutex> g(g_notif_mu);
    if (cb)
      g_notif_cbs[id] = std::move(cb);
    else
      g_notif_cbs.erase(id);
  }

  void on_notification_action(
      int id,
      std::function<void(const std::string& action_id, std::optional<std::string> input)> cb) {
    if (id <= 0)
      return;
    std::lock_guard<std::mutex> g(g_notif_mu);
    if (cb)
      g_notif_action_cbs[id] = std::move(cb);
    else
      g_notif_action_cbs.erase(id);
  }

  // ---- Menus --------------------------------------------------------------
  // FxeMenuTarget @class is forward-declared at file scope below this
  // namespace closure (Objective-C declarations require global scope).
} // namespace fxe::os

@interface FxeMenuTarget : NSObject
@property(nonatomic, copy) void (^block)(NSMenuItem*);
@end
@implementation FxeMenuTarget
- (void)fxeAction:(NSMenuItem*)sender {
  if (self.block)
    self.block(sender);
}
@end

namespace fxe::os {
  void handle_tray_click(int tray_id);
}

@interface FxeTrayTarget : NSObject
@property(nonatomic, assign) int trayId;
@end
@implementation FxeTrayTarget
- (void)_fxeTrayClick:(id)sender {
  (void)sender;
  fxe::os::handle_tray_click(self.trayId);
}
@end

namespace fxe::os {
  namespace {
    NSEventModifierFlags parse_mods(std::string_view& acc) {
      // Strip leading "Cmd+", "Ctrl+", "Shift+", "Alt+", "Option+" tokens.
      NSEventModifierFlags m = 0;
      while (!acc.empty()) {
        auto p = acc.find('+');
        if (p == std::string_view::npos)
          break;
        auto tok = acc.substr(0, p);
        std::string t(tok);
        for (auto& c : t)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "cmd" || t == "command" || t == "meta" || t == "super")
          m |= NSEventModifierFlagCommand;
        else if (t == "ctrl" || t == "control")
          m |= NSEventModifierFlagControl;
        else if (t == "shift")
          m |= NSEventModifierFlagShift;
        else if (t == "alt" || t == "option")
          m |= NSEventModifierFlagOption;
        else
          break;
        acc.remove_prefix(p + 1);
      }
      return m;
    }

    struct parsed_accelerator {
      NSEventModifierFlags modifiers = 0;
      std::string key;
    };

    parsed_accelerator parse_accelerator(std::string_view accelerator) {
      std::string owned(accelerator);
      std::string_view acc(owned);
      parsed_accelerator out;
      out.modifiers = parse_mods(acc);
      out.key.assign(acc.begin(), acc.end());
      for (auto& c : out.key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
    }

    void apply_accelerator(NSMenuItem* item, std::string_view accelerator) {
      auto parsed = parse_accelerator(accelerator);
      [item setKeyEquivalent:parsed.key.empty() ? @"" : ns(parsed.key)];
      [item setKeyEquivalentModifierMask:parsed.modifiers];
    }

    NSMutableDictionary<NSString*, NSMenuItem*>* app_menu_item_registry() {
      static NSMutableDictionary<NSString*, NSMenuItem*>* registry =
          [[NSMutableDictionary alloc] init];
      return registry;
    }

    NSMenu* build_menu(const std::vector<menu_item>& items, FxeMenuTarget* target,
                       std::unordered_map<NSInteger, std::string>* tag_to_id,
                       NSMutableDictionary<NSString*, NSMenuItem*>* item_registry) {
      static NSInteger s_next_tag = 1;
      NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
      [menu setAutoenablesItems:NO];
      for (auto& it : items) {
        if (it.type == "separator") {
          [menu addItem:[NSMenuItem separatorItem]];
          continue;
        }
        auto parsed = parse_accelerator(it.accelerator);
        NSMenuItem* mi =
            [[NSMenuItem alloc] initWithTitle:ns(it.label)
                                       action:nil
                                keyEquivalent:parsed.key.empty() ? @"" : ns(parsed.key)];
        [mi setKeyEquivalentModifierMask:parsed.modifiers];
        [mi setEnabled:it.enabled ? YES : NO];
        if (it.type == "checkbox")
          [mi setState:it.checked ? NSControlStateValueOn : NSControlStateValueOff];
        if (item_registry && !it.id.empty())
          [item_registry setObject:mi forKey:ns(it.id)];
        if (it.type == "submenu" || !it.submenu.empty()) {
          [mi setSubmenu:build_menu(it.submenu, target, tag_to_id, item_registry)];
        } else {
          NSInteger tag = ++s_next_tag;
          [mi setTag:tag];
          if (target) {
            [mi setTarget:target];
            [mi setAction:@selector(fxeAction:)];
          }
          if (tag_to_id)
            (*tag_to_id)[tag] = it.id;
        }
        [menu addItem:mi];
      }
      return menu;
    }
  } // namespace

  void set_application_menu(const std::vector<menu_item>& items) {
    run_on_main_sync_void(^{
      static FxeMenuTarget* s_target = [[FxeMenuTarget alloc] init];
      static std::unordered_map<NSInteger, std::string> s_tag_to_id;
      s_tag_to_id.clear();
      [app_menu_item_registry() removeAllObjects];
      s_target.block = ^(NSMenuItem* mi) {
        auto it = s_tag_to_id.find([mi tag]);
        if (it == s_tag_to_id.end())
          return;
        std::string id = it->second;
        post_main_thread_dispatch([id] {
          // Application menu has no JS callback by default; selection is
          // routed through the same notification-click registry by id.
          std::lock_guard<std::mutex> g(g_notif_mu);
          auto cbi = g_notif_cbs.find(0);
          (void)cbi;
          (void)id; // Reserved hook; integration may extend.
        });
      };
      NSMenu* m = build_menu(items, s_target, &s_tag_to_id, app_menu_item_registry());
      [NSApp setMainMenu:m];
    });
  }

  bool update_menu_item(std::string_view id, const menu_item_patch& patch) {
    if (id.empty())
      return false;
    __block bool ok = false;
    std::string key(id);
    run_on_main_sync_void(^{
      NSMenuItem* item = [app_menu_item_registry() objectForKey:ns(key)];
      if (!item)
        return;
      if (patch.label)
        [item setTitle:ns(*patch.label)];
      if (patch.enabled)
        [item setEnabled:*patch.enabled ? YES : NO];
      if (patch.checked)
        [item setState:*patch.checked ? NSControlStateValueOn : NSControlStateValueOff];
      if (patch.visible)
        [item setHidden:*patch.visible ? NO : YES];
      if (patch.accelerator)
        apply_accelerator(item, *patch.accelerator);
      ok = true;
    });
    return ok;
  }

  bool menu_item_exists(std::string_view id) {
    if (id.empty())
      return false;
    __block bool exists = false;
    std::string key(id);
    run_on_main_sync_void(^{
      exists = [app_menu_item_registry() objectForKey:ns(key)] != nil;
    });
    return exists;
  }

  void show_context_menu(const std::vector<menu_item>& items, int x, int y,
                         std::function<void(const std::string&)> on_select) {
    run_on_main_sync_void(^{
      auto cb = std::make_shared<std::function<void(const std::string&)>>(on_select);
      auto tag_map = std::make_shared<std::unordered_map<NSInteger, std::string>>();
      FxeMenuTarget* target = [[FxeMenuTarget alloc] init];
      target.block = ^(NSMenuItem* mi) {
        auto it = tag_map->find([mi tag]);
        std::string id = (it == tag_map->end()) ? std::string{} : it->second;
        auto cbref = cb;
        post_main_thread_dispatch([cbref, id] {
          if (*cbref)
            (*cbref)(id);
        });
      };
      NSMenu* m = build_menu(items, target, tag_map.get(), nil);
      NSPoint loc = NSMakePoint(static_cast<CGFloat>(x), static_cast<CGFloat>(y));
      BOOL clicked = [m popUpMenuPositioningItem:nil atLocation:loc inView:nil];
      if (!clicked) {
        auto cbref = cb;
        post_main_thread_dispatch([cbref] {
          if (*cbref)
            (*cbref)(std::string{});
        });
      }
    });
  }

  // ---- Tray ---------------------------------------------------------------
  namespace {
    struct tray_listener {
      int token = -1;
      std::function<void()> cb;
    };

    struct tray_entry {
      NSStatusItem* item = nil;
      NSMenu* menu = nil;
      FxeMenuTarget* target = nil;
      FxeTrayTarget* click_target = nil;
      std::shared_ptr<std::unordered_map<NSInteger, std::string>> tag_map;
      std::array<std::vector<tray_listener>, 3> listeners;
    };

    std::mutex g_tray_mu;
    std::unordered_map<int, tray_entry> g_trays;
    std::atomic<int> g_tray_seq{0};
    std::atomic<int> g_tray_listener_seq{0};

    usize tray_event_index(tray_event_kind kind) {
      switch (kind) {
      case tray_event_kind::click:
        return 0;
      case tray_event_kind::right_click:
        return 1;
      case tray_event_kind::double_click:
        return 2;
      }
      return 0;
    }

    void configure_tray_button(NSStatusItem* item, FxeTrayTarget* target) {
      if (!item || !item.button)
        return;
      item.button.target = target;
      item.button.action = @selector(_fxeTrayClick:);
      [item.button sendActionOn:(NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp)];
    }
  } // namespace

  void handle_tray_click(int tray_id) {
    NSEvent* event = [NSApp currentEvent];
    NSEventType type = event ? [event type] : NSEventTypeLeftMouseUp;
    NSInteger button = event ? [event buttonNumber] : 0;
    bool is_right = type == NSEventTypeRightMouseUp;
    if (type != NSEventTypeLeftMouseUp && type != NSEventTypeRightMouseUp && event)
      is_right = button == 1 || button == 2;
    bool is_double = !is_right && event && [event clickCount] >= 2;
    tray_event_kind kind =
        is_right ? tray_event_kind::right_click
                 : (is_double ? tray_event_kind::double_click : tray_event_kind::click);

    std::vector<std::function<void()>> callbacks;
    NSStatusItem* item = nil;
    NSMenu* menu = nil;
    {
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(tray_id);
      if (it == g_trays.end())
        return;
      auto& entry = it->second;
      for (auto& listener : entry.listeners[tray_event_index(kind)])
        callbacks.push_back(listener.cb);
      if (!is_right) {
        item = entry.item;
        menu = entry.menu;
      }
    }

    for (auto& cb : callbacks)
      if (cb)
        post_main_thread_dispatch(cb);

    // We keep tray menus action-driven rather than assigning NSStatusItem.menu:
    // assigning the menu suppresses button actions on macOS, so left-clicks
    // dispatch listeners and then pop the stored menu manually. Right-clicks
    // dispatch only right-click listeners.
    if (!is_right && item && menu) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      [item popUpStatusItemMenu:menu];
#pragma clang diagnostic pop
    }
  }

  tray_handle tray_create(std::string_view icon_path, std::string_view tooltip) {
    __block tray_handle out{};
    int id = ++g_tray_seq;
    NSString* icon = ns(icon_path);
    NSString* tip = ns(tooltip);
    run_on_main_sync_void(^{
      NSStatusItem* si =
          [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
      if (icon.length > 0) {
        NSImage* img = [[NSImage alloc] initWithContentsOfFile:icon];
        if (img) {
          [img setTemplate:YES];
          [img setSize:NSMakeSize(18, 18)];
          si.button.image = img;
        }
      }
      if (tip.length > 0)
        si.button.toolTip = tip;
      tray_entry e;
      e.item = si;
      e.target = [[FxeMenuTarget alloc] init];
      e.click_target = [[FxeTrayTarget alloc] init];
      e.click_target.trayId = id;
      e.tag_map = std::make_shared<std::unordered_map<NSInteger, std::string>>();
      configure_tray_button(si, e.click_target);
      {
        std::lock_guard<std::mutex> g(g_tray_mu);
        g_trays.emplace(id, std::move(e));
      }
      out = tray_handle{id};
    });
    return out;
  }

  void tray_set_menu(tray_handle h, const std::vector<menu_item>& items) {
    if (!h)
      return;
    int id = h.id;
    run_on_main_sync_void(^{
      tray_entry* e = nullptr;
      {
        std::lock_guard<std::mutex> g(g_tray_mu);
        auto it = g_trays.find(id);
        if (it == g_trays.end())
          return;
        e = &it->second;
      }
      e->tag_map->clear();
      auto tag_map = e->tag_map;
      e->target.block = ^(NSMenuItem* mi) {
        auto it = tag_map->find([mi tag]);
        if (it == tag_map->end())
          return;
        // Tray menu selection callbacks are reserved for future binding work.
        (void)it;
      };
      NSMenu* m = build_menu(items, e->target, e->tag_map.get(), nil);
      e->menu = m;
      [e->item setMenu:nil];
      configure_tray_button(e->item, e->click_target);
    });
  }

  void tray_destroy(tray_handle h) {
    if (!h)
      return;
    int id = h.id;
    run_on_main_sync_void(^{
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(id);
      if (it == g_trays.end())
        return;
      [[NSStatusBar systemStatusBar] removeStatusItem:it->second.item];
      g_trays.erase(it);
    });
  }

  bool tray_set_image(tray_handle h, std::string_view icon_path) {
    if (!h)
      return false;
    __block bool ok = false;
    int id = h.id;
    NSString* icon = ns(icon_path);
    run_on_main_sync_void(^{
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(id);
      if (it == g_trays.end() || !it->second.item.button)
        return;
      NSImage* img = [[NSImage alloc] initWithContentsOfFile:icon];
      if (!img)
        return;
      [img setTemplate:YES];
      [img setSize:NSMakeSize(18, 18)];
      it->second.item.button.image = img;
      ok = true;
    });
    return ok;
  }

  bool tray_set_title(tray_handle h, std::string_view title) {
    if (!h)
      return false;
    __block bool ok = false;
    int id = h.id;
    NSString* text = ns(title);
    run_on_main_sync_void(^{
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(id);
      if (it == g_trays.end() || !it->second.item.button)
        return;
      it->second.item.button.title = text;
      ok = true;
    });
    return ok;
  }

  bool tray_set_tooltip(tray_handle h, std::string_view tip) {
    if (!h)
      return false;
    __block bool ok = false;
    int id = h.id;
    NSString* text = ns(tip);
    run_on_main_sync_void(^{
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(id);
      if (it == g_trays.end() || !it->second.item.button)
        return;
      it->second.item.button.toolTip = text;
      ok = true;
    });
    return ok;
  }

  int tray_on(tray_handle h, tray_event_kind kind, std::function<void()> cb) {
    if (!h || !cb)
      return -1;
    __block int token = -1;
    int id = h.id;
    auto callback = std::make_shared<std::function<void()>>(std::move(cb));
    run_on_main_sync_void(^{
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(id);
      if (it == g_trays.end())
        return;
      token = ++g_tray_listener_seq;
      it->second.listeners[tray_event_index(kind)].push_back(tray_listener{token, *callback});
      configure_tray_button(it->second.item, it->second.click_target);
    });
    return token;
  }

  void tray_off(tray_handle h, int token) {
    if (!h || token < 0)
      return;
    int id = h.id;
    run_on_main_sync_void(^{
      std::lock_guard<std::mutex> g(g_tray_mu);
      auto it = g_trays.find(id);
      if (it == g_trays.end())
        return;
      for (auto& listeners : it->second.listeners) {
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                       [token](const tray_listener& listener) {
                                         return listener.token == token;
                                       }),
                        listeners.end());
      }
    });
  }

  // ---- Carbon global hotkeys ---------------------------------------------
  namespace {
    struct hotkey_entry {
      EventHotKeyRef ref = nullptr;
      UInt32 id = 0;
      std::string accelerator;
      std::function<void()> cb;
    };
    std::mutex g_hk_mu;
    std::unordered_map<UInt32, hotkey_entry> g_hk_by_id;
    std::unordered_map<std::string, UInt32> g_hk_by_acc;
    std::atomic<UInt32> g_hk_seq{1};
    EventHandlerRef g_hk_handler_ref = nullptr;

    OSStatus hk_handler(EventHandlerCallRef, EventRef ev, void*) {
      EventHotKeyID hkid;
      if (GetEventParameter(ev, kEventParamDirectObject, typeEventHotKeyID, NULL, sizeof(hkid),
                            NULL, &hkid) != noErr)
        return noErr;
      std::function<void()> cb;
      {
        std::lock_guard<std::mutex> g(g_hk_mu);
        auto it = g_hk_by_id.find(hkid.id);
        if (it != g_hk_by_id.end())
          cb = it->second.cb;
      }
      if (cb)
        post_main_thread_dispatch(std::move(cb));
      return noErr;
    }

    void ensure_hk_handler() {
      if (g_hk_handler_ref)
        return;
      EventTypeSpec spec{kEventClassKeyboard, kEventHotKeyPressed};
      InstallApplicationEventHandler(&hk_handler, 1, &spec, nullptr, &g_hk_handler_ref);
    }

    // Map a single key character/name to a Carbon virtual key code.
    // Subset is sufficient for typical accelerators; unknown returns 0xFFFF.
    UInt32 map_keycode(std::string_view k) {
      std::string s(k);
      for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      static const std::pair<const char*, UInt32> table[] = {
          {"a", kVK_ANSI_A},       {"b", kVK_ANSI_B},
          {"c", kVK_ANSI_C},       {"d", kVK_ANSI_D},
          {"e", kVK_ANSI_E},       {"f", kVK_ANSI_F},
          {"g", kVK_ANSI_G},       {"h", kVK_ANSI_H},
          {"i", kVK_ANSI_I},       {"j", kVK_ANSI_J},
          {"k", kVK_ANSI_K},       {"l", kVK_ANSI_L},
          {"m", kVK_ANSI_M},       {"n", kVK_ANSI_N},
          {"o", kVK_ANSI_O},       {"p", kVK_ANSI_P},
          {"q", kVK_ANSI_Q},       {"r", kVK_ANSI_R},
          {"s", kVK_ANSI_S},       {"t", kVK_ANSI_T},
          {"u", kVK_ANSI_U},       {"v", kVK_ANSI_V},
          {"w", kVK_ANSI_W},       {"x", kVK_ANSI_X},
          {"y", kVK_ANSI_Y},       {"z", kVK_ANSI_Z},
          {"0", kVK_ANSI_0},       {"1", kVK_ANSI_1},
          {"2", kVK_ANSI_2},       {"3", kVK_ANSI_3},
          {"4", kVK_ANSI_4},       {"5", kVK_ANSI_5},
          {"6", kVK_ANSI_6},       {"7", kVK_ANSI_7},
          {"8", kVK_ANSI_8},       {"9", kVK_ANSI_9},
          {"f1", kVK_F1},          {"f2", kVK_F2},
          {"f3", kVK_F3},          {"f4", kVK_F4},
          {"f5", kVK_F5},          {"f6", kVK_F6},
          {"f7", kVK_F7},          {"f8", kVK_F8},
          {"f9", kVK_F9},          {"f10", kVK_F10},
          {"f11", kVK_F11},        {"f12", kVK_F12},
          {"space", kVK_Space},    {"return", kVK_Return},
          {"enter", kVK_Return},   {"tab", kVK_Tab},
          {"escape", kVK_Escape},  {"esc", kVK_Escape},
          {"delete", kVK_Delete},  {"backspace", kVK_Delete},
          {"left", kVK_LeftArrow}, {"right", kVK_RightArrow},
          {"up", kVK_UpArrow},     {"down", kVK_DownArrow},
      };
      for (auto& [k0, v] : table)
        if (s == k0)
          return v;
      return 0xFFFF;
    }

    UInt32 carbon_mods_from(std::string_view& acc) {
      UInt32 m = 0;
      while (!acc.empty()) {
        auto p = acc.find('+');
        if (p == std::string_view::npos)
          break;
        std::string t(acc.substr(0, p));
        for (auto& c : t)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "cmd" || t == "command" || t == "meta" || t == "super")
          m |= cmdKey;
        else if (t == "ctrl" || t == "control")
          m |= controlKey;
        else if (t == "shift")
          m |= shiftKey;
        else if (t == "alt" || t == "option")
          m |= optionKey;
        else
          break;
        acc.remove_prefix(p + 1);
      }
      return m;
    }
  } // namespace

  bool global_shortcut_register(std::string_view accelerator, std::function<void()> cb) {
    ensure_hk_handler();
    std::string key(accelerator);
    std::string_view acc(key);
    UInt32 mods = carbon_mods_from(acc);
    UInt32 vk = map_keycode(acc);
    if (vk == 0xFFFF)
      return false;
    UInt32 id = g_hk_seq.fetch_add(1);
    EventHotKeyID hkid{'fxhk', id};
    EventHotKeyRef ref = nullptr;
    if (RegisterEventHotKey(vk, mods, hkid, GetApplicationEventTarget(), 0, &ref) != noErr)
      return false;
    std::lock_guard<std::mutex> g(g_hk_mu);
    auto& e = g_hk_by_id[id];
    e.ref = ref;
    e.id = id;
    e.accelerator = key;
    e.cb = std::move(cb);
    g_hk_by_acc[key] = id;
    return true;
  }

  void global_shortcut_unregister(std::string_view accelerator) {
    std::string key(accelerator);
    std::lock_guard<std::mutex> g(g_hk_mu);
    auto it = g_hk_by_acc.find(key);
    if (it == g_hk_by_acc.end())
      return;
    auto idit = g_hk_by_id.find(it->second);
    if (idit != g_hk_by_id.end()) {
      if (idit->second.ref)
        UnregisterEventHotKey(idit->second.ref);
      g_hk_by_id.erase(idit);
    }
    g_hk_by_acc.erase(it);
  }

  void global_shortcut_unregister_all() {
    std::lock_guard<std::mutex> g(g_hk_mu);
    for (auto& [_, e] : g_hk_by_id)
      if (e.ref)
        UnregisterEventHotKey(e.ref);
    g_hk_by_id.clear();
    g_hk_by_acc.clear();
  }

  // ---- Clipboard formats --------------------------------------------------
  bool clipboard_set_html(std::string_view utf8) {
    std::string copy(utf8);
    __block bool ok = false;
    run_on_main_sync_void(^{
      NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
      NSData* data = data_from_bytes(reinterpret_cast<const u8*>(copy.data()), copy.size());
      if (!pasteboard || !data)
        return;
      [pasteboard clearContents];
      ok = [pasteboard setData:data forType:NSPasteboardTypeHTML] == YES;
    });
    return ok;
  }

  std::optional<std::string> clipboard_get_html() {
    __block std::optional<std::string> out;
    run_on_main_sync_void(^{
      NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
      NSData* data = pasteboard ? [pasteboard dataForType:NSPasteboardTypeHTML] : nil;
      if (!data)
        return;
      out = std::string(static_cast<const char*>(data.bytes), static_cast<usize>(data.length));
    });
    return out;
  }

  bool clipboard_set_rtf(std::string_view rtf) {
    std::string copy(rtf);
    __block bool ok = false;
    run_on_main_sync_void(^{
      NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
      NSData* data = data_from_bytes(reinterpret_cast<const u8*>(copy.data()), copy.size());
      if (!pasteboard || !data)
        return;
      [pasteboard clearContents];
      ok = [pasteboard setData:data forType:NSPasteboardTypeRTF] == YES;
    });
    return ok;
  }

  std::optional<std::string> clipboard_get_rtf() {
    __block std::optional<std::string> out;
    run_on_main_sync_void(^{
      NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
      NSData* data = pasteboard ? [pasteboard dataForType:NSPasteboardTypeRTF] : nil;
      if (!data)
        return;
      out = std::string(static_cast<const char*>(data.bytes), static_cast<usize>(data.length));
    });
    return out;
  }

  bool clipboard_set_mime(std::string_view mime, const std::vector<u8>& bytes) {
    std::string mime_copy(mime);
    std::vector<u8> data_copy(bytes);
    __block bool ok = false;
    run_on_main_sync_void(^{
      NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
      NSString* type = pasteboard_type_for_mime(mime_copy);
      NSData* data = data_from_bytes(data_copy.data(), data_copy.size());
      if (!pasteboard || !type || !data)
        return;
      [pasteboard clearContents];
      ok = [pasteboard setData:data forType:type] == YES;
    });
    return ok;
  }

  std::optional<std::vector<u8>> clipboard_get_mime(std::string_view mime) {
    std::string mime_copy(mime);
    __block std::optional<std::vector<u8>> out;
    run_on_main_sync_void(^{
      NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
      NSString* type = pasteboard_type_for_mime(mime_copy);
      NSData* data = (pasteboard && type) ? [pasteboard dataForType:type] : nil;
      if (!data)
        return;
      out = bytes_from_nsdata(data);
    });
    return out;
  }

  // ---- Main-thread dispatch ----------------------------------------------
  void post_main_thread_dispatch(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(g_dispatch_mu);
    g_dispatch_q.push(std::move(fn));
  }

  void pump_main_thread_dispatches() {
    for (;;) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> g(g_dispatch_mu);
        if (g_dispatch_q.empty())
          break;
        fn = std::move(g_dispatch_q.front());
        g_dispatch_q.pop();
      }
      if (fn)
        fn();
    }
  }

  // ---- NEW: single-instance handoff / deep-link / file-open helpers -------
  namespace single_instance_detail {
    std::string encode_handoff(int argc, char** argv);
    bool decode_handoff(std::string_view data, std::vector<std::string>& argv, std::string& cwd);
    void dispatch_launch(std::vector<std::string> argv, std::string cwd);
    void dispatch_open_url(std::string url);
    void dispatch_open_file(std::string path);
  } // namespace single_instance_detail

  namespace {
    std::atomic<bool> g_single_instance_listener_started{false};
    std::atomic<bool> g_apple_event_handlers_installed{false};

    std::string sanitize_single_instance_component(std::string value) {
      if (value.empty())
        value = "fxe";
      for (char& ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '.' && ch != '_' && ch != '-')
          ch = '_';
      }
      return value;
    }

    std::string mac_bundle_id() {
      std::string id = from_ns([[NSBundle mainBundle] bundleIdentifier]);
      if (!id.empty())
        return sanitize_single_instance_component(id);
      char exe[1024];
      u32 size = sizeof(exe);
      if (_NSGetExecutablePath(exe, &size) == 0) {
        NSString* name =
            [[[NSString stringWithUTF8String:exe] lastPathComponent] stringByDeletingPathExtension];
        id = from_ns(name);
      }
      return sanitize_single_instance_component(id);
    }

    std::string mac_single_instance_socket_path() {
      std::string base = get_path("userData");
      if (base.empty())
        return {};
      std::string id = mac_bundle_id();
      std::string dir = base + "/" + id;
      [[NSFileManager defaultManager] createDirectoryAtPath:ns(dir)
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
      return dir + "/" + id + ".sock";
    }

    bool fill_unix_addr(const std::string& path, sockaddr_un& addr) {
      if (path.empty() || path.size() >= sizeof(addr.sun_path))
        return false;
      addr = {};
      addr.sun_family = AF_UNIX;
      for (usize i = 0; i < path.size(); ++i)
        addr.sun_path[i] = path[i];
      addr.sun_path[path.size()] = '\0';
      return true;
    }

    bool write_all_fd(int fd, const std::string& payload) {
      const char* data = payload.data();
      usize left = payload.size();
      while (left > 0) {
        ssize_t n = ::write(fd, data, left);
        if (n < 0)
          return false;
        data += n;
        left -= static_cast<usize>(n);
      }
      return true;
    }

    bool forward_to_primary_socket(const std::string& path, const std::string& payload) {
      sockaddr_un addr{};
      if (!fill_unix_addr(path, addr))
        return false;
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0)
        return false;
      bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                write_all_fd(fd, payload);
      ::close(fd);
      return ok;
    }

    void handle_single_instance_client(int client) {
      std::string payload;
      char buffer[4096];
      for (;;) {
        ssize_t n = ::read(client, buffer, sizeof(buffer));
        if (n > 0) {
          payload.append(buffer, static_cast<usize>(n));
          continue;
        }
        break;
      }
      ::close(client);
      std::vector<std::string> argv;
      std::string cwd;
      if (single_instance_detail::decode_handoff(payload, argv, cwd))
        single_instance_detail::dispatch_launch(std::move(argv), std::move(cwd));
    }

    void start_single_instance_socket_listener(const std::string& path) {
      if (path.empty() || g_single_instance_listener_started.exchange(true))
        return;
      std::thread([path] {
        sockaddr_un addr{};
        if (!fill_unix_addr(path, addr))
          return;
        int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (server < 0)
          return;
        ::unlink(path.c_str());
        if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(server, 16) != 0) {
          ::close(server);
          return;
        }
        for (;;) {
          int client = ::accept(server, nullptr, nullptr);
          if (client >= 0)
            handle_single_instance_client(client);
        }
      }).detach();
    }

    OSErr handle_get_url_event(const AppleEvent* event, AppleEvent*, SRefCon) {
      Size size = 0;
      OSStatus status =
          AEGetParamPtr(event, keyDirectObject, typeUTF8Text, nullptr, nullptr, 0, &size);
      if (status != errAEBufferTooSmall && status != noErr)
        return static_cast<OSErr>(status);
      if (size <= 0)
        return noErr;
      std::vector<char> bytes(static_cast<usize>(size));
      status =
          AEGetParamPtr(event, keyDirectObject, typeUTF8Text, nullptr, bytes.data(), size, &size);
      if (status == noErr)
        single_instance_detail::dispatch_open_url(
            std::string(bytes.data(), static_cast<usize>(size)));
      return static_cast<OSErr>(status);
    }

    OSErr handle_open_documents_event(const AppleEvent* event, AppleEvent*, SRefCon) {
      AEDescList docs{};
      OSStatus status = AEGetParamDesc(event, keyDirectObject, typeAEList, &docs);
      if (status != noErr)
        return static_cast<OSErr>(status);
      long count = 0;
      status = AECountItems(&docs, &count);
      if (status == noErr) {
        for (long i = 1; i <= count; ++i) {
          AEDesc item{};
          if (AEGetNthDesc(&docs, i, typeFileURL, nullptr, &item) != noErr)
            continue;
          Size size = AEGetDescDataSize(&item);
          if (size > 0) {
            std::vector<char> bytes(static_cast<usize>(size));
            if (AEGetDescData(&item, bytes.data(), size) == noErr) {
              std::string url(bytes.data(), static_cast<usize>(size));
              NSURL* nsurl = [NSURL URLWithString:ns(url)];
              std::string path = from_ns([nsurl path]);
              if (!path.empty())
                single_instance_detail::dispatch_open_file(std::move(path));
            }
          }
          AEDisposeDesc(&item);
        }
      }
      AEDisposeDesc(&docs);
      return static_cast<OSErr>(status);
    }

    bool valid_scheme(std::string_view scheme) {
      if (scheme.empty() || !std::isalpha(static_cast<unsigned char>(scheme.front())))
        return false;
      for (char ch : scheme) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '+' && ch != '-' && ch != '.')
          return false;
      }
      return true;
    }
  } // namespace

  bool single_instance_platform_acquire_or_forward(int argc, char** argv) {
    std::string id = mac_bundle_id();
    std::string path = mac_single_instance_socket_path();
    if (request_single_instance_lock(id)) {
      start_single_instance_socket_listener(path);
      return true;
    }
    (void)forward_to_primary_socket(path, single_instance_detail::encode_handoff(argc, argv));
    return false;
  }

  void single_instance_platform_install_open_handlers() {
    if (g_apple_event_handlers_installed.exchange(true))
      return;
    static AEEventHandlerUPP get_url_handler = NewAEEventHandlerUPP(handle_get_url_event);
    static AEEventHandlerUPP open_docs_handler = NewAEEventHandlerUPP(handle_open_documents_event);
    AEInstallEventHandler(kInternetEventClass, kAEGetURL, get_url_handler, 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, open_docs_handler, 0, false);
  }

  bool single_instance_platform_set_default_protocol_client(const std::string& scheme) {
    if (!valid_scheme(scheme))
      return false;
    NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
    if (!bundle)
      return false;
    NSString* s = ns(scheme);
    return LSSetDefaultHandlerForURLScheme((__bridge CFStringRef)s, (__bridge CFStringRef)bundle) ==
           noErr;
  }

  bool single_instance_platform_set_default_file_handler(const std::string& ext) {
    NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
    if (!bundle || ext.empty())
      return false;
    std::string clean = ext.front() == '.' ? ext.substr(1) : ext;
    if (clean.empty())
      return false;
    NSString* extension = ns(clean);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFStringRef uti = UTTypeCreatePreferredIdentifierForTag(
        kUTTagClassFilenameExtension, (__bridge CFStringRef)extension, nullptr);
#pragma clang diagnostic pop
    if (!uti)
      return false;
    OSStatus status =
        LSSetDefaultRoleHandlerForContentType(uti, kLSRolesAll, (__bridge CFStringRef)bundle);
    CFRelease(uti);
    return status == noErr;
  }
} // namespace fxe::os
