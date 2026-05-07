#pragma once

#include <functional>
#include <memory>
#include <string>

namespace fxe::runtime {

  struct fs_watch_event {
    enum class kind { changed, renamed, deleted, created };

    std::string path;
    kind k = kind::changed;
  };

  struct fs_watch_error {
    std::string code;
    int errno_value = 0;
    std::string syscall;
    std::string path;
    std::string message;
  };

  class fs_watcher {
  public:
    static std::unique_ptr<fs_watcher> create(const std::string& path, bool recursive,
                                              std::function<void(const fs_watch_event&)> cb,
                                              fs_watch_error* error = nullptr);
    virtual ~fs_watcher();
    virtual void close() = 0;
  };

} // namespace fxe::runtime
