#include "runtime/v8/fs_watcher.hpp"
#include <fxe/types.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace fxe::runtime {
  fs_watcher::~fs_watcher() = default;

#if defined(_WIN32)

  namespace {
    namespace fs = std::filesystem;

    constexpr DWORD notify_filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                    FILE_NOTIFY_CHANGE_ATTRIBUTES;

    fs::path path_from_utf8(std::string_view value) {
      std::u8string u8;
      u8.reserve(value.size());
      for (const char c : value)
        u8.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
      return fs::path(u8);
    }

    std::string path_to_utf8(const fs::path& path) {
      const std::u8string u8 = path.generic_u8string();
      return {reinterpret_cast<const char*>(u8.data()), u8.size()};
    }

    fs::path absolute_path(const std::string& path) {
      std::error_code ec;
      auto out = fs::absolute(path_from_utf8(path), ec);
      if (ec)
        out = path_from_utf8(path);
      return out.lexically_normal();
    }

    bool existing_directory(const fs::path& path) {
      std::error_code ec;
      const bool is_dir = fs::is_directory(path, ec);
      return is_dir && !ec;
    }

    std::string narrow_utf8(const wchar_t* value, int length) {
      if (value == nullptr || length <= 0)
        return {};
      const int needed =
          WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
      if (needed <= 0)
        return {};
      std::string out(static_cast<usize>(needed), '\0');
      (void)WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), needed, nullptr, nullptr);
      return out;
    }

    std::wstring file_name_from_info(const FILE_NOTIFY_INFORMATION* info) {
      if (info == nullptr || info->FileNameLength == 0)
        return {};
      return {info->FileName, info->FileNameLength / sizeof(wchar_t)};
    }

    std::wstring basename_of(std::wstring_view path) {
      const usize pos = path.find_last_of(L"\\/");
      if (pos == std::wstring_view::npos)
        return std::wstring(path);
      return std::wstring(path.substr(pos + 1));
    }

    bool same_filename(std::wstring_view a, std::wstring_view b) {
      if (a.empty() || b.empty())
        return false;
      const int result = CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                              static_cast<int>(b.size()), TRUE);
      return result == CSTR_EQUAL;
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

    class read_directory_changes_watcher final : public fs_watcher {
    public:
      read_directory_changes_watcher(std::string path, bool recursive,
                                     std::function<void(const fs_watch_event&)> cb)
          : requested_path_(absolute_path(path)), recursive_(recursive), cb_(std::move(cb)) {
        filter_single_file_ = !existing_directory(requested_path_);
        watch_root_ = filter_single_file_ ? requested_path_.parent_path() : requested_path_;
        if (watch_root_.empty())
          watch_root_ = fs::path(L".");
        filter_basename_ = filter_single_file_ ? requested_path_.filename().wstring() : L"";

        directory_handle_ =
            CreateFileW(watch_root_.c_str(), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_handle_ == INVALID_HANDLE_VALUE)
          return;

        event_handle_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_handle_ == nullptr) {
          close_handles();
          return;
        }

        remember_snapshot(requested_path_);
        if (recursive_ && !filter_single_file_)
          remember_existing_descendants(requested_path_);

        thread_ = std::thread([this] { run(); });

        std::unique_lock lock(mutex_);
        ready_cv_.wait(lock, [this] { return ready_; });
      }

      ~read_directory_changes_watcher() override {
        close();
      }

      bool active() const {
        return active_.load();
      }

      void close() override {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true))
          return;

        cancel_pending_io();

        if (thread_.joinable()) {
          if (thread_.get_id() == std::this_thread::get_id())
            thread_.detach();
          else
            thread_.join();
        }
      }

    private:
      void cancel_pending_io() {
        std::lock_guard lock(mutex_);
        if (directory_handle_ != INVALID_HANDLE_VALUE)
          (void)CancelIoEx(directory_handle_, nullptr);
      }

      void close_handles() {
        std::lock_guard lock(mutex_);
        if (directory_handle_ != INVALID_HANDLE_VALUE) {
          CloseHandle(directory_handle_);
          directory_handle_ = INVALID_HANDLE_VALUE;
        }
        if (event_handle_ != nullptr) {
          CloseHandle(event_handle_);
          event_handle_ = nullptr;
        }
      }

      void mark_ready(bool active) {
        {
          std::lock_guard lock(mutex_);
          active_.store(active);
          ready_ = true;
        }
        ready_cv_.notify_all();
      }

      void remember_snapshot(const fs::path& path) {
        if (auto snapshot = snapshot_path(path))
          snapshots_[path_to_utf8(path)] = *snapshot;
      }

      void remember_existing_descendants(const fs::path& root) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
          remember_snapshot(it->path().lexically_normal());
      }

      static fs_watch_event::kind kind_from_action(DWORD action) {
        switch (action) {
        case FILE_ACTION_ADDED:
          return fs_watch_event::kind::created;
        case FILE_ACTION_REMOVED:
          return fs_watch_event::kind::deleted;
        case FILE_ACTION_MODIFIED:
          return fs_watch_event::kind::changed;
        case FILE_ACTION_RENAMED_OLD_NAME:
        case FILE_ACTION_RENAMED_NEW_NAME:
          return fs_watch_event::kind::renamed;
        default:
          return fs_watch_event::kind::changed;
        }
      }

      fs_watch_event::kind classify_event(const fs::path& event, DWORD action) {
        const std::string key = path_to_utf8(event);
        const auto previous_it = snapshots_.find(key);
        std::optional<path_snapshot> previous;
        if (previous_it != snapshots_.end())
          previous = previous_it->second;

        const auto current = snapshot_path(event);
        if (current)
          snapshots_[key] = *current;

        if (action == FILE_ACTION_MODIFIED && previous && current) {
          if (previous->exists != current->exists)
            return current->exists ? fs_watch_event::kind::created : fs_watch_event::kind::deleted;
          if (previous->exists && !same_snapshot(*previous, *current))
            return fs_watch_event::kind::changed;
        }

        return kind_from_action(action);
      }

      void emit_event(const FILE_NOTIFY_INFORMATION* info) {
        if (closed_.load())
          return;

        const std::wstring relative_name = file_name_from_info(info);
        if (relative_name.empty())
          return;

        if (filter_single_file_ && !same_filename(basename_of(relative_name), filter_basename_))
          return;

        const std::string utf8_name =
            narrow_utf8(relative_name.data(), static_cast<int>(relative_name.size()));
        if (utf8_name.empty())
          return;

        const fs::path event_path = (watch_root_ / path_from_utf8(utf8_name)).lexically_normal();
        cb_(fs_watch_event{path_to_utf8(event_path), classify_event(event_path, info->Action)});
      }

      void process_events(const std::vector<std::byte>& buffer, DWORD bytes) {
        usize offset = 0;
        while (!closed_.load() && offset < bytes) {
          const auto* info =
              reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
          emit_event(info);
          if (info->NextEntryOffset == 0)
            break;
          offset += info->NextEntryOffset;
        }
      }

      bool read_once(std::vector<std::byte>& buffer, bool& announced_ready) {
        HANDLE directory_handle = INVALID_HANDLE_VALUE;
        HANDLE event_handle = nullptr;
        {
          std::lock_guard lock(mutex_);
          directory_handle = directory_handle_;
          event_handle = event_handle_;
        }
        if (directory_handle == INVALID_HANDLE_VALUE || event_handle == nullptr)
          return false;

        (void)ResetEvent(event_handle);
        OVERLAPPED overlapped{};
        overlapped.hEvent = event_handle;

        const BOOL started = ReadDirectoryChangesW(
            directory_handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            recursive_ ? TRUE : FALSE, notify_filter, nullptr, &overlapped, nullptr);
        if (!started && GetLastError() != ERROR_IO_PENDING) {
          if (!announced_ready)
            mark_ready(false);
          return false;
        }

        if (!announced_ready) {
          mark_ready(true);
          announced_ready = true;
        }

        const DWORD wait_result = WaitForSingleObject(event_handle, INFINITE);
        if (wait_result != WAIT_OBJECT_0)
          return false;

        DWORD bytes = 0;
        if (!GetOverlappedResult(directory_handle, &overlapped, &bytes, FALSE)) {
          const DWORD error = GetLastError();
          if (closed_.load() || error == ERROR_OPERATION_ABORTED)
            return false;
          return true;
        }

        if (!closed_.load() && bytes > 0)
          process_events(buffer, bytes);
        return true;
      }

      void run() {
        std::vector<std::byte> buffer(64 * 1024);
        bool announced_ready = false;

        while (!closed_.load()) {
          if (!read_once(buffer, announced_ready))
            break;
        }

        if (!announced_ready)
          mark_ready(false);
        active_.store(false);
        close_handles();
      }

      fs::path requested_path_;
      fs::path watch_root_;
      bool recursive_ = false;
      bool filter_single_file_ = false;
      std::wstring filter_basename_;
      std::unordered_map<std::string, path_snapshot> snapshots_;
      std::function<void(const fs_watch_event&)> cb_;
      std::atomic<bool> closed_{false};
      std::atomic<bool> active_{false};
      mutable std::mutex mutex_;
      std::condition_variable ready_cv_;
      bool ready_ = false;
      HANDLE directory_handle_ = INVALID_HANDLE_VALUE;
      HANDLE event_handle_ = nullptr;
      std::thread thread_;
    };
  } // namespace

  std::unique_ptr<fs_watcher> fs_watcher::create(const std::string& path, bool recursive,
                                                 std::function<void(const fs_watch_event&)> cb,
                                                 fs_watch_error* error) {
    auto watcher = std::make_unique<read_directory_changes_watcher>(path, recursive, std::move(cb));
    if (!watcher->active()) {
      if (error != nullptr) {
        error->code = "EIO";
        error->errno_value = 0;
        error->syscall = "ReadDirectoryChangesW";
        error->path = path;
        error->message = "ReadDirectoryChangesW: failed to start watcher '" + path + "'";
      }
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
