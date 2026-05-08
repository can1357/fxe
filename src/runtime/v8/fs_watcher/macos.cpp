#include "runtime/v8/fs_watcher.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fxe/types.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fxe::runtime {
  fs_watcher::~fs_watcher() = default;

  namespace {
    namespace fs = std::filesystem;

    std::string cf_string_to_utf8(CFStringRef value) {
      if (value == nullptr)
        return {};
      const CFIndex length = CFStringGetLength(value);
      const CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
      if (max_size <= 0)
        return {};

      std::string out(static_cast<usize>(max_size), '\0');
      if (!CFStringGetCString(value, out.data(), max_size, kCFStringEncodingUTF8))
        return {};
      out.resize(std::strlen(out.c_str()));
      return out;
    }

    fs::path absolute_path(const std::string& path) {
      std::error_code ec;
      auto out = fs::absolute(fs::path(path), ec);
      if (ec)
        out = fs::path(path);
      return out.lexically_normal();
    }

    bool existing_directory(const fs::path& path) {
      std::error_code ec;
      const bool is_dir = fs::is_directory(path, ec);
      return is_dir && !ec;
    }

    struct path_snapshot {
      bool exists = false;
      fs::file_type type = fs::file_type::none;
      std::uintmax_t size = 0;
      fs::file_time_type write_time{};
    };

    std::optional<path_snapshot> snapshot_path(const fs::path& path) {
      std::error_code ec;
      const bool exists = fs::exists(path, ec);
      if (ec)
        return std::nullopt;

      path_snapshot snapshot{};
      snapshot.exists = exists;
      if (!exists)
        return snapshot;

      snapshot.type = fs::status(path, ec).type();
      if (ec)
        return std::nullopt;

      snapshot.write_time = fs::last_write_time(path, ec);
      if (ec)
        return std::nullopt;

      if (snapshot.type == fs::file_type::regular) {
        snapshot.size = fs::file_size(path, ec);
        if (ec)
          return std::nullopt;
      }

      return snapshot;
    }

    bool same_snapshot(const path_snapshot& a, const path_snapshot& b) {
      return a.exists == b.exists && a.type == b.type && a.size == b.size &&
             a.write_time == b.write_time;
    }

    class fsevents_fs_watcher final : public fs_watcher {
    public:
      fsevents_fs_watcher(std::string path, bool recursive,
                          std::function<void(const fs_watch_event&)> cb)
          : requested_path_(absolute_path(path)), recursive_(recursive), cb_(std::move(cb)) {
        const bool is_dir = existing_directory(requested_path_);
        if (is_dir) {
          watch_root_ = requested_path_;
          filter_single_file_ = false;
        } else {
          watch_root_ = requested_path_.parent_path();
          if (watch_root_.empty())
            watch_root_ = fs::path(".");
          filter_single_file_ = true;
        }
        remember_snapshot(requested_path_);
        if (recursive_ && !filter_single_file_)
          remember_existing_descendants(requested_path_);

        thread_ = std::thread([this] { run(); });

        std::unique_lock lock(mutex_);
        ready_cv_.wait(lock, [this] { return ready_; });
      }

      ~fsevents_fs_watcher() override {
        close();
      }

      bool active() const {
        return active_.load();
      }

      void close() override {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true))
          return;

        CFRunLoopRef run_loop = nullptr;
        {
          std::lock_guard lock(mutex_);
          run_loop = run_loop_;
          if (run_loop != nullptr)
            CFRetain(run_loop);
        }

        if (run_loop != nullptr) {
          CFRunLoopStop(run_loop);
          CFRunLoopWakeUp(run_loop);
          CFRelease(run_loop);
        }

        if (thread_.joinable()) {
          if (thread_.get_id() == std::this_thread::get_id())
            thread_.detach();
          else
            thread_.join();
        }
      }

    private:
      static void events_callback(ConstFSEventStreamRef /* stream */, void* client_info,
                                  usize num_events, void* event_paths,
                                  const FSEventStreamEventFlags event_flags[],
                                  const FSEventStreamEventId[] /* event_ids */) {
        auto* self = static_cast<fsevents_fs_watcher*>(client_info);
        if (self == nullptr || self->closed_.load())
          return;

        auto paths = static_cast<CFArrayRef>(event_paths);
        for (usize i = 0; i < num_events; ++i) {
          auto path =
              static_cast<CFStringRef>(CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
          self->handle_event(cf_string_to_utf8(path), event_flags[i]);
        }
      }

      void run() {
        const std::string watch_root = watch_root_.generic_string();
        CFStringRef root =
            CFStringCreateWithCString(nullptr, watch_root.c_str(), kCFStringEncodingUTF8);
        if (root == nullptr) {
          mark_ready(false, nullptr);
          return;
        }

        const void* values[] = {root};
        CFArrayRef paths = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
        CFRelease(root);
        if (paths == nullptr) {
          mark_ready(false, nullptr);
          return;
        }

        FSEventStreamContext context{};
        context.info = this;
        constexpr FSEventStreamCreateFlags flags =
            kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagNoDefer |
            kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagFileEvents;
        FSEventStreamRef stream = FSEventStreamCreate(nullptr, events_callback, &context, paths,
                                                      kFSEventStreamEventIdSinceNow, 0.05, flags);
        CFRelease(paths);
        if (stream == nullptr) {
          mark_ready(false, nullptr);
          return;
        }

        CFRunLoopRef loop = CFRunLoopGetCurrent();
        CFRetain(loop);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        FSEventStreamScheduleWithRunLoop(stream, loop, kCFRunLoopDefaultMode);
#pragma clang diagnostic pop
        const bool started = FSEventStreamStart(stream);
        if (!started) {
          FSEventStreamInvalidate(stream);
          FSEventStreamRelease(stream);
          CFRelease(loop);
          mark_ready(false, nullptr);
          return;
        }

        mark_ready(true, loop);
        if (!closed_.load())
          CFRunLoopRun();

        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);

        {
          std::lock_guard lock(mutex_);
          if (run_loop_ == loop)
            run_loop_ = nullptr;
        }
        CFRelease(loop);
        active_.store(false);
      }

      void mark_ready(bool active, CFRunLoopRef loop) {
        {
          std::lock_guard lock(mutex_);
          active_.store(active);
          run_loop_ = loop;
          ready_ = true;
        }
        ready_cv_.notify_all();
      }

      bool should_deliver(const std::string& event_path) const {
        if (event_path.empty())
          return false;

        const fs::path event = fs::path(event_path).lexically_normal();
        if (filter_single_file_)
          return event == requested_path_ || event.filename() == requested_path_.filename();

        if (recursive_)
          return event != watch_root_;
        return event.parent_path() == watch_root_;
      }

      void remember_snapshot(const fs::path& path) {
        if (auto snapshot = snapshot_path(path))
          snapshots_[path.generic_string()] = *snapshot;
      }

      void remember_existing_descendants(const fs::path& root) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
          remember_snapshot(it->path().lexically_normal());
      }

      static bool is_descendant_of(const fs::path& path, const fs::path& root) {
        const fs::path relative = path.lexically_relative(root);
        return !relative.empty() && *relative.begin() != "..";
      }

      std::optional<fs::path> changed_descendant(const fs::path& root) const {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
          const fs::path candidate = it->path().lexically_normal();
          const auto current = snapshot_path(candidate);
          if (!current)
            continue;
          const auto previous_it = snapshots_.find(candidate.generic_string());
          if (previous_it == snapshots_.end() || !same_snapshot(previous_it->second, *current))
            return candidate;
        }

        for (const auto& [key, previous] : snapshots_) {
          if (!previous.exists)
            continue;
          const fs::path candidate = fs::path(key).lexically_normal();
          if (!is_descendant_of(candidate, root))
            continue;
          const auto current = snapshot_path(candidate);
          if (current && !current->exists)
            return candidate;
        }

        return std::nullopt;
      }

      fs_watch_event::kind classify_event(const fs::path& event, FSEventStreamEventFlags flags) {
        const std::string key = event.generic_string();
        const auto previous_it = snapshots_.find(key);
        std::optional<path_snapshot> previous;
        if (previous_it != snapshots_.end())
          previous = previous_it->second;
        const auto current = snapshot_path(event);
        if (current)
          snapshots_[key] = *current;

        if (previous && current) {
          if (previous->exists != current->exists)
            return current->exists ? fs_watch_event::kind::created : fs_watch_event::kind::deleted;
          if (previous->exists && !same_snapshot(*previous, *current))
            return fs_watch_event::kind::changed;
        }

        if ((flags & kFSEventStreamEventFlagItemRemoved) != 0)
          return fs_watch_event::kind::deleted;
        if ((flags & kFSEventStreamEventFlagItemRenamed) != 0)
          return fs_watch_event::kind::renamed;
        if ((flags & kFSEventStreamEventFlagItemCreated) != 0)
          return fs_watch_event::kind::created;
        if ((flags &
             (kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemInodeMetaMod |
              kFSEventStreamEventFlagItemXattrMod)) != 0)
          return fs_watch_event::kind::changed;
        return fs_watch_event::kind::changed;
      }

      fs::path delivery_path_for(const fs::path& event) const {
        if (!recursive_ || filter_single_file_)
          return event;
        std::error_code ec;
        if (!fs::is_directory(event, ec) || ec)
          return event;
        if (auto descendant = changed_descendant(event))
          return *descendant;
        return event;
      }

      void handle_event(const std::string& event_path, FSEventStreamEventFlags flags) {
        if (closed_.load() || !should_deliver(event_path))
          return;
        const fs::path event = fs::path(event_path).lexically_normal();
        const fs::path delivery_path = delivery_path_for(event);
        cb_(fs_watch_event{delivery_path.generic_string(), classify_event(delivery_path, flags)});
      }

      fs::path requested_path_;
      fs::path watch_root_;
      bool recursive_ = false;
      bool filter_single_file_ = false;
      std::unordered_map<std::string, path_snapshot> snapshots_;
      std::function<void(const fs_watch_event&)> cb_;
      std::atomic<bool> closed_{false};
      std::atomic<bool> active_{false};
      mutable std::mutex mutex_;
      std::condition_variable ready_cv_;
      bool ready_ = false;
      CFRunLoopRef run_loop_ = nullptr;
      std::thread thread_;
    };
  } // namespace

  std::unique_ptr<fs_watcher> fs_watcher::create(const std::string& path, bool recursive,
                                                 std::function<void(const fs_watch_event&)> cb,
                                                 fs_watch_error* error) {
    auto watcher = std::make_unique<fsevents_fs_watcher>(path, recursive, std::move(cb));
    if (!watcher->active()) {
      if (error != nullptr) {
        error->code = "EIO";
        error->errno_value = 0;
        error->syscall = "FSEventStreamStart";
        error->path = path;
        error->message = "FSEventStreamStart: failed to start watcher '" + path + "'";
      }
      return nullptr;
    }
    return watcher;
  }

} // namespace fxe::runtime
