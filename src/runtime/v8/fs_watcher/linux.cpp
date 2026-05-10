#include "runtime/v8/fs_watcher.hpp"

#include <fxe/log.hpp>
#include <fxe/types.hpp>
#include <utility>

#if defined(__linux__)
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#endif

namespace fxe::runtime {
  fs_watcher::~fs_watcher() = default;

#if defined(__linux__)
  namespace {
    namespace fs = std::filesystem;

    constexpr usize max_recursive_watches = 4096;
    constexpr u32 directory_watch_mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM |
                                         IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                                         IN_ONLYDIR;

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

    void close_fd(int& fd) {
      if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
      }
    }

    const char* errno_code(int value) {
      switch (value) {
      case EACCES:
        return "EACCES";
      case EMFILE:
        return "EMFILE";
      case ENOENT:
        return "ENOENT";
      case ENOMEM:
        return "ENOMEM";
      case ENOSPC:
        return "ENOSPC";
      case ENOTDIR:
        return "ENOTDIR";
      case EINVAL:
        return "EINVAL";
      default:
        return "EIO";
      }
    }

    void assign_watch_error(fs_watch_error& out, int value, const char* syscall,
                            const fs::path& path, std::string_view detail = {}) {
      out.code = errno_code(value);
      out.errno_value = value;
      out.syscall = syscall != nullptr ? syscall : "fs.watch";
      out.path = path.generic_string();
      out.message = out.syscall + std::string(": ") + std::strerror(value);
      if (!out.path.empty()) {
        out.message += " '";
        out.message += out.path;
        out.message += "'";
      }
      if (!detail.empty()) {
        out.message += ". ";
        out.message.append(detail);
      }
    }

    constexpr std::string_view inotify_limit_hint =
        "Increase fs.inotify.max_user_watches with: "
        "sudo sysctl fs.inotify.max_user_watches=<larger value>";

    class inotify_fs_watcher final : public fs_watcher {
    public:
      inotify_fs_watcher(std::string path, bool recursive,
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

        fd_ = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (fd_ < 0) {
          assign_watch_error(error_, errno, "inotify_init1", watch_root_);
          return;
        }

        if (::pipe2(wake_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
          assign_watch_error(error_, errno, "pipe2", watch_root_);
          cleanup_fds();
          return;
        }

        if (!add_directory(watch_root_)) {
          cleanup_fds();
          return;
        }

        if (recursive_ && !filter_single_file_ && !add_existing_descendants(watch_root_)) {
          cleanup_fds();
          return;
        }

        active_.store(true);
        try {
          thread_ = std::thread([this] { run(); });
        } catch (...) {
          active_.store(false);
          assign_watch_error(error_, ENOMEM, "std::thread", watch_root_);
          cleanup_fds();
        }
      }

      ~inotify_fs_watcher() override {
        close();
      }

      bool active() const {
        return active_.load();
      }

      const fs_watch_error& error() const {
        return error_;
      }

      void close() override {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true))
          return;

        wake_thread();

        if (thread_.joinable()) {
          if (thread_.get_id() == std::this_thread::get_id())
            thread_.detach();
          else
            thread_.join();
        }

        cleanup_fds();
        watches_.clear();
        path_to_wd_.clear();
        active_.store(false);
      }

    private:
      void cleanup_fds() {
        close_fd(fd_);
        close_fd(wake_pipe_[0]);
        close_fd(wake_pipe_[1]);
      }

      void wake_thread() const {
        if (wake_pipe_[1] < 0)
          return;
        const u8 byte = 1;
        (void)::write(wake_pipe_[1], &byte, sizeof(byte));
      }

      void log_watch_limit_once() {
        bool expected = false;
        if (!watch_limit_logged_.compare_exchange_strong(expected, true))
          return;
        FXE_WARN("runtime.fs_watch",
                 "inotify fs watcher reached recursive watch limit ({}); watching stopped. {}",
                 max_recursive_watches, inotify_limit_hint);
      }

      bool at_watch_limit(const fs::path& path) {
        if (watches_.size() < max_recursive_watches)
          return false;
        log_watch_limit_once();
        assign_watch_error(error_, ENOSPC, "inotify_add_watch", path, inotify_limit_hint);
        return true;
      }

      bool add_directory(const fs::path& path) {
        const fs::path normalized = path.lexically_normal();
        const std::string key = normalized.generic_string();
        if (path_to_wd_.find(key) != path_to_wd_.end())
          return true;

        if (at_watch_limit(normalized))
          return false;

        const int wd = ::inotify_add_watch(fd_, normalized.c_str(), directory_watch_mask);
        if (wd < 0) {
          const int value = errno;
          const bool limit_error = value == EMFILE || value == ENOSPC;
          assign_watch_error(error_, value, "inotify_add_watch", normalized,
                             limit_error ? inotify_limit_hint : std::string_view{});
          return false;
        }

        const auto existing = watches_.find(wd);
        if (existing != watches_.end() && existing->second != normalized)
          path_to_wd_.erase(existing->second.generic_string());

        watches_[wd] = normalized;
        path_to_wd_[key] = wd;
        return true;
      }

