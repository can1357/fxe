// Single-instance coordination owns three process-local contracts:
//
// 1. Lockfile: platform code creates a per-app runtime directory and records the
//    primary PID as decimal text plus '\n' in `.lock`. POSIX platforms clean a
//    dead PID only while holding the sibling `.lock.guard` flock.
// 2. Handoff endpoint: the primary listens on `<runtime_dir>/<app>.sock` on
//    macOS/Linux, and `\\.\pipe\<app>` on Windows. Secondary instances keep the
//    existing acquire_or_forward() semantics: return false after attempting the
//    best-effort handoff.
// 3. Message frame: close-delimited text over the stream. Line 1 is argc, line
//    2 is the escaped cwd, followed by exactly argc newline-separated argv
//    lines. Payload bytes use backslash escapes (`\\`, `\n`, `\r`, `\0`).

#include "../../include/fxe/single_instance.hpp"
#include "os.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fxe/types.hpp>
#include <mutex>
#include <string_view>
#include <utility>

namespace fxe::os {

  bool single_instance_platform_acquire_or_forward(int argc, char** argv);
  void single_instance_platform_install_open_handlers();
  bool single_instance_platform_set_default_protocol_client(const std::string& scheme);
  bool single_instance_platform_set_default_file_handler(const std::string& ext);

  namespace {
    std::mutex g_single_instance_callbacks_mu;
    std::function<void(std::vector<std::string>, std::string)> g_second_instance_cb;
    std::function<void(std::string)> g_open_url_cb;
    std::function<void(std::string)> g_open_file_cb;
    std::vector<std::pair<std::vector<std::string>, std::string>> g_pending_second_instances;
    std::vector<std::string> g_pending_urls;
    std::vector<std::string> g_pending_files;

    std::vector<std::string> argv_vector(int argc, char** argv) {
      std::vector<std::string> out;
      if (argc <= 0 || !argv)
        return out;
      out.reserve(static_cast<usize>(argc));
      for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i] ? argv[i] : "");
      return out;
    }

    std::string cwd_string() {
      std::error_code ec;
      std::filesystem::path cwd = std::filesystem::current_path(ec);
      return ec ? std::string{} : cwd.string();
    }

    void append_escaped_line(std::string& out, std::string_view value) {
      for (char ch : value) {
        switch (ch) {
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\0':
          out += "\\0";
          break;
        default:
          out.push_back(ch);
          break;
        }
      }
      out.push_back('\n');
    }

    bool decode_escaped(std::string_view encoded, std::string& value) {
      std::string decoded;
      decoded.reserve(encoded.size());
      for (usize i = 0; i < encoded.size(); ++i) {
        char ch = encoded[i];
        if (ch != '\\') {
          decoded.push_back(ch);
          continue;
        }
        if (++i >= encoded.size())
          return false;
        switch (encoded[i]) {
        case '\\':
          decoded.push_back('\\');
          break;
        case 'n':
          decoded.push_back('\n');
          break;
        case 'r':
          decoded.push_back('\r');
          break;
        case '0':
          decoded.push_back('\0');
          break;
        default:
          return false;
        }
      }
      value = std::move(decoded);
      return true;
    }

    bool read_line(std::string_view data, usize& offset, std::string_view& line) {
      if (offset > data.size())
        return false;
      usize next = data.find('\n', offset);
      if (next == std::string_view::npos)
        return false;
      line = data.substr(offset, next - offset);
      offset = next + 1;
      return true;
    }

    bool parse_argc(std::string_view line, u32& argc) {
      if (line.empty())
        return false;
      u32 value = 0;
      for (char ch : line) {
        if (ch < '0' || ch > '9')
          return false;
        u32 digit = static_cast<u32>(ch - '0');
        if (value > (4096u - digit) / 10u)
          return false;
        value = value * 10u + digit;
      }
      argc = value;
      return true;
    }

    bool looks_like_url(std::string_view arg) {
      usize pos = arg.find("://");
      if (pos == std::string_view::npos || pos == 0)
        return false;
      if (!std::isalpha(static_cast<unsigned char>(arg[0])))
        return false;
      for (usize i = 1; i < pos; ++i) {
        unsigned char c = static_cast<unsigned char>(arg[i]);
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.')
          return false;
      }
      return true;
    }

    bool looks_like_absolute_file(std::string_view arg) {
      if (arg.empty())
        return false;
#if defined(_WIN32)
      return arg.size() >= 3 && std::isalpha(static_cast<unsigned char>(arg[0])) && arg[1] == ':' &&
             (arg[2] == '\\' || arg[2] == '/');
#else
      return arg[0] == '/';
#endif
    }

    void deliver_second_instance(std::vector<std::string> argv, std::string cwd) {
      std::function<void(std::vector<std::string>, std::string)> cb;
      {
        std::lock_guard<std::mutex> lock(g_single_instance_callbacks_mu);
        cb = g_second_instance_cb;
        if (!cb) {
          g_pending_second_instances.emplace_back(std::move(argv), std::move(cwd));
          return;
        }
      }
      cb(std::move(argv), std::move(cwd));
    }

    void deliver_open_url(std::string url) {
      std::function<void(std::string)> cb;
      {
        std::lock_guard<std::mutex> lock(g_single_instance_callbacks_mu);
        cb = g_open_url_cb;
        if (!cb) {
          g_pending_urls.push_back(std::move(url));
          return;
        }
      }
      cb(std::move(url));
    }

    void deliver_open_file(std::string path) {
      std::function<void(std::string)> cb;
      {
        std::lock_guard<std::mutex> lock(g_single_instance_callbacks_mu);
        cb = g_open_file_cb;
        if (!cb) {
          g_pending_files.push_back(std::move(path));
          return;
        }
      }
      cb(std::move(path));
    }
  } // namespace

  namespace single_instance_detail {
    std::string encode_handoff(const std::vector<std::string>& argv, const std::string& cwd) {
      std::string out = std::to_string(argv.size());
      out.push_back('\n');
      append_escaped_line(out, cwd);
      for (const std::string& arg : argv)
        append_escaped_line(out, arg);
      return out;
    }

    std::string encode_handoff(int argc, char** argv) {
      return encode_handoff(argv_vector(argc, argv), cwd_string());
    }

    bool decode_handoff(std::string_view data, std::vector<std::string>& argv, std::string& cwd) {
      usize offset = 0;
      std::string_view line;
      u32 argc = 0;
      if (!read_line(data, offset, line) || !parse_argc(line, argc))
        return false;

      std::string decoded_cwd;
      if (!read_line(data, offset, line) || !decode_escaped(line, decoded_cwd))
        return false;

      std::vector<std::string> decoded;
      decoded.reserve(argc);
      for (u32 i = 0; i < argc; ++i) {
        std::string arg;
        if (!read_line(data, offset, line) || !decode_escaped(line, arg))
          return false;
        decoded.push_back(std::move(arg));
      }
      if (offset != data.size())
        return false;
      argv = std::move(decoded);
      cwd = std::move(decoded_cwd);
      return true;
    }

    void dispatch_open_url(std::string url) {
      post_main_thread_dispatch(
          [url = std::move(url)]() mutable { deliver_open_url(std::move(url)); });
    }

    void dispatch_open_file(std::string path) {
      post_main_thread_dispatch(
          [path = std::move(path)]() mutable { deliver_open_file(std::move(path)); });
    }

    void dispatch_argv_open_events(const std::vector<std::string>& argv) {
      for (usize i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (looks_like_url(arg)) {
          dispatch_open_url(arg);
        } else if (looks_like_absolute_file(arg)) {
          dispatch_open_file(arg);
        }
      }
    }

    void dispatch_launch(std::vector<std::string> argv, std::string cwd) {
      std::vector<std::string> argv_copy = argv;
      post_main_thread_dispatch([argv = std::move(argv), cwd = std::move(cwd)]() mutable {
        deliver_second_instance(std::move(argv), std::move(cwd));
      });
      dispatch_argv_open_events(argv_copy);
    }
  } // namespace single_instance_detail

  bool acquire_or_forward(int argc, char** argv) {
    bool primary = single_instance_platform_acquire_or_forward(argc, argv);
    if (primary)
      single_instance_detail::dispatch_argv_open_events(argv_vector(argc, argv));
    return primary;
  }

  void on_second_instance(std::function<void(std::vector<std::string> argv, std::string cwd)> cb) {
    std::vector<std::pair<std::vector<std::string>, std::string>> pending;
    std::function<void(std::vector<std::string>, std::string)> active;
    {
      std::lock_guard<std::mutex> lock(g_single_instance_callbacks_mu);
      g_second_instance_cb = std::move(cb);
      active = g_second_instance_cb;
      if (active)
        pending.swap(g_pending_second_instances);
    }
    for (auto& event : pending)
      active(std::move(event.first), std::move(event.second));
  }

  void on_second_instance(std::function<void(std::vector<std::string> argv)> cb) {
    on_second_instance([cb = std::move(cb)](std::vector<std::string> argv, std::string) mutable {
      if (cb)
        cb(std::move(argv));
    });
  }

  void on_open_url(std::function<void(std::string url)> cb) {
    std::vector<std::string> pending;
    std::function<void(std::string)> active;
    {
      std::lock_guard<std::mutex> lock(g_single_instance_callbacks_mu);
      g_open_url_cb = std::move(cb);
      active = g_open_url_cb;
      if (active)
        pending.swap(g_pending_urls);
    }
    single_instance_platform_install_open_handlers();
    for (std::string& url : pending)
      active(std::move(url));
  }

  void on_open_file(std::function<void(std::string path)> cb) {
    std::vector<std::string> pending;
    std::function<void(std::string)> active;
    {
      std::lock_guard<std::mutex> lock(g_single_instance_callbacks_mu);
      g_open_file_cb = std::move(cb);
      active = g_open_file_cb;
      if (active)
        pending.swap(g_pending_files);
    }
    single_instance_platform_install_open_handlers();
    for (std::string& path : pending)
      active(std::move(path));
  }

  bool set_default_protocol_client(const std::string& scheme) {
    return single_instance_platform_set_default_protocol_client(scheme);
  }

  bool set_default_file_handler(const std::string& ext) {
    return single_instance_platform_set_default_file_handler(ext);
  }

} // namespace fxe::os