      bool add_existing_descendants(const fs::path& root) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
          std::error_code status_ec;
          if (it->is_directory(status_ec) && !status_ec && !add_directory(it->path()))
            return false;
        }
        return true;
      }

      void remove_watch_mapping(int wd) {
        const auto it = watches_.find(wd);
        if (it == watches_.end())
          return;
        path_to_wd_.erase(it->second.generic_string());
        watches_.erase(it);
      }

      static fs_watch_event::kind classify_event(u32 mask) {
        if ((mask & (IN_MODIFY | IN_ATTRIB)) != 0)
          return fs_watch_event::kind::changed;
        if ((mask & IN_CREATE) != 0)
          return fs_watch_event::kind::created;
        if ((mask & (IN_DELETE | IN_DELETE_SELF)) != 0)
          return fs_watch_event::kind::deleted;
        if ((mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF)) != 0)
          return fs_watch_event::kind::renamed;
        return fs_watch_event::kind::changed;
      }

      std::optional<fs::path> event_path_for(const inotify_event& event) const {
        const auto it = watches_.find(event.wd);
        if (it == watches_.end())
          return std::nullopt;

        if (event.len > 0 && event.name[0] != '\0')
          return (it->second / event.name).lexically_normal();
        return it->second;
      }

      bool should_deliver(const fs::path& event_path, const inotify_event& event) const {
        if (filter_single_file_)
          return event.len > 0 && event_path.filename() == requested_path_.filename();

        if (event.len == 0)
          return event_path != watch_root_ || (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0;

        if (recursive_)
          return event_path != watch_root_;
        return event_path.parent_path() == watch_root_;
      }

      void maybe_add_recursive_directory(const inotify_event& event, const fs::path& event_path) {
        if (!recursive_ || filter_single_file_ || (event.mask & IN_ISDIR) == 0)
          return;
        if ((event.mask & (IN_CREATE | IN_MOVED_TO)) == 0)
          return;

        if (add_directory(event_path))
          add_existing_descendants(event_path);
      }

      void emit_overflow_event() {
        // inotify(7): IN_Q_OVERFLOW is delivered with wd == -1 to report that the
        // kernel queue overflowed and one or more events were dropped. Emit a
        // sentinel so callers can resynchronize from requested_path_.
        // Coverage gap: no Linux-only fs_watcher native test harness exists yet.
        FXE_WARN("runtime.fs_watch", "inotify queue overflowed for {}",
                 requested_path_.generic_string());
        if (!closed_.load())
          cb_(fs_watch_event{requested_path_.generic_string(), fs_watch_event::kind::overflow});
      }
      void handle_event(const inotify_event& event) {
        if (event.wd == -1 && (event.mask & IN_Q_OVERFLOW) != 0) {
          emit_overflow_event();
          return;
        }

        const auto event_path = event_path_for(event);
        if (!event_path) {
          if ((event.mask & IN_IGNORED) != 0)
            remove_watch_mapping(event.wd);
          return;
        }

        if ((event.mask & IN_IGNORED) != 0) {
          remove_watch_mapping(event.wd);
          return;
        }

        maybe_add_recursive_directory(event, *event_path);

        if (!closed_.load() && should_deliver(*event_path, event))
          cb_(fs_watch_event{event_path->generic_string(), classify_event(event.mask)});

        if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
          remove_watch_mapping(event.wd);
      }

      void drain_events() {
        alignas(inotify_event) std::array<char, 64 * 1024> buffer{};
        for (;;) {
          const ssize_t n = ::read(fd_, buffer.data(), buffer.size());
          if (n < 0) {
            if (errno == EINTR)
              continue;
            return;
          }
          if (n == 0)
            return;

          for (char* cursor = buffer.data(); cursor < buffer.data() + n;) {
            const auto* event = reinterpret_cast<const inotify_event*>(cursor);
            if (event->wd < 0 && (event->mask & IN_Q_OVERFLOW) == 0) {
              FXE_TRACE("runtime.fs_watch", "skipping inotify event with wd={} mask=0x{:x}",
                        event->wd, event->mask);
            } else {
              handle_event(*event);
            }
            cursor += sizeof(inotify_event) + event->len;
          }
        }
      }

      void drain_wake_pipe() const {
        std::array<u8, 64> buffer{};
        while (wake_pipe_[0] >= 0) {
          const ssize_t n = ::read(wake_pipe_[0], buffer.data(), buffer.size());
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            return;
        }
      }

      void run() {
        while (!closed_.load()) {
          pollfd pfds[] = {{fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
          const int rc = ::poll(pfds, 2, -1);
          if (rc < 0) {
            if (errno == EINTR)
              continue;
            break;
          }

          if ((pfds[1].revents & POLLIN) != 0)
            drain_wake_pipe();
          if (closed_.load())
            break;
          if ((pfds[0].revents & POLLIN) != 0)
            drain_events();
          if ((pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            break;
        }

        active_.store(false);
      }

      fs::path requested_path_;
      fs::path watch_root_;
      bool recursive_ = false;
      bool filter_single_file_ = false;
      std::function<void(const fs_watch_event&)> cb_;
      std::atomic<bool> closed_{false};
      std::atomic<bool> active_{false};
      std::atomic<bool> watch_limit_logged_{false};
      int fd_ = -1;
      int wake_pipe_[2] = {-1, -1};
      std::unordered_map<int, fs::path> watches_;
      fs_watch_error error_;
      std::unordered_map<std::string, int> path_to_wd_;
      std::thread thread_;
    };
  } // namespace

  std::unique_ptr<fs_watcher> fs_watcher::create(const std::string& path, bool recursive,
                                                 std::function<void(const fs_watch_event&)> cb,
                                                 fs_watch_error* error) {
    auto watcher = std::make_unique<inotify_fs_watcher>(path, recursive, std::move(cb));
    if (!watcher->active()) {
      if (error != nullptr)
        *error = watcher->error();
      return nullptr;
    }
    return watcher;
  }

#else

  std::unique_ptr<fs_watcher>
  fs_watcher::create(const std::string& /* path */, bool /* recursive */,
                     std::function<void(const fs_watch_event&)> /* cb */,
                     fs_watch_error* /* error */) {
    return nullptr;
  }

#endif

} // namespace fxe::runtime
