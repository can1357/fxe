// Linux implementation for fxe::os using POSIX/freedesktop fallbacks plus
// optional low-level libdbus-1 desktop integrations.

#include "../os.hpp"
#include <cstdio>

#if !defined(__APPLE__) && !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
#include <dbus/dbus.h>
#endif
#endif

#include <mutex>
#include <queue>
#include <unordered_set>

namespace fxe::os {
  namespace {
    std::mutex g_mu;
    std::queue<std::function<void()>> g_q;

#if !defined(__APPLE__) && !defined(_WIN32)
    std::mutex g_lock_mu;
    std::map<std::string, int> g_single_instance_locks;

    std::string env_or_empty(const char* name) {
      const char* value = std::getenv(name);
      return value ? std::string(value) : std::string{};
    }

    bool is_executable_file(const std::filesystem::path& path) {
      struct stat st{};
      return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
             ::access(path.c_str(), X_OK) == 0;
    }

    bool command_exists(const char* name) {
      if (!name || !*name)
        return false;
      std::string path_env = env_or_empty("PATH");
      if (path_env.empty())
        path_env = "/usr/local/bin:/usr/bin:/bin";

      std::stringstream ss(path_env);
      std::string dir;
      while (std::getline(ss, dir, ':')) {
        if (dir.empty())
          dir = ".";
        if (is_executable_file(std::filesystem::path(dir) / name))
          return true;
      }
      return false;
    }

    bool exec_args(const std::vector<std::string>& args);

    struct subprocess_result {
      int status = 127;
      std::vector<uint8_t> output;
    };

    subprocess_result run_subprocess(const std::vector<std::string>& args,
                                     const std::vector<uint8_t>* input, bool capture_stdout) {
      subprocess_result result;
      if (args.empty())
        return result;
      int in_pipe[2] = {-1, -1};
      int out_pipe[2] = {-1, -1};
      if (input && ::pipe(in_pipe) != 0)
        return result;
      if (capture_stdout && ::pipe(out_pipe) != 0) {
        if (in_pipe[0] >= 0) {
          ::close(in_pipe[0]);
          ::close(in_pipe[1]);
        }
        return result;
      }

      pid_t pid = ::fork();
      if (pid < 0) {
        if (in_pipe[0] >= 0) {
          ::close(in_pipe[0]);
          ::close(in_pipe[1]);
        }
        if (out_pipe[0] >= 0) {
          ::close(out_pipe[0]);
          ::close(out_pipe[1]);
        }
        return result;
      }
      if (pid == 0) {
        if (input) {
          ::dup2(in_pipe[0], STDIN_FILENO);
          ::close(in_pipe[0]);
          ::close(in_pipe[1]);
        }
        if (capture_stdout) {
          ::dup2(out_pipe[1], STDOUT_FILENO);
          ::close(out_pipe[0]);
          ::close(out_pipe[1]);
        }
        int dev_null = ::open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
          if (!input)
            ::dup2(dev_null, STDIN_FILENO);
          if (!capture_stdout)
            ::dup2(dev_null, STDOUT_FILENO);
          ::dup2(dev_null, STDERR_FILENO);
          if (dev_null > STDERR_FILENO)
            ::close(dev_null);
        }
        exec_args(args);
        _exit(127);
      }

      if (input) {
        ::close(in_pipe[0]);
        size_t written = 0;
        while (written < input->size()) {
          ssize_t n = ::write(in_pipe[1], input->data() + written, input->size() - written);
          if (n < 0) {
            if (errno == EINTR)
              continue;
            break;
          }
          if (n == 0)
            break;
          written += static_cast<size_t>(n);
        }
        ::close(in_pipe[1]);
      }
      if (capture_stdout) {
        ::close(out_pipe[1]);
        uint8_t buffer[4096];
        for (;;) {
          ssize_t n = ::read(out_pipe[0], buffer, sizeof(buffer));
          if (n < 0) {
            if (errno == EINTR)
              continue;
            break;
          }
          if (n == 0)
            break;
          result.output.insert(result.output.end(), buffer, buffer + n);
        }
        ::close(out_pipe[0]);
      }
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      result.status = status;
      return result;
    }

    bool subprocess_ok(const subprocess_result& result) {
      return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0;
    }

    bool set_clipboard_target(std::string_view target, const std::vector<uint8_t>& bytes) {
      std::string target_string(target);
      if (target_string.empty())
        return false;
      if (!env_or_empty("WAYLAND_DISPLAY").empty() && command_exists("wl-copy")) {
        auto result = run_subprocess({"wl-copy", "--type", target_string}, &bytes, false);
        if (subprocess_ok(result))
          return true;
      }
      if (command_exists("xclip")) {
        auto result = run_subprocess({"xclip", "-selection", "clipboard", "-t", target_string},
                                     &bytes, false);
        if (subprocess_ok(result))
          return true;
      }
      return false;
    }

    std::optional<std::vector<uint8_t>> get_clipboard_target(std::string_view target) {
      std::string target_string(target);
      if (target_string.empty())
        return std::nullopt;
      if (!env_or_empty("WAYLAND_DISPLAY").empty() && command_exists("wl-paste")) {
        auto result =
            run_subprocess({"wl-paste", "--type", target_string, "--no-newline"}, nullptr, true);
        if (subprocess_ok(result))
          return std::move(result.output);
      }
      if (command_exists("xclip")) {
        auto result = run_subprocess(
            {"xclip", "-selection", "clipboard", "-t", target_string, "-o"}, nullptr, true);
        if (subprocess_ok(result))
          return std::move(result.output);
      }
      return std::nullopt;
    }

    bool exec_args(const std::vector<std::string>& args) {
      if (args.empty())
        return false;

      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
      argv.push_back(nullptr);
      ::execvp(argv[0], argv.data());
      return false;
    }

    bool spawn_detached(const std::vector<std::string>& args) {
      if (args.empty())
        return false;

      pid_t pid = ::fork();
      if (pid < 0)
        return false;
      if (pid == 0) {
        ::setsid();
        pid_t grandchild = ::fork();
        if (grandchild < 0)
          _exit(127);
        if (grandchild > 0)
          _exit(0);

        int dev_null = ::open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
          ::dup2(dev_null, STDIN_FILENO);
          ::dup2(dev_null, STDOUT_FILENO);
          ::dup2(dev_null, STDERR_FILENO);
          if (dev_null > STDERR_FILENO)
            ::close(dev_null);
        }

        exec_args(args);
        _exit(127);
      }

      int status = 0;
      pid_t waited = 0;
      do {
        waited = ::waitpid(pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
      return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    struct command_result {
      int exit_code = -1;
      std::string stdout_text;
    };

    command_result run_command_capture(const std::vector<std::string>& args) {
      command_result result;
      if (args.empty())
        return result;

      int pipefd[2];
      if (::pipe(pipefd) != 0)
        return result;

      pid_t pid = ::fork();
      if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return result;
      }

      if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        int dev_null = ::open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
          ::dup2(dev_null, STDIN_FILENO);
          ::dup2(dev_null, STDERR_FILENO);
          if (dev_null > STDERR_FILENO)
            ::close(dev_null);
        }
        if (pipefd[1] > STDERR_FILENO)
          ::close(pipefd[1]);

        exec_args(args);
        _exit(127);
      }

      ::close(pipefd[1]);
      char buffer[4096];
      for (;;) {
        ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
          result.stdout_text.append(buffer, static_cast<size_t>(n));
          continue;
        }
        if (n == 0)
          break;
        if (errno == EINTR)
          continue;
        break;
      }
      ::close(pipefd[0]);

      int status = 0;
      pid_t waited = 0;
      do {
        waited = ::waitpid(pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
      if (waited == pid && WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
      return result;
    }

    bool run_command_success(const std::vector<std::string>& args) {
      return run_command_capture(args).exit_code == 0;
    }

    std::string path_join(std::string base, std::string_view child) {
      if (base.empty())
        return {};
      std::filesystem::path p(base);
      p /= child;
      return p.string();
    }

    std::string home_dir() {
      return env_or_empty("HOME");
    }

    std::string xdg_config_home(const std::string& home) {
      std::string config = env_or_empty("XDG_CONFIG_HOME");
      return config.empty() ? path_join(home, ".config") : config;
    }

    std::string xdg_data_home(const std::string& home) {
      std::string data = env_or_empty("XDG_DATA_HOME");
      return data.empty() ? path_join(home, ".local/share") : data;
    }

    std::string expand_user_dir_value(std::string value, const std::string& home) {
      constexpr std::string_view home_var = "$HOME";
      size_t pos = value.find(home_var);
      if (pos != std::string::npos)
        value.replace(pos, home_var.size(), home);
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
      return value;
    }

    std::string xdg_user_dir(const std::string& key, const std::string& fallback,
                             const std::string& home) {
      std::filesystem::path config_path =
          std::filesystem::path(xdg_config_home(home)) / "user-dirs.dirs";
      std::ifstream file(config_path);
      std::string line;
      const std::string prefix = "XDG_" + key + "_DIR=";
      while (std::getline(file, line)) {
        if (line.rfind(prefix, 0) != 0)
          continue;
        return expand_user_dir_value(line.substr(prefix.size()), home);
      }
      return path_join(home, fallback);
    }

    std::string absolute_path(std::string_view raw) {
      if (raw.empty())
        return {};
      std::error_code ec;
      auto abs = std::filesystem::absolute(std::filesystem::path(raw), ec);
      return ec ? std::string(raw) : abs.string();
    }

    std::string file_uri_for_path(std::string_view raw) {
      std::string path = absolute_path(raw);
      std::string uri = "file://";
      static constexpr char hex[] = "0123456789ABCDEF";
      for (unsigned char c : path) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' ||
                          c == '~';
        if (safe) {
          uri.push_back(static_cast<char>(c));
        } else {
          uri.push_back('%');
          uri.push_back(hex[c >> 4]);
          uri.push_back(hex[c & 0x0f]);
        }
      }
      return uri;
    }

    std::string xml_escape(std::string_view value) {
      std::string out;
      out.reserve(value.size());
      for (char ch : value) {
        switch (ch) {
        case '&':
          out += "&amp;";
          break;
        case '<':
          out += "&lt;";
          break;
        case '>':
          out += "&gt;";
          break;
        case '"':
          out += "&quot;";
          break;
        case '\'':
          out += "&apos;";
          break;
        default:
          out.push_back(ch);
          break;
        }
      }
      return out;
    }

    std::string xml_unescape(std::string value) {
      struct entity {
        const char* escaped;
        const char* plain;
      };
      static constexpr entity entities[] = {
          {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
      for (const auto& e : entities) {
        size_t pos = 0;
        while ((pos = value.find(e.escaped, pos)) != std::string::npos) {
          value.replace(pos, std::strlen(e.escaped), e.plain);
          pos += std::strlen(e.plain);
        }
      }
      return value;
    }

    int hex_value(char ch) {
      if (ch >= '0' && ch <= '9')
        return ch - '0';
      if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
      return -1;
    }

    std::string path_from_file_uri(std::string uri) {
      constexpr std::string_view prefix = "file://";
      if (uri.rfind(prefix, 0) != 0)
        return {};
      std::string encoded = uri.substr(prefix.size());
      std::string out;
      out.reserve(encoded.size());
      for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
          int hi = hex_value(encoded[i + 1]);
          int lo = hex_value(encoded[i + 2]);
          if (hi >= 0 && lo >= 0) {
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
          }
        }
        out.push_back(encoded[i]);
      }
      return out;
    }

    std::filesystem::path recently_used_xbel_path() {
      const std::string home = home_dir();
      if (home.empty())
        return {};
      return std::filesystem::path(xdg_data_home(home)) / "recently-used.xbel";
    }

    std::string xbel_timestamp() {
      std::time_t now = std::time(nullptr);
      std::tm tm{};
      gmtime_r(&now, &tm);
      char buffer[32]{};
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
      return buffer;
    }

    std::string empty_recent_xbel() {
      return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<xbel version=\"1.0\" xmlns:bookmark=\"http://www.freedesktop.org/standards/"
             "desktop-bookmarks\" xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-"
             "info\">\n"
             "</xbel>\n";
    }

    std::string read_text_file(const std::filesystem::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in)
        return {};
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    bool write_text_file(const std::filesystem::path& path, const std::string& content) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec)
        return false;
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out)
        return false;
      out << content;
      return static_cast<bool>(out);
    }

    std::string xbel_bookmark(std::string_view uri, std::string_view timestamp) {
      std::string app_name = env_or_empty("FXE_APP_NAME");
      if (app_name.empty())
        app_name = "fxe";
      const std::string escaped_uri = xml_escape(uri);
      const std::string escaped_app = xml_escape(app_name);
      const std::string escaped_time = xml_escape(timestamp);
      return "  <bookmark href=\"" + escaped_uri + "\" added=\"" + escaped_time + "\" modified=\"" +
             escaped_time + "\" visited=\"" + escaped_time +
             "\">\n"
             "    <info>\n"
             "      <metadata owner=\"http://freedesktop.org\">\n"
             "        <bookmark:applications>\n"
             "          <bookmark:application name=\"" +
             escaped_app + "\" exec=\"" + escaped_app + " %u\" modified=\"" + escaped_time +
             "\" count=\"1\"/>\n"
             "        </bookmark:applications>\n"
             "      </metadata>\n"
             "    </info>\n"
             "  </bookmark>\n";
    }

    bool upsert_recent_xbel_entry(std::string_view path) {
      std::filesystem::path xbel_path = recently_used_xbel_path();
      if (xbel_path.empty())
        return false;
      const std::string uri = file_uri_for_path(path);
      if (uri.empty())
        return false;
      std::string content = read_text_file(xbel_path);
      if (content.find("<xbel") == std::string::npos ||
          content.find("</xbel>") == std::string::npos)
        content = empty_recent_xbel();

      const std::string escaped_uri = xml_escape(uri);
      size_t href = content.find("href=\"" + escaped_uri + "\"");
      if (href != std::string::npos) {
        size_t begin = content.rfind("<bookmark", href);
        size_t end = content.find("</bookmark>", href);
        if (begin != std::string::npos && end != std::string::npos)
          content.erase(begin, end + std::strlen("</bookmark>") - begin);
      }

      size_t insert = content.rfind("</xbel>");
      if (insert == std::string::npos) {
        content = empty_recent_xbel();
        insert = content.rfind("</xbel>");
      }
      content.insert(insert, xbel_bookmark(uri, xbel_timestamp()));
      return write_text_file(xbel_path, content);
    }

    std::string parent_directory(std::string_view raw) {
      if (raw.empty())
        return {};
      std::filesystem::path p(raw);
      std::filesystem::path parent =
          p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
      return parent.string();
    }

    std::string sanitize_lock_component(std::string_view app_id) {
      std::string out;
      out.reserve(app_id.size());
      for (unsigned char c : app_id) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
          out.push_back(static_cast<char>(c));
        } else {
          out.push_back('_');
        }
      }
      return out.empty() ? std::string("fxe") : out;
    }

    std::filesystem::path single_instance_runtime_dir(std::string_view app_id) {
      std::string root = env_or_empty("XDG_RUNTIME_DIR");
      if (root.empty())
        root = "/tmp";
      return std::filesystem::path(root) / sanitize_lock_component(app_id);
    }

    bool read_lock_pid(const std::filesystem::path& lock_path, pid_t& pid) {
      int fd = ::open(lock_path.c_str(), O_RDONLY | O_CLOEXEC);
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
      size_t left = payload.size();
      while (left > 0) {
        ssize_t n = ::write(fd, data, left);
        if (n < 0) {
          if (errno == EINTR)
            continue;
          return false;
        }
        data += n;
        left -= static_cast<size_t>(n);
      }
      return true;
    }

    std::vector<std::string> read_cmdline_args() {
      std::vector<std::string> args;
      int fd = ::open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        return args;

      std::string data;
      char buffer[4096];
      for (;;) {
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
          data.append(buffer, static_cast<size_t>(n));
          continue;
        }
        if (n == 0)
          break;
        if (errno == EINTR)
          continue;
        data.clear();
        break;
      }
      ::close(fd);

      size_t start = 0;
      while (start < data.size()) {
        size_t end = data.find('\0', start);
        if (end == std::string::npos)
          end = data.size();
        if (end > start)
          args.emplace_back(data.substr(start, end - start));
        start = end + 1;
      }
      return args;
    }

    std::string executable_stem() {
      std::vector<std::string> args = read_cmdline_args();
      if (!args.empty())
        return std::filesystem::path(args.front()).stem().string();
      char exe_buffer[4096];
      ssize_t len = ::readlink("/proc/self/exe", exe_buffer, sizeof(exe_buffer) - 1);
      if (len > 0) {
        exe_buffer[len] = '\0';
        return std::filesystem::path(exe_buffer).stem().string();
      }
      return "fxe";
    }

    void add_zenity_file_filters(std::vector<std::string>& args,
                                 const std::vector<dialog_filter>& filters) {
      for (const auto& filter : filters) {
        if (filter.extensions.empty())
          continue;
        std::string pattern = filter.name.empty() ? std::string("Files") : filter.name;
        pattern += " |";
        for (const auto& ext : filter.extensions) {
          pattern += " *.";
          pattern += ext;
        }
        args.push_back("--file-filter=" + pattern);
      }
    }

    std::vector<std::string> split_lines(std::string text) {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

      std::vector<std::string> lines;
      std::stringstream ss(text);
      std::string line;
      while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (!line.empty())
          lines.push_back(line);
      }
      return lines;
    }

    std::string lower_ascii(std::string_view raw) {
      std::string out;
      out.reserve(raw.size());
      for (unsigned char c : raw)
        out.push_back(static_cast<char>(std::tolower(c)));
      return out;
    }

    std::string upper_ascii(std::string_view raw) {
      std::string out;
      out.reserve(raw.size());
      for (unsigned char c : raw)
        out.push_back(static_cast<char>(std::toupper(c)));
      return out;
    }

    std::string trim_copy(std::string_view raw) {
      size_t first = 0;
      while (first < raw.size() && std::isspace(static_cast<unsigned char>(raw[first])) != 0)
        ++first;
      size_t last = raw.size();
      while (last > first && std::isspace(static_cast<unsigned char>(raw[last - 1])) != 0)
        --last;
      return std::string(raw.substr(first, last - first));
    }

    std::string join_strings(const std::vector<std::string>& parts, char delimiter) {
      std::string out;
      for (const auto& part : parts) {
        if (!out.empty())
          out.push_back(delimiter);
        out += part;
      }
      return out;
    }

    std::string normalize_accelerator_for_portal_impl(std::string_view accelerator) {
      std::vector<std::string> modifiers;
      std::string key;
      size_t start = 0;
      while (start <= accelerator.size()) {
        size_t end = accelerator.find('+', start);
        if (end == std::string_view::npos)
          end = accelerator.size();
        std::string token = trim_copy(accelerator.substr(start, end - start));
        std::string lower = lower_ascii(token);
        if (lower == "cmd" || lower == "command" || lower == "meta" || lower == "super")
          modifiers.push_back("Meta");
        else if (lower == "ctrl" || lower == "control" || lower == "commandorcontrol" ||
                 lower == "cmdorctrl")
          modifiers.push_back("Ctrl");
        else if (lower == "alt" || lower == "option")
          modifiers.push_back("Alt");
        else if (lower == "shift")
          modifiers.push_back("Shift");
        else if (!token.empty())
          key = token;
        if (end == accelerator.size())
          break;
        start = end + 1;
      }

      if (key.empty())
        return {};
      std::string lower_key = lower_ascii(key);
      if (lower_key == "return")
        key = "Enter";
      else if (lower_key == "esc")
        key = "Escape";
      else if (lower_key == "space")
        key = "Space";
      else if (lower_key == "plus")
        key = "+";
      else if (key.size() == 1)
        key = upper_ascii(key);

      modifiers.push_back(key);
      return join_strings(modifiers, '+');
    }

    struct menu_export_node {
      int id = 0;
      std::string action_id;
      std::string label;
      std::string type = "normal";
      bool enabled = true;
      bool checked = false;
      bool visible = true;
      std::string accelerator;
      std::vector<menu_export_node> children;
    };

    menu_export_node build_menu_export_node(const menu_item& item, int& next_id) {
      menu_export_node node;
      node.id = next_id++;
      node.action_id = item.id;
      node.label = item.label;
      node.type = item.type.empty() ? std::string("normal") : item.type;
      node.enabled = item.enabled;
      node.checked = item.checked;
      node.accelerator = item.accelerator;
      node.children.reserve(item.submenu.size());
      for (const auto& child : item.submenu)
        node.children.push_back(build_menu_export_node(child, next_id));
      return node;
    }

    std::vector<menu_export_node> build_menu_export_tree(const std::vector<menu_item>& items) {
      int next_id = 1;
      std::vector<menu_export_node> nodes;
      nodes.reserve(items.size());
      for (const auto& item : items)
        nodes.push_back(build_menu_export_node(item, next_id));
      return nodes;
    }

    int count_menu_nodes(const std::vector<menu_export_node>& nodes) {
      int count = 0;
      for (const auto& node : nodes) {
        ++count;
        count += count_menu_nodes(node.children);
      }
      return count;
    }

    void collect_menu_item_ids(const std::vector<menu_export_node>& nodes,
                               std::unordered_map<std::string, int>& ids) {
      for (const auto& node : nodes) {
        if (!node.action_id.empty())
          ids[node.action_id] = node.id;
        collect_menu_item_ids(node.children, ids);
      }
    }

    menu_export_node* find_menu_node_by_id(std::vector<menu_export_node>& nodes, int id) {
      for (auto& node : nodes) {
        if (node.id == id)
          return &node;
        if (auto* child = find_menu_node_by_id(node.children, id))
          return child;
      }
      return nullptr;
    }
#endif

    void warn_unsupported_once(bool& warned, const char* message) {
      if (!warned) {
        warned = true;
        std::fprintf(stderr, "%s\n", message);
      }
    }

    static void warn_dbus_disabled_once(const char* feature) {
      static std::mutex warning_mu;
      static std::unordered_set<std::string> warned_features;
      std::string key =
          feature && *feature ? std::string(feature) : std::string("D-Bus integration");
      {
        std::lock_guard<std::mutex> lock(warning_mu);
        if (!warned_features.insert(key).second)
          return;
      }
      std::fprintf(stderr, "fxe.os: %s requires libdbus-1 on Linux\n", key.c_str());
    }

#if !defined(__APPLE__) && !defined(_WIN32) && defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    constexpr const char* kNotificationsBus = "org.freedesktop.Notifications";
    constexpr const char* kNotificationsPath = "/org/freedesktop/Notifications";
    constexpr const char* kNotificationsIface = "org.freedesktop.Notifications";
    constexpr const char* kDbusBus = "org.freedesktop.DBus";
    constexpr const char* kDbusPath = "/org/freedesktop/DBus";
    constexpr const char* kDbusIface = "org.freedesktop.DBus";
    constexpr const char* kSniWatcherBus = "org.kde.StatusNotifierWatcher";
    constexpr const char* kSniWatcherPath = "/StatusNotifierWatcher";
    constexpr const char* kSniWatcherIface = "org.kde.StatusNotifierWatcher";
    constexpr const char* kSniIface = "org.kde.StatusNotifierItem";
    constexpr const char* kSniPath = "/StatusNotifierItem";
    constexpr const char* kPortalBus = "org.freedesktop.portal.Desktop";
    constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
    constexpr const char* kPortalShortcutsIface = "org.freedesktop.portal.GlobalShortcuts";
    constexpr const char* kAppMenuRegistrarBus = "com.canonical.AppMenu.Registrar";
    constexpr const char* kAppMenuRegistrarPath = "/com/canonical/AppMenu/Registrar";
    constexpr const char* kAppMenuRegistrarIface = "com.canonical.AppMenu.Registrar";
    constexpr const char* kDbusMenuIface = "com.canonical.dbusmenu";
    constexpr const char* kDbusMenuPath = "/com/canonical/dbusmenu";
    constexpr const char* kTrayMenuPath = "/com/canonical/dbusmenu/tray";
    constexpr const char* kContextMenuPath = "/com/canonical/dbusmenu/context";
    constexpr const char* kUnityLauncherPath = "/com/canonical/Unity/LauncherEntry";
    constexpr const char* kUnityLauncherIface = "com.canonical.Unity.LauncherEntry";

    struct notification_entry {
      std::function<void()> click_callback;
      std::function<void(const std::string&, std::optional<std::string>)> action_callback;
    };

    struct tray_entry {
      int id = -1;
      std::string bus_name;
      std::string icon_path;
      std::string title;
      std::string tooltip;
      std::vector<menu_export_node> menu;
    };

    struct tray_listener_entry {
      int tray_id = -1;
      tray_event_kind kind = tray_event_kind::click;
      std::function<void()> callback;
    };

    struct shortcut_entry {
      std::string id;
      std::string accelerator;
      std::function<void()> callback;
    };

    struct portal_response_waiter {
      std::mutex mu;
      std::condition_variable cv;
      bool done = false;
      uint32_t response = UINT32_MAX;
      std::string session_handle;
    };

    struct dbus_state {
      std::mutex mu;
      DBusConnection* conn = nullptr;
      std::thread dispatcher;
      bool dispatcher_started = false;
      bool shutting_down = false;
      bool atexit_registered = false;
      bool filter_installed = false;
    };

    dbus_state g_dbus_state;
    std::mutex g_notif_mu;
    std::unordered_map<uint32_t, notification_entry> g_notifications;
    std::mutex g_tray_mu;
    std::unordered_map<int, tray_entry> g_trays;
    std::atomic<int> g_tray_id{1};
    std::atomic<int> g_tray_listener_seq{1};
    std::map<int, tray_listener_entry> g_tray_listeners;
    std::mutex g_shortcut_mu;
    std::unordered_map<std::string, shortcut_entry> g_shortcuts;
    std::string g_shortcut_session_handle;
    std::mutex g_portal_response_mu;
    std::unordered_map<std::string, std::shared_ptr<portal_response_waiter>> g_portal_responses;
    std::mutex g_menu_mu;
    std::vector<menu_export_node> g_app_menu;
    std::unordered_map<std::string, int> g_app_menu_item_ids;
    uint32_t g_menu_revision = 1;
    std::vector<menu_export_node> g_context_menu;
    uint32_t g_context_menu_revision = 1;

    void dbus_shutdown();
    DBusHandlerResult dbus_filter(DBusConnection*, DBusMessage*, void*);
    DBusHandlerResult dbus_object_dispatch(DBusConnection*, DBusMessage*, void*);
    bool register_app_menu_with_registrar(DBusConnection* conn);

    DBusObjectPathVTable g_object_vtable = {
        nullptr, dbus_object_dispatch, nullptr, nullptr, nullptr, nullptr};

    void free_error(DBusError& error) {
      if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    }

    void append_basic_string(DBusMessageIter* iter, const std::string& value) {
      const char* raw = value.c_str();
      dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &raw);
    }

    void append_basic_object_path(DBusMessageIter* iter, const std::string& value) {
      const char* raw = value.c_str();
      dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &raw);
    }

    void append_variant_string(DBusMessageIter* iter, const std::string& value) {
      DBusMessageIter variant;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &variant);
      append_basic_string(&variant, value);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_variant_object_path(DBusMessageIter* iter, const std::string& value) {
      DBusMessageIter variant;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "o", &variant);
      append_basic_object_path(&variant, value);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_variant_bool(DBusMessageIter* iter, bool value) {
      dbus_bool_t raw = value ? TRUE : FALSE;
      DBusMessageIter variant;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &variant);
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &raw);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_variant_int32(DBusMessageIter* iter, int32_t value) {
      DBusMessageIter variant;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &variant);
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_variant_int64(DBusMessageIter* iter, int64_t value) {
      DBusMessageIter variant;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "x", &variant);
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT64, &value);
      dbus_message_iter_close_container(iter, &variant);
    }

    std::vector<std::string> dbus_shortcut_tokens(std::string_view accelerator) {
      std::string normalized = normalize_accelerator_for_portal_impl(accelerator);
      if (normalized.empty())
        return {};
      std::vector<std::string> tokens;
      size_t start = 0;
      while (start <= normalized.size()) {
        size_t end = normalized.find('+', start);
        std::string token =
            normalized.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (token == "Ctrl")
          token = "Control";
        if (!token.empty())
          tokens.push_back(std::move(token));
        if (end == std::string::npos)
          break;
        start = end + 1;
      }
      return tokens;
    }

    void append_variant_shortcut(DBusMessageIter* iter, std::string_view accelerator) {
      DBusMessageIter variant;
      DBusMessageIter outer;
      DBusMessageIter shortcut;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "aas", &variant);
      dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "as", &outer);
      dbus_message_iter_open_container(&outer, DBUS_TYPE_ARRAY, "s", &shortcut);
      for (const auto& token : dbus_shortcut_tokens(accelerator))
        append_basic_string(&shortcut, token);
      dbus_message_iter_close_container(&outer, &shortcut);
      dbus_message_iter_close_container(&variant, &outer);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_dict_string(DBusMessageIter* dict, const char* key, const std::string& value) {
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_variant_string(&entry, value);
      dbus_message_iter_close_container(dict, &entry);
    }

    void append_dict_bool(DBusMessageIter* dict, const char* key, bool value) {
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_variant_bool(&entry, value);
      dbus_message_iter_close_container(dict, &entry);
    }

    void append_dict_int32(DBusMessageIter* dict, const char* key, int32_t value) {
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_variant_int32(&entry, value);
      dbus_message_iter_close_container(dict, &entry);
    }

    void append_dict_int64(DBusMessageIter* dict, const char* key, int64_t value) {
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_variant_int64(&entry, value);
      dbus_message_iter_close_container(dict, &entry);
    }

    void append_dict_shortcut(DBusMessageIter* dict, const char* key,
                              std::string_view accelerator) {
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_variant_shortcut(&entry, accelerator);
      dbus_message_iter_close_container(dict, &entry);
    }

    std::string read_variant_string(DBusMessageIter* variant) {
      if (!variant || dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return {};
      DBusMessageIter value;
      dbus_message_iter_recurse(variant, &value);
      int type = dbus_message_iter_get_arg_type(&value);
      if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        return {};
      const char* raw = nullptr;
      dbus_message_iter_get_basic(&value, &raw);
      return raw ? std::string(raw) : std::string{};
    }

    std::string dict_lookup_string(DBusMessageIter* dict, const char* wanted_key) {
      if (!dict || !wanted_key)
        return {};
      while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        DBusMessageIter value;
        const char* key = nullptr;
        dbus_message_iter_recurse(dict, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
          dbus_message_iter_get_basic(&entry, &key);
          if (key && std::strcmp(key, wanted_key) == 0 && dbus_message_iter_next(&entry)) {
            value = entry;
            return read_variant_string(&value);
          }
        }
        dbus_message_iter_next(dict);
      }
      return {};
    }

    void append_empty_icon_pixmap_variant(DBusMessageIter* iter) {
      DBusMessageIter variant;
      DBusMessageIter array;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a(iiay)", &variant);
      dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(iiay)", &array);
      dbus_message_iter_close_container(&variant, &array);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_tooltip_variant(DBusMessageIter* iter, const tray_entry& tray) {
      DBusMessageIter variant;
      DBusMessageIter tooltip;
      DBusMessageIter pixmaps;
      dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "(sa(iiay)ss)", &variant);
      dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &tooltip);
      append_basic_string(&tooltip, tray.icon_path);
      dbus_message_iter_open_container(&tooltip, DBUS_TYPE_ARRAY, "(iiay)", &pixmaps);
      dbus_message_iter_close_container(&tooltip, &pixmaps);
      append_basic_string(&tooltip, tray.title.empty() ? std::string("fxe") : tray.title);
      append_basic_string(&tooltip, tray.tooltip);
      dbus_message_iter_close_container(&variant, &tooltip);
      dbus_message_iter_close_container(iter, &variant);
    }

    void append_dict_tooltip(DBusMessageIter* dict, const tray_entry& tray) {
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      const char* key = "ToolTip";
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_tooltip_variant(&entry, tray);
      dbus_message_iter_close_container(dict, &entry);
    }

    bool name_has_owner(DBusConnection* conn, const char* name) {
      if (!conn)
        return false;
      DBusMessage* msg =
          dbus_message_new_method_call(kDbusBus, kDbusPath, kDbusIface, "NameHasOwner");
      if (!msg)
        return false;
      dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        free_error(error);
        return false;
      }
      dbus_bool_t has_owner = FALSE;
      dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &has_owner, DBUS_TYPE_INVALID);
      dbus_message_unref(reply);
      free_error(error);
      return has_owner != FALSE;
    }

    void add_signal_match(DBusConnection* conn, const char* rule) {
      DBusError error;
      dbus_error_init(&error);
      dbus_bus_add_match(conn, rule, &error);
      free_error(error);
    }

    void install_filter_and_matches(DBusConnection* conn) {
      if (!conn)
        return;
      if (!g_dbus_state.filter_installed) {
        dbus_connection_add_filter(conn, dbus_filter, nullptr, nullptr);
        g_dbus_state.filter_installed = true;
      }
      add_signal_match(
          conn, "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'");
      add_signal_match(
          conn,
          "type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'");
      add_signal_match(
          conn,
          "type='signal',interface='org.freedesktop.portal.GlobalShortcuts',member='Activated'");
      add_signal_match(
          conn, "type='signal',interface='org.freedesktop.portal.Request',member='Response'");
      add_signal_match(conn,
                       "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'");
      dbus_connection_flush(conn);
    }

    void dispatcher_loop() {
      for (;;) {
        DBusConnection* conn = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_dbus_state.mu);
          if (g_dbus_state.shutting_down)
            return;
          conn = g_dbus_state.conn;
          if (conn)
            dbus_connection_ref(conn);
        }

        if (!conn) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }

        dbus_connection_read_write_dispatch(conn, 100);
        const bool connected = dbus_connection_get_is_connected(conn) != FALSE;
        dbus_connection_unref(conn);
        if (!connected) {
          std::lock_guard<std::mutex> lock(g_dbus_state.mu);
          if (g_dbus_state.conn && dbus_connection_get_is_connected(g_dbus_state.conn) == FALSE) {
            dbus_connection_unref(g_dbus_state.conn);
            g_dbus_state.conn = nullptr;
            g_dbus_state.filter_installed = false;
          }
        }
      }
    }

    void ensure_dispatcher_started() {
      if (!g_dbus_state.atexit_registered) {
        std::atexit(dbus_shutdown);
        g_dbus_state.atexit_registered = true;
      }
      if (!g_dbus_state.dispatcher_started) {
        g_dbus_state.dispatcher_started = true;
        g_dbus_state.dispatcher = std::thread(dispatcher_loop);
      }
    }

    DBusConnection* dbus_connection() {
      std::lock_guard<std::mutex> lock(g_dbus_state.mu);
      if (g_dbus_state.shutting_down)
        return nullptr;
      if (g_dbus_state.conn && dbus_connection_get_is_connected(g_dbus_state.conn) != FALSE)
        return g_dbus_state.conn;
      if (g_dbus_state.conn) {
        dbus_connection_unref(g_dbus_state.conn);
        g_dbus_state.conn = nullptr;
        g_dbus_state.filter_installed = false;
      }

      dbus_threads_init_default();
      DBusError error;
      dbus_error_init(&error);
      DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
      if (!conn) {
        free_error(error);
        return nullptr;
      }
      dbus_connection_set_exit_on_disconnect(conn, FALSE);
      g_dbus_state.conn = conn;
      install_filter_and_matches(conn);
      dbus_connection_register_object_path(conn, kDbusMenuPath, &g_object_vtable, nullptr);
      dbus_connection_register_object_path(conn, kContextMenuPath, &g_object_vtable, nullptr);
      ensure_dispatcher_started();
      free_error(error);
      return conn;
    }

    void dbus_shutdown() {
      DBusConnection* conn = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_dbus_state.mu);
        if (g_dbus_state.shutting_down)
          return;
        g_dbus_state.shutting_down = true;
        conn = g_dbus_state.conn;
        g_dbus_state.conn = nullptr;
      }
      if (conn) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
      }
      if (g_dbus_state.dispatcher.joinable())
        g_dbus_state.dispatcher.join();
    }

    bool register_sni_watcher(DBusConnection* conn, const std::string& bus_name) {
      if (!name_has_owner(conn, kSniWatcherBus))
        return false;
      DBusMessage* msg = dbus_message_new_method_call(
          kSniWatcherBus, kSniWatcherPath, kSniWatcherIface, "RegisterStatusNotifierItem");
      if (!msg)
        return false;
      const char* raw = bus_name.c_str();
      dbus_message_append_args(msg, DBUS_TYPE_STRING, &raw, DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      const bool ok = reply != nullptr;
      if (reply)
        dbus_message_unref(reply);
      free_error(error);
      return ok;
    }

    std::optional<tray_entry> tray_for_message(DBusMessage* msg) {
      const char* path = dbus_message_get_path(msg);
      if (!path || std::strcmp(path, kSniPath) != 0)
        return std::nullopt;
      const char* destination = dbus_message_get_destination(msg);
      std::lock_guard<std::mutex> lock(g_tray_mu);
      if (destination) {
        for (const auto& entry : g_trays) {
          if (entry.second.bus_name == destination)
            return entry.second;
        }
      }
      if (g_trays.empty())
        return std::nullopt;
      return g_trays.begin()->second;
    }

    bool send_sni_property(DBusConnection* conn, DBusMessage* msg, const tray_entry& tray,
                           const std::string& property) {
      DBusMessage* reply = dbus_message_new_method_return(msg);
      if (!reply)
        return true;
      DBusMessageIter iter;
      dbus_message_iter_init_append(reply, &iter);
      if (property == "Id")
        append_variant_string(&iter, "fxe-tray-" + std::to_string(tray.id));
      else if (property == "Title")
        append_variant_string(&iter, tray.title.empty() ? std::string("fxe") : tray.title);
      else if (property == "Status")
        append_variant_string(&iter, "Active");
      else if (property == "IconName")
        append_variant_string(&iter, tray.icon_path);
      else if (property == "IconPixmap")
        append_empty_icon_pixmap_variant(&iter);
      else if (property == "ToolTip")
        append_tooltip_variant(&iter, tray);
      else if (property == "Menu" && !tray.menu.empty())
        append_variant_object_path(&iter, kTrayMenuPath);
      else
        append_variant_string(&iter, std::string{});
      dbus_connection_send(conn, reply, nullptr);
      dbus_message_unref(reply);
      return true;
    }

    void emit_sni_signal(DBusConnection* conn, const char* member) {
      if (!conn || !member)
        return;
      DBusMessage* signal = dbus_message_new_signal(kSniPath, kSniIface, member);
      if (!signal)
        return;
      dbus_connection_send(conn, signal, nullptr);
      dbus_message_unref(signal);
      dbus_connection_flush(conn);
    }

    void emit_dbus_menu_layout_updated(DBusConnection* conn, const char* path, uint32_t revision) {
      if (!conn || !path)
        return;
      DBusMessage* signal = dbus_message_new_signal(path, kDbusMenuIface, "LayoutUpdated");
      if (!signal)
        return;
      int32_t parent = 0;
      dbus_message_append_args(signal, DBUS_TYPE_UINT32, &revision, DBUS_TYPE_INT32, &parent,
                               DBUS_TYPE_INVALID);
      dbus_connection_send(conn, signal, nullptr);
      dbus_message_unref(signal);
      dbus_connection_flush(conn);
    }

    void dispatch_tray_event(int tray_id, tray_event_kind kind) {
      std::vector<std::function<void()>> callbacks;
      {
        std::lock_guard<std::mutex> lock(g_tray_mu);
        for (const auto& [_, listener] : g_tray_listeners) {
          if (listener.tray_id == tray_id && listener.kind == kind && listener.callback)
            callbacks.push_back(listener.callback);
        }
      }
      for (auto& cb : callbacks)
        post_main_thread_dispatch(std::move(cb));
    }

    void append_sni_property_dict(DBusMessageIter* dict, const tray_entry& tray) {
      append_dict_string(dict, "Id", "fxe-tray-" + std::to_string(tray.id));
      append_dict_string(dict, "Title", tray.title.empty() ? std::string("fxe") : tray.title);
      append_dict_string(dict, "Status", "Active");
      append_dict_string(dict, "IconName", tray.icon_path);
      DBusMessageIter entry;
      dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
      const char* key = "IconPixmap";
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      append_empty_icon_pixmap_variant(&entry);
      dbus_message_iter_close_container(dict, &entry);
      append_dict_tooltip(dict, tray);
      if (!tray.menu.empty()) {
        DBusMessageIter menu_entry;
        dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &menu_entry);
        const char* menu_key = "Menu";
        dbus_message_iter_append_basic(&menu_entry, DBUS_TYPE_STRING, &menu_key);
        append_variant_object_path(&menu_entry, kTrayMenuPath);
        dbus_message_iter_close_container(dict, &menu_entry);
      }
    }

    void append_menu_node(DBusMessageIter* iter, const menu_export_node& node) {
      DBusMessageIter node_struct;
      DBusMessageIter props;
      DBusMessageIter children;
      int32_t id = static_cast<int32_t>(node.id);
      dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, nullptr, &node_struct);
      dbus_message_iter_append_basic(&node_struct, DBUS_TYPE_INT32, &id);
      dbus_message_iter_open_container(&node_struct, DBUS_TYPE_ARRAY, "{sv}", &props);
      append_dict_string(&props, "label", node.label);
      append_dict_bool(&props, "enabled", node.enabled);
      append_dict_bool(&props, "visible", node.visible);
      if (node.type == "separator")
        append_dict_string(&props, "type", "separator");
      if (node.type == "checkbox") {
        append_dict_string(&props, "toggle-type", "checkmark");
        append_dict_int32(&props, "toggle-state", node.checked ? 1 : 0);
      }
      if (!node.children.empty())
        append_dict_string(&props, "children-display", "submenu");
      if (!node.accelerator.empty())
        append_dict_shortcut(&props, "shortcut", node.accelerator);
      dbus_message_iter_close_container(&node_struct, &props);
      dbus_message_iter_open_container(&node_struct, DBUS_TYPE_ARRAY, "v", &children);
      for (const auto& child : node.children) {
        DBusMessageIter variant;
        dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &variant);
        append_menu_node(&variant, child);
        dbus_message_iter_close_container(&children, &variant);
      }
      dbus_message_iter_close_container(&node_struct, &children);
      dbus_message_iter_close_container(iter, &node_struct);
    }

    std::string find_action_id(const std::vector<menu_export_node>& nodes, int32_t id) {
      for (const auto& node : nodes) {
        if (node.id == id)
          return node.action_id;
        std::string child = find_action_id(node.children, id);
        if (!child.empty())
          return child;
      }
      return {};
    }

    DBusHandlerResult handle_dbus_menu(DBusConnection* conn, DBusMessage* msg) {
      if (dbus_message_is_method_call(msg, kDbusMenuIface, "GetLayout")) {
        std::vector<menu_export_node> menu;
        uint32_t revision = 0;
        const char* path = dbus_message_get_path(msg);
        if (path && std::strcmp(path, kTrayMenuPath) == 0) {
          std::lock_guard<std::mutex> lock(g_tray_mu);
          if (!g_trays.empty())
            menu = g_trays.begin()->second.menu;
          revision = 1;
        } else if (path && std::strcmp(path, kContextMenuPath) == 0) {
          std::lock_guard<std::mutex> lock(g_menu_mu);
          menu = g_context_menu;
          revision = g_context_menu_revision;
        } else {
          std::lock_guard<std::mutex> lock(g_menu_mu);
          menu = g_app_menu;
          revision = g_menu_revision;
        }
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
          return DBUS_HANDLER_RESULT_HANDLED;
        DBusMessageIter iter;
        DBusMessageIter root;
        DBusMessageIter props;
        DBusMessageIter children;
        int32_t root_id = 0;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &revision);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr, &root);
        dbus_message_iter_append_basic(&root, DBUS_TYPE_INT32, &root_id);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &props);
        append_dict_string(&props, "children-display", "submenu");
        dbus_message_iter_close_container(&root, &props);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "v", &children);
        for (const auto& node : menu) {
          DBusMessageIter variant;
          dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &variant);
          append_menu_node(&variant, node);
          dbus_message_iter_close_container(&children, &variant);
        }
        dbus_message_iter_close_container(&root, &children);
        dbus_message_iter_close_container(&iter, &root);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, kDbusMenuIface, "Event")) {
        int32_t id = 0;
        const char* event_id = nullptr;
        DBusError error;
        dbus_error_init(&error);
        dbus_message_get_args(msg, &error, DBUS_TYPE_INT32, &id, DBUS_TYPE_STRING, &event_id,
                              DBUS_TYPE_INVALID);
        free_error(error);
        if (event_id && std::strcmp(event_id, "clicked") == 0) {
          std::string action;
          const char* path = dbus_message_get_path(msg);
          if (path && std::strcmp(path, kTrayMenuPath) == 0) {
            std::lock_guard<std::mutex> lock(g_tray_mu);
            if (!g_trays.empty())
              action = find_action_id(g_trays.begin()->second.menu, id);
          } else if (path && std::strcmp(path, kContextMenuPath) == 0) {
            std::lock_guard<std::mutex> lock(g_menu_mu);
            action = find_action_id(g_context_menu, id);
          } else {
            std::lock_guard<std::mutex> lock(g_menu_mu);
            action = find_action_id(g_app_menu, id);
          }
          if (!action.empty())
            post_main_thread_dispatch([action]() { (void)action; });
        }
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
          dbus_connection_send(conn, reply, nullptr);
          dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, kDbusMenuIface, "AboutToShow")) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
          dbus_bool_t need_update = FALSE;
          dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &need_update, DBUS_TYPE_INVALID);
          dbus_connection_send(conn, reply, nullptr);
          dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusHandlerResult handle_sni(DBusConnection* conn, DBusMessage* msg) {
      std::optional<tray_entry> tray = tray_for_message(msg);
      if (!tray)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

      if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get")) {
        const char* iface = nullptr;
        const char* property = nullptr;
        DBusError error;
        dbus_error_init(&error);
        dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &property,
                              DBUS_TYPE_INVALID);
        free_error(error);
        if (property)
          send_sni_property(conn, msg, *tray, property);
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll")) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
          return DBUS_HANDLER_RESULT_HANDLED;
        DBusMessageIter iter;
        DBusMessageIter dict;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
        append_sni_property_dict(&dict, *tray);
        dbus_message_iter_close_container(&iter, &dict);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, kSniIface, "Activate")) {
        dispatch_tray_event(tray->id, tray_event_kind::click);
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
          dbus_connection_send(conn, reply, nullptr);
          dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, kSniIface, "ContextMenu")) {
        dispatch_tray_event(tray->id, tray_event_kind::right_click);
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
          dbus_connection_send(conn, reply, nullptr);
          dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, kSniIface, "SecondaryActivate")) {
        dispatch_tray_event(tray->id, tray_event_kind::double_click);
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
          dbus_connection_send(conn, reply, nullptr);
          dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        static const char* xml =
            "<node><interface name='org.kde.StatusNotifierItem'>"
            "<method name='Activate'><arg type='i' direction='in'/><arg type='i' "
            "direction='in'/></method>"
            "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' "
            "direction='in'/></method>"
            "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' "
            "direction='in'/></method>"
            "</interface></node>";
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
          dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
          dbus_connection_send(conn, reply, nullptr);
          dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusHandlerResult dbus_object_dispatch(DBusConnection* conn, DBusMessage* msg, void*) {
      const char* path = dbus_message_get_path(msg);
      if (path && std::strcmp(path, kDbusMenuPath) == 0)
        return handle_dbus_menu(conn, msg);
      if (path && std::strcmp(path, kTrayMenuPath) == 0)
        return handle_dbus_menu(conn, msg);
      if (path && std::strcmp(path, kContextMenuPath) == 0)
        return handle_dbus_menu(conn, msg);
      if (path && std::strcmp(path, kSniPath) == 0)
        return handle_sni(conn, msg);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusHandlerResult dbus_filter(DBusConnection*, DBusMessage* msg, void*) {
      if (dbus_message_is_signal(msg, kNotificationsIface, "ActionInvoked")) {
        uint32_t id = 0;
        const char* action = nullptr;
        DBusError error;
        dbus_error_init(&error);
        dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &action,
                              DBUS_TYPE_INVALID);
        free_error(error);
        std::function<void()> click_cb;
        std::function<void(const std::string&, std::optional<std::string>)> action_cb;
        std::string action_id = action ? std::string(action) : std::string();
        {
          std::lock_guard<std::mutex> lock(g_notif_mu);
          auto found = g_notifications.find(id);
          if (found != g_notifications.end()) {
            if (action_id.empty() || action_id == "default") {
              click_cb = std::move(found->second.click_callback);
            } else {
              action_cb = std::move(found->second.action_callback);
            }
            g_notifications.erase(found);
          }
        }
        if (click_cb)
          post_main_thread_dispatch(std::move(click_cb));
        if (action_cb) {
          post_main_thread_dispatch(
              [cb = std::move(action_cb), action_id = std::move(action_id)]() mutable {
                cb(action_id, std::nullopt);
              });
        }
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_signal(msg, kNotificationsIface, "NotificationClosed")) {
        uint32_t id = 0;
        uint32_t reason = 0;
        DBusError error;
        dbus_error_init(&error);
        dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason,
                              DBUS_TYPE_INVALID);
        free_error(error);
        (void)reason;
        std::lock_guard<std::mutex> lock(g_notif_mu);
        g_notifications.erase(id);
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_signal(msg, kPortalShortcutsIface, "Activated")) {
        const char* session = nullptr;
        const char* shortcut_id = nullptr;
        uint32_t timestamp = 0;
        DBusError error;
        dbus_error_init(&error);
        dbus_message_get_args(msg, &error, DBUS_TYPE_OBJECT_PATH, &session, DBUS_TYPE_STRING,
                              &shortcut_id, DBUS_TYPE_UINT32, &timestamp, DBUS_TYPE_INVALID);
        free_error(error);
        (void)timestamp;
        std::function<void()> cb;
        if (shortcut_id) {
          std::lock_guard<std::mutex> lock(g_shortcut_mu);
          auto found = g_shortcuts.find(shortcut_id);
          if (found != g_shortcuts.end() && (!session || g_shortcut_session_handle.empty() ||
                                             g_shortcut_session_handle == session))
            cb = found->second.callback;
        }
        if (cb)
          post_main_thread_dispatch(std::move(cb));
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
        const char* path = dbus_message_get_path(msg);
        if (!path)
          return DBUS_HANDLER_RESULT_HANDLED;
        std::shared_ptr<portal_response_waiter> waiter;
        {
          std::lock_guard<std::mutex> lock(g_portal_response_mu);
          auto found = g_portal_responses.find(path);
          if (found != g_portal_responses.end())
            waiter = found->second;
        }
        if (!waiter)
          return DBUS_HANDLER_RESULT_HANDLED;

        uint32_t response = UINT32_MAX;
        std::string session_handle;
        DBusMessageIter iter;
        if (dbus_message_iter_init(msg, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
          dbus_message_iter_get_basic(&iter, &response);
          if (dbus_message_iter_next(&iter) &&
              dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            session_handle = dict_lookup_string(&iter, "session_handle");
          }
        }
        {
          std::lock_guard<std::mutex> lock(waiter->mu);
          waiter->response = response;
          waiter->session_handle = std::move(session_handle);
          waiter->done = true;
        }
        waiter->cv.notify_all();
        return DBUS_HANDLER_RESULT_HANDLED;
      }

      if (dbus_message_is_signal(msg, kDbusIface, "NameOwnerChanged")) {
        const char* name = nullptr;
        const char* old_owner = nullptr;
        const char* new_owner = nullptr;
        DBusError error;
        dbus_error_init(&error);
        dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                              DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID);
        free_error(error);
        (void)old_owner;
        if (name && new_owner && std::strcmp(name, kAppMenuRegistrarBus) == 0 && *new_owner) {
          bool has_menu = false;
          {
            std::lock_guard<std::mutex> lock(g_menu_mu);
            has_menu = !g_app_menu.empty();
          }
          DBusConnection* conn = has_menu ? dbus_connection() : nullptr;
          if (conn)
            register_app_menu_with_registrar(conn);
        }
      }

      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    bool register_app_menu_with_registrar(DBusConnection* conn) {
      if (!conn || !name_has_owner(conn, kAppMenuRegistrarBus))
        return false;
      DBusMessage* msg = dbus_message_new_method_call(kAppMenuRegistrarBus, kAppMenuRegistrarPath,
                                                      kAppMenuRegistrarIface, "RegisterWindow");
      if (!msg)
        return false;
      uint32_t window_id = 0;
      const char* menu_path = kDbusMenuPath;
      dbus_message_append_args(msg, DBUS_TYPE_UINT32, &window_id, DBUS_TYPE_OBJECT_PATH, &menu_path,
                               DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      const bool ok = reply != nullptr;
      if (reply)
        dbus_message_unref(reply);
      free_error(error);
      return ok;
    }

    bool unregister_app_menu_with_registrar(DBusConnection* conn) {
      if (!conn || !name_has_owner(conn, kAppMenuRegistrarBus))
        return false;
      DBusMessage* msg = dbus_message_new_method_call(kAppMenuRegistrarBus, kAppMenuRegistrarPath,
                                                      kAppMenuRegistrarIface, "UnregisterWindow");
      if (!msg)
        return false;
      uint32_t window_id = 0;
      dbus_message_append_args(msg, DBUS_TYPE_UINT32, &window_id, DBUS_TYPE_INVALID);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      const bool ok = reply != nullptr;
      if (reply)
        dbus_message_unref(reply);
      free_error(error);
      return ok;
    }

    std::shared_ptr<portal_response_waiter> track_portal_request(const std::string& path) {
      if (path.empty())
        return nullptr;
      auto waiter = std::make_shared<portal_response_waiter>();
      std::lock_guard<std::mutex> lock(g_portal_response_mu);
      g_portal_responses[path] = waiter;
      return waiter;
    }

    std::optional<std::string> wait_for_portal_request(DBusConnection* conn, DBusMessage* msg,
                                                       bool expect_session_handle) {
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        free_error(error);
        return std::nullopt;
      }
      const char* request_path = nullptr;
      dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &request_path, DBUS_TYPE_INVALID);
      std::string request = request_path ? request_path : std::string{};
      dbus_message_unref(reply);
      free_error(error);
      auto waiter = track_portal_request(request);
      if (!waiter)
        return std::nullopt;

      std::unique_lock<std::mutex> lock(waiter->mu);
      bool signaled =
          waiter->cv.wait_for(lock, std::chrono::seconds(3), [&]() { return waiter->done; });
      uint32_t response = waiter->response;
      std::string session = waiter->session_handle;
      lock.unlock();
      {
        std::lock_guard<std::mutex> response_lock(g_portal_response_mu);
        g_portal_responses.erase(request);
      }
      if (!signaled || response != 0)
        return std::nullopt;
      if (expect_session_handle && session.empty())
        return std::nullopt;
      return session;
    }

    void close_global_shortcuts_session(DBusConnection* conn) {
      std::string session;
      {
        std::lock_guard<std::mutex> lock(g_shortcut_mu);
        session = g_shortcut_session_handle;
        g_shortcut_session_handle.clear();
      }
      if (!conn || session.empty())
        return;
      DBusMessage* msg = dbus_message_new_method_call(kPortalBus, session.c_str(),
                                                      "org.freedesktop.portal.Session", "Close");
      if (!msg)
        return;
      dbus_connection_send(conn, msg, nullptr);
      dbus_connection_flush(conn);
      dbus_message_unref(msg);
    }

    bool ensure_global_shortcuts_session(DBusConnection* conn) {
      {
        std::lock_guard<std::mutex> lock(g_shortcut_mu);
        if (!g_shortcut_session_handle.empty())
          return true;
      }
      if (!conn || !name_has_owner(conn, kPortalBus))
        return false;
      DBusMessage* msg = dbus_message_new_method_call(kPortalBus, kPortalPath,
                                                      kPortalShortcutsIface, "CreateSession");
      if (!msg)
        return false;
      DBusMessageIter iter;
      DBusMessageIter dict;
      dbus_message_iter_init_append(msg, &iter);
      dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
      append_dict_string(&dict, "session_handle_token", "fxe_global_shortcuts");
      dbus_message_iter_close_container(&iter, &dict);
      DBusError error;
      dbus_error_init(&error);
      DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
      dbus_message_unref(msg);
      if (!reply) {
        free_error(error);
        return false;
      }
      const char* request_path = nullptr;
      dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &request_path, DBUS_TYPE_INVALID);
      std::string request = request_path ? request_path : std::string{};
      dbus_message_unref(reply);
      free_error(error);
      auto waiter = track_portal_request(request);
      if (!waiter)
        return false;
      std::unique_lock<std::mutex> wait_lock(waiter->mu);
      bool signaled =
          waiter->cv.wait_for(wait_lock, std::chrono::seconds(3), [&]() { return waiter->done; });
      uint32_t response = waiter->response;
      std::string session = waiter->session_handle;
      wait_lock.unlock();
      {
        std::lock_guard<std::mutex> response_lock(g_portal_response_mu);
        g_portal_responses.erase(request);
      }
      if (!signaled || response != 0 || session.empty())
        return false;
      std::lock_guard<std::mutex> lock(g_shortcut_mu);
      g_shortcut_session_handle = session;
      return true;
    }

    bool bind_portal_shortcut(DBusConnection* conn, const shortcut_entry& shortcut) {
      if (!ensure_global_shortcuts_session(conn))
        return false;
      DBusMessage* msg = dbus_message_new_method_call(kPortalBus, kPortalPath,
                                                      kPortalShortcutsIface, "BindShortcuts");
      if (!msg)
        return false;

      std::string session;
      {
        std::lock_guard<std::mutex> lock(g_shortcut_mu);
        session = g_shortcut_session_handle;
      }
      const char* parent_window = "";
      DBusMessageIter iter;
      DBusMessageIter shortcuts;
      DBusMessageIter shortcut_struct;
      DBusMessageIter shortcut_options;
      DBusMessageIter options;
      dbus_message_iter_init_append(msg, &iter);
      append_basic_object_path(&iter, session);
      dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sa{sv})", &shortcuts);
      dbus_message_iter_open_container(&shortcuts, DBUS_TYPE_STRUCT, nullptr, &shortcut_struct);
      append_basic_string(&shortcut_struct, shortcut.id);
      dbus_message_iter_open_container(&shortcut_struct, DBUS_TYPE_ARRAY, "{sv}",
                                       &shortcut_options);
      append_dict_string(&shortcut_options, "description", shortcut.accelerator);
      append_dict_string(&shortcut_options, "preferred-trigger", shortcut.accelerator);
      dbus_message_iter_close_container(&shortcut_struct, &shortcut_options);
      dbus_message_iter_close_container(&shortcuts, &shortcut_struct);
      dbus_message_iter_close_container(&iter, &shortcuts);
      dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent_window);
      dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options);
      dbus_message_iter_close_container(&iter, &options);

      std::optional<std::string> response = wait_for_portal_request(conn, msg, false);
      return response.has_value();
    }

    std::string desktop_file_uri() {
      const std::string home = home_dir();
      const std::string data_home = xdg_data_home(home);
      std::string bundle_id = env_or_empty("FXE_BUNDLE_ID");
      if (bundle_id.empty())
        bundle_id = executable_stem();
      std::filesystem::path preferred =
          std::filesystem::path(data_home) / "applications" / (bundle_id + ".desktop");
      if (std::filesystem::exists(preferred))
        return file_uri_for_path(preferred.string());
      std::filesystem::path fallback =
          std::filesystem::path(data_home) / "applications" / (executable_stem() + ".desktop");
      return file_uri_for_path(fallback.string());
    }
#endif
  } // namespace

#if !defined(__APPLE__) && !defined(_WIN32)
  std::string get_path(std::string_view kind) {
    const std::string home = home_dir();
    if (kind == "home")
      return home;
    if (kind == "temp") {
      std::string tmp = env_or_empty("TMPDIR");
      return tmp.empty() ? std::string("/tmp") : tmp;
    }
    if (kind == "userData") {
      std::string base = xdg_data_home(home);
      if (base.empty())
        base = xdg_config_home(home);
      return path_join(base, "fxe");
    }
    if (kind == "documents")
      return xdg_user_dir("DOCUMENTS", "Documents", home);
    if (kind == "downloads")
      return xdg_user_dir("DOWNLOAD", "Downloads", home);
    return {};
  }

  bool open_external(std::string_view url) {
    if (url.empty() || !command_exists("xdg-open"))
      return false;
    return spawn_detached({"xdg-open", std::string(url)});
  }

  bool show_item_in_folder(std::string_view path) {
    if (path.empty())
      return false;

    if (command_exists("dbus-send")) {
      if (run_command_success({"dbus-send", "--session", "--dest=org.freedesktop.FileManager1",
                               "--type=method_call", "/org/freedesktop/FileManager1",
                               "org.freedesktop.FileManager1.ShowItems",
                               "array:string:" + file_uri_for_path(path), "string:"})) {
        return true;
      }
    }

    std::string parent = parent_directory(path);
    return !parent.empty() && open_external(parent);
  }

  void beep() {
    const char bell = '\a';
    ssize_t ignored = ::write(STDERR_FILENO, &bell, 1);
    (void)ignored;
  }

  bool trash_item(std::string_view path) {
    if (path.empty() || !command_exists("gio"))
      return false;
    return run_command_success({"gio", "trash", std::string(path)});
  }

  bool request_single_instance_lock(std::string_view app_id) {
    std::filesystem::path dir = single_instance_runtime_dir(app_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
      return false;

    std::filesystem::path lock_path = dir / ".lock";
    std::string key = lock_path.string();
    std::lock_guard<std::mutex> g(g_lock_mu);
    if (g_single_instance_locks.find(key) != g_single_instance_locks.end())
      return true;

    std::filesystem::path guard_path = dir / ".lock.guard";
    int guard_fd = ::open(guard_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (guard_fd < 0)
      return false;
    if (::flock(guard_fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(guard_fd);
      return false;
    }

    pid_t recorded = 0;
    if (read_lock_pid(lock_path, recorded) && pid_is_gone(recorded))
      ::unlink(lock_path.c_str());

    int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
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

    g_single_instance_locks.emplace(std::move(key), fd);
    ::close(guard_fd);
    return true;
  }

  void set_badge_count(int n) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    DBusConnection* conn = dbus_connection();
    if (!conn) {
      static bool warned = false;
      warn_unsupported_once(warned, "fxe.os: setBadgeCount requires a D-Bus session bus");
      return;
    }
    DBusMessage* msg = dbus_message_new_signal(kUnityLauncherPath, kUnityLauncherIface, "Update");
    if (!msg)
      return;
    std::string app_uri = desktop_file_uri();
    DBusMessageIter iter;
    DBusMessageIter props;
    dbus_message_iter_init_append(msg, &iter);
    append_basic_string(&iter, app_uri);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &props);
    append_dict_bool(&props, "count-visible", n > 0);
    append_dict_int64(&props, "count", static_cast<int64_t>(n));
    dbus_message_iter_close_container(&iter, &props);
    dbus_connection_send(conn, msg, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
#else
    (void)n;
    warn_dbus_disabled_once("setBadgeCount");
#endif
  }

  void relaunch() {
    char exe_buffer[4096];
    ssize_t len = ::readlink("/proc/self/exe", exe_buffer, sizeof(exe_buffer) - 1);
    if (len <= 0)
      return;
    exe_buffer[len] = '\0';
    std::vector<std::string> args = read_cmdline_args();
    if (args.empty())
      args.push_back(std::string(exe_buffer));
    else
      args[0] = std::string(exe_buffer);
    spawn_detached(args);
  }

  namespace app {
    bool add_recent_document(std::string_view path) {
      if (path.empty())
        return false;
      return upsert_recent_xbel_entry(path);
    }

    std::vector<std::string> recent_documents() {
      std::vector<std::string> out;
      std::filesystem::path xbel_path = recently_used_xbel_path();
      if (xbel_path.empty())
        return out;
      std::string content = read_text_file(xbel_path);
      size_t pos = 0;
      while ((pos = content.find("<bookmark", pos)) != std::string::npos) {
        size_t tag_end = content.find('>', pos);
        if (tag_end == std::string::npos)
          break;
        size_t href = content.find("href=\"", pos);
        if (href != std::string::npos && href < tag_end) {
          href += std::strlen("href=\"");
          size_t end = content.find('"', href);
          if (end != std::string::npos && end <= tag_end) {
            std::string path = path_from_file_uri(xml_unescape(content.substr(href, end - href)));
            if (!path.empty())
              out.push_back(std::move(path));
          }
        }
        pos = tag_end + 1;
      }
      return out;
    }

    bool clear_recent_documents() {
      std::filesystem::path xbel_path = recently_used_xbel_path();
      if (xbel_path.empty())
        return false;
      return write_text_file(xbel_path, empty_recent_xbel());
    }
  } // namespace app

  std::optional<std::string> bookmark_persist(std::string_view path) {
    return std::string(path);
  }

  std::optional<std::pair<std::string, bool>> bookmark_resolve(std::string_view blob) {
    return std::make_pair(std::string(blob), false);
  }

  bool bookmark_start_access(std::string_view blob) {
    (void)blob;
    return true;
  }

  void bookmark_stop_access(std::string_view blob) {
    (void)blob;
  }

  std::vector<std::string> show_open_dialog(const open_dialog_options& options) {
    if (!command_exists("zenity"))
      return {};

    std::vector<std::string> args{"zenity", "--file-selection"};
    if (!options.title.empty())
      args.push_back("--title=" + options.title);
    if (!options.default_path.empty())
      args.push_back("--filename=" + options.default_path);
    if (options.multiple) {
      args.push_back("--multiple");
      args.push_back("--separator=\n");
    }
    if (options.directories)
      args.push_back("--directory");
    add_zenity_file_filters(args, options.filters);

    command_result result = run_command_capture(args);
    if (result.exit_code != 0)
      return {};
    if (options.multiple)
      return split_lines(result.stdout_text);
    std::vector<std::string> single = split_lines(result.stdout_text);
    return single.empty() ? std::vector<std::string>{} : std::vector<std::string>{single.front()};
  }

  std::optional<std::string> show_save_dialog(const save_dialog_options& options) {
    if (!command_exists("zenity"))
      return std::nullopt;

    std::vector<std::string> args{"zenity", "--file-selection", "--save", "--confirm-overwrite"};
    if (!options.title.empty())
      args.push_back("--title=" + options.title);
    if (!options.default_path.empty())
      args.push_back("--filename=" + options.default_path);
    add_zenity_file_filters(args, options.filters);

    command_result result = run_command_capture(args);
    if (result.exit_code != 0)
      return std::nullopt;
    std::vector<std::string> lines = split_lines(result.stdout_text);
    if (lines.empty())
      return std::nullopt;
    return lines.front();
  }

  int show_message_box(const message_box_options& options) {
    if (!command_exists("zenity"))
      return -1;

    std::vector<std::string> buttons = options.buttons;
    if (buttons.empty())
      buttons.push_back("OK");

    if (buttons.size() <= 2) {
      std::vector<std::string> args{"zenity"};
      if (options.type == "error")
        args.push_back("--error");
      else if (options.type == "warning")
        args.push_back("--warning");
      else if (options.type == "question")
        args.push_back("--question");
      else
        args.push_back("--info");
      if (!options.title.empty())
        args.push_back("--title=" + options.title);
      std::string text = options.message;
      if (!options.detail.empty()) {
        if (!text.empty())
          text += "\n\n";
        text += options.detail;
      }
      args.push_back("--text=" + text);
      if (!buttons.empty())
        args.push_back("--ok-label=" + buttons[0]);
      if (buttons.size() == 2)
        args.push_back("--cancel-label=" + buttons[1]);

      int code = run_command_capture(args).exit_code;
      if (code == 0)
        return 0;
      if (buttons.size() == 2 && code == 1)
        return 1;
      return -1;
    }

    std::vector<std::string> args{"zenity", "--list", "--hide-header", "--column=button"};
    if (!options.title.empty())
      args.push_back("--title=" + options.title);
    std::string text = options.message;
    if (!options.detail.empty()) {
      if (!text.empty())
        text += "\n\n";
      text += options.detail;
    }
    if (!text.empty())
      args.push_back("--text=" + text);
    for (const auto& button : buttons)
      args.push_back(button);

    command_result result = run_command_capture(args);
    if (result.exit_code != 0)
      return -1;
    std::vector<std::string> lines = split_lines(result.stdout_text);
    if (lines.empty())
      return -1;
    for (size_t i = 0; i < buttons.size(); ++i) {
      if (buttons[i] == lines.front())
        return static_cast<int>(i);
    }
    return -1;
  }

  int show_notification(const notification_options& options) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    DBusConnection* conn = dbus_connection();
    if (!conn) {
      static bool warned = false;
      warn_unsupported_once(warned, "fxe.os: notifications require a D-Bus session bus");
      return 0;
    }

    DBusMessage* msg = dbus_message_new_method_call(kNotificationsBus, kNotificationsPath,
                                                    kNotificationsIface, "Notify");
    if (!msg)
      return 0;

    std::string app_name = "fxe";
    uint32_t replaces_id = 0;
    std::string app_icon = options.icon_path;
    std::string summary = options.title.empty() ? std::string("fxe") : options.title;
    std::string body = options.body;
    int32_t expire_timeout = -1;
    DBusMessageIter iter;
    DBusMessageIter actions;
    DBusMessageIter hints;
    dbus_message_iter_init_append(msg, &iter);
    append_basic_string(&iter, app_name);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &replaces_id);
    append_basic_string(&iter, app_icon);
    append_basic_string(&iter, summary);
    append_basic_string(&iter, body);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &actions);
    append_basic_string(&actions, "default");
    append_basic_string(&actions, "Activate");
    for (const auto& action : options.actions) {
      if (action.id.empty() || action.title.empty())
        continue;
      append_basic_string(&actions, action.id);
      append_basic_string(&actions, action.title);
    }
    dbus_message_iter_close_container(&iter, &actions);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &hints);
    if (options.image_path && !options.image_path->empty())
      append_dict_string(&hints, "image-path", *options.image_path);
    dbus_message_iter_close_container(&iter, &hints);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &expire_timeout);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
    dbus_message_unref(msg);
    if (!reply) {
      free_error(error);
      return 0;
    }
    uint32_t id = 0;
    dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);
    free_error(error);
    return static_cast<int>(id);
#else
    (void)options;
    warn_dbus_disabled_once("notifications");
    return 0;
#endif
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
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (id <= 0 || !cb)
      return;
    std::lock_guard<std::mutex> lock(g_notif_mu);
    g_notifications[static_cast<uint32_t>(id)].click_callback = std::move(cb);
#else
    (void)id;
    (void)cb;
    warn_dbus_disabled_once("notification click callbacks");
#endif
  }

  void on_notification_action(
      int id,
      std::function<void(const std::string& action_id, std::optional<std::string> input)> cb) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (id <= 0 || !cb)
      return;
    std::lock_guard<std::mutex> lock(g_notif_mu);
    g_notifications[static_cast<uint32_t>(id)].action_callback = std::move(cb);
#else
    (void)id;
    (void)cb;
    warn_dbus_disabled_once("notification action callbacks");
#endif
  }

  void set_application_menu(const std::vector<menu_item>& items) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    const bool empty_menu = items.empty();
    {
      std::lock_guard<std::mutex> lock(g_menu_mu);
      g_app_menu = build_menu_export_tree(items);
      g_app_menu_item_ids.clear();
      collect_menu_item_ids(g_app_menu, g_app_menu_item_ids);
      ++g_menu_revision;
    }
    DBusConnection* conn = dbus_connection();
    if (!conn)
      return;
    dbus_connection_register_object_path(conn, kDbusMenuPath, &g_object_vtable, nullptr);
    if (empty_menu) {
      (void)unregister_app_menu_with_registrar(conn);
      return;
    }
    (void)register_app_menu_with_registrar(conn);
#else
    (void)items;
    warn_dbus_disabled_once("application menus");
#endif
  }

  bool update_menu_item(std::string_view id, const menu_item_patch& patch) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    std::lock_guard<std::mutex> lock(g_menu_mu);
    auto found_id = g_app_menu_item_ids.find(std::string(id));
    if (found_id == g_app_menu_item_ids.end())
      return false;
    menu_export_node* node = find_menu_node_by_id(g_app_menu, found_id->second);
    if (!node)
      return false;
    if (patch.label)
      node->label = *patch.label;
    if (patch.enabled)
      node->enabled = *patch.enabled;
    if (patch.checked)
      node->checked = *patch.checked;
    if (patch.visible)
      node->visible = *patch.visible;
    if (patch.accelerator)
      node->accelerator = *patch.accelerator;
    ++g_menu_revision;
    return true;
#else
    (void)id;
    (void)patch;
    warn_dbus_disabled_once("menu item updates");
    return false;
#endif
  }

  bool menu_item_exists(std::string_view id) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    std::lock_guard<std::mutex> lock(g_menu_mu);
    return g_app_menu_item_ids.find(std::string(id)) != g_app_menu_item_ids.end();
#else
    (void)id;
    warn_dbus_disabled_once("menu item lookup");
    return false;
#endif
  }
  void show_context_menu(const std::vector<menu_item>& items, int x, int y,
                         std::function<void(const std::string&)> on_select) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    (void)x;
    (void)y;
    DBusConnection* conn = dbus_connection();
    if (!conn) {
      static bool warned = false;
      warn_unsupported_once(warned, "fxe.os: context menus require a D-Bus session bus");
      if (on_select)
        on_select(std::string{});
      return;
    }
    uint32_t revision = 0;
    {
      std::lock_guard<std::mutex> lock(g_menu_mu);
      g_context_menu = build_menu_export_tree(items);
      revision = ++g_context_menu_revision;
    }
    dbus_connection_register_object_path(conn, kContextMenuPath, &g_object_vtable, nullptr);
    emit_dbus_menu_layout_updated(conn, kContextMenuPath, revision);
    // TODO(linux-popup): wire a Wayland/X11 toolkit popup for the requested x/y coordinates.
    static bool popup_warned = false;
    warn_unsupported_once(
        popup_warned,
        "fxe.os: Linux context menu x/y popup is not wired; exported D-BusMenu path only");
    if (on_select)
      on_select(std::string(kContextMenuPath));
#else
    (void)items;
    (void)x;
    (void)y;
    warn_dbus_disabled_once("context menus");
    if (on_select)
      on_select(std::string{});
#endif
  }

  tray_handle tray_create(std::string_view icon_path, std::string_view tooltip) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    DBusConnection* conn = dbus_connection();
    if (!conn) {
      static bool warned = false;
      warn_unsupported_once(warned, "fxe.os: tray icons require a D-Bus session bus");
      return tray_handle{};
    }
    if (!name_has_owner(conn, kSniWatcherBus)) {
      static bool warned = false;
      warn_unsupported_once(warned, "fxe.os: no StatusNotifierWatcher on this Linux desktop");
      return tray_handle{};
    }

    const int id = g_tray_id.fetch_add(1);
    tray_entry entry;
    entry.id = id;
    entry.bus_name = "org.fxe.tray-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                     std::to_string(id);
    entry.icon_path = std::string(icon_path);
    entry.tooltip = std::string(tooltip);

    DBusError error;
    dbus_error_init(&error);
    int request =
        dbus_bus_request_name(conn, entry.bus_name.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    free_error(error);
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
      return tray_handle{};
    {
      std::lock_guard<std::mutex> lock(g_tray_mu);
      g_trays.emplace(id, entry);
    }
    dbus_connection_register_object_path(conn, kSniPath, &g_object_vtable, nullptr);
    dbus_connection_register_object_path(conn, kTrayMenuPath, &g_object_vtable, nullptr);
    if (!register_sni_watcher(conn, entry.bus_name)) {
      {
        std::lock_guard<std::mutex> lock(g_tray_mu);
        g_trays.erase(id);
      }
      dbus_bus_release_name(conn, entry.bus_name.c_str(), nullptr);
      return tray_handle{};
    }
    return tray_handle{id};
#else
    (void)icon_path;
    (void)tooltip;
    warn_dbus_disabled_once("tray icons");
    return tray_handle{};
#endif
  }

  void tray_set_menu(tray_handle handle, const std::vector<menu_item>& items) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (!handle)
      return;
    std::lock_guard<std::mutex> lock(g_tray_mu);
    auto found = g_trays.find(handle.id);
    if (found == g_trays.end())
      return;
    found->second.menu = build_menu_export_tree(items);
#else
    (void)handle;
    (void)items;
    warn_dbus_disabled_once("tray menus");
#endif
  }

  bool tray_set_image(tray_handle handle, std::string_view icon_path) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (!handle)
      return false;
    DBusConnection* conn = dbus_connection();
    if (!conn)
      return false;
    {
      std::lock_guard<std::mutex> lock(g_tray_mu);
      auto found = g_trays.find(handle.id);
      if (found == g_trays.end())
        return false;
      found->second.icon_path = std::string(icon_path);
    }
    emit_sni_signal(conn, "NewIcon");
    return true;
#else
    (void)handle;
    (void)icon_path;
    warn_dbus_disabled_once("tray image updates");
    return false;
#endif
  }

  bool tray_set_title(tray_handle handle, std::string_view title) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (!handle)
      return false;
    DBusConnection* conn = dbus_connection();
    if (!conn)
      return false;
    {
      std::lock_guard<std::mutex> lock(g_tray_mu);
      auto found = g_trays.find(handle.id);
      if (found == g_trays.end())
        return false;
      found->second.title = std::string(title);
    }
    emit_sni_signal(conn, "NewTitle");
    return true;
#else
    (void)handle;
    (void)title;
    warn_dbus_disabled_once("tray title updates");
    return false;
#endif
  }

  bool tray_set_tooltip(tray_handle handle, std::string_view tip) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (!handle)
      return false;
    DBusConnection* conn = dbus_connection();
    if (!conn)
      return false;
    {
      std::lock_guard<std::mutex> lock(g_tray_mu);
      auto found = g_trays.find(handle.id);
      if (found == g_trays.end())
        return false;
      found->second.tooltip = std::string(tip);
    }
    emit_sni_signal(conn, "NewToolTip");
    return true;
#else
    (void)handle;
    (void)tip;
    warn_dbus_disabled_once("tray tooltip updates");
    return false;
#endif
  }

  int tray_on(tray_handle handle, tray_event_kind kind, std::function<void()> cb) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (!handle || !cb)
      return -1;
    std::lock_guard<std::mutex> lock(g_tray_mu);
    if (g_trays.find(handle.id) == g_trays.end())
      return -1;
    int token = g_tray_listener_seq.fetch_add(1);
    g_tray_listeners[token] = tray_listener_entry{handle.id, kind, std::move(cb)};
    return token;
#else
    (void)handle;
    (void)kind;
    (void)cb;
    warn_dbus_disabled_once("tray event listeners");
    return -1;
#endif
  }

  void tray_off(tray_handle handle, int token) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    std::lock_guard<std::mutex> lock(g_tray_mu);
    auto found = g_tray_listeners.find(token);
    if (found != g_tray_listeners.end() && found->second.tray_id == handle.id)
      g_tray_listeners.erase(found);
#else
    (void)handle;
    (void)token;
    warn_dbus_disabled_once("tray event listener removal");
#endif
  }

  void tray_destroy(tray_handle handle) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    if (!handle)
      return;
    DBusConnection* conn = dbus_connection();
    std::string bus_name;
    {
      std::lock_guard<std::mutex> lock(g_tray_mu);
      auto found = g_trays.find(handle.id);
      if (found == g_trays.end())
        return;
      bus_name = found->second.bus_name;
      g_trays.erase(found);
      for (auto it = g_tray_listeners.begin(); it != g_tray_listeners.end();) {
        if (it->second.tray_id == handle.id)
          it = g_tray_listeners.erase(it);
        else
          ++it;
      }
    }
    if (conn) {
      dbus_bus_release_name(conn, bus_name.c_str(), nullptr);
      std::lock_guard<std::mutex> lock(g_tray_mu);
      if (g_trays.empty()) {
        dbus_connection_unregister_object_path(conn, kSniPath);
        dbus_connection_unregister_object_path(conn, kTrayMenuPath);
      }
    }
#else
    (void)handle;
    warn_dbus_disabled_once("tray icon destruction");
#endif
  }

  bool global_shortcut_register(std::string_view accelerator, std::function<void()> cb) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    std::string normalized = normalize_accelerator_for_portal_impl(accelerator);
    if (normalized.empty() || !cb)
      return false;
    DBusConnection* conn = dbus_connection();
    if (!conn) {
      static bool warned = false;
      warn_unsupported_once(warned, "fxe.os: global shortcuts require a D-Bus session bus");
      return false;
    }
    shortcut_entry entry;
    entry.id = "fxe-shortcut-" + std::to_string(std::hash<std::string>{}(std::string(accelerator)));
    entry.accelerator = normalized;
    entry.callback = std::move(cb);
    {
      std::lock_guard<std::mutex> lock(g_shortcut_mu);
      if (g_shortcuts.find(entry.id) != g_shortcuts.end())
        return false;
    }
    if (!bind_portal_shortcut(conn, entry)) {
      static bool warned = false;
      warn_unsupported_once(warned,
                            "fxe.os: GlobalShortcuts portal is unavailable on this desktop");
      return false;
    }
    std::lock_guard<std::mutex> lock(g_shortcut_mu);
    g_shortcuts[entry.id] = std::move(entry);
    return true;
#else
    (void)accelerator;
    (void)cb;
    warn_dbus_disabled_once("global shortcuts");
    return false;
#endif
  }

  void global_shortcut_unregister(std::string_view accelerator) {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    std::string id =
        "fxe-shortcut-" + std::to_string(std::hash<std::string>{}(std::string(accelerator)));
    bool close_session = false;
    {
      std::lock_guard<std::mutex> lock(g_shortcut_mu);
      g_shortcuts.erase(id);
      close_session = g_shortcuts.empty();
    }
    if (close_session)
      close_global_shortcuts_session(dbus_connection());
#else
    (void)accelerator;
    warn_dbus_disabled_once("global shortcut unregister");
#endif
  }

  void global_shortcut_unregister_all() {
#if defined(FXE_HAS_DBUS) && FXE_HAS_DBUS
    {
      std::lock_guard<std::mutex> lock(g_shortcut_mu);
      g_shortcuts.clear();
    }
    close_global_shortcuts_session(dbus_connection());
#else
    warn_dbus_disabled_once("global shortcut unregister all");
#endif
  }

  bool clipboard_set_html(std::string_view utf8) {
    std::vector<uint8_t> bytes(utf8.begin(), utf8.end());
    return set_clipboard_target("text/html", bytes);
  }

  std::optional<std::string> clipboard_get_html() {
    auto bytes = get_clipboard_target("text/html");
    if (!bytes)
      return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  }

  bool clipboard_set_rtf(std::string_view rtf) {
    std::vector<uint8_t> bytes(rtf.begin(), rtf.end());
    return set_clipboard_target("text/rtf", bytes);
  }

  std::optional<std::string> clipboard_get_rtf() {
    auto bytes = get_clipboard_target("text/rtf");
    if (!bytes)
      return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  }

  bool clipboard_set_mime(std::string_view mime, const std::vector<uint8_t>& bytes) {
    return set_clipboard_target(mime, bytes);
  }

  std::optional<std::vector<uint8_t>> clipboard_get_mime(std::string_view mime) {
    return get_clipboard_target(mime);
  }

  void post_main_thread_dispatch(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(g_mu);
    g_q.push(std::move(fn));
  }

  void pump_main_thread_dispatches() {
    for (;;) {
      std::function<void()> fn;

      {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_q.empty())
          break;
        fn = std::move(g_q.front());
        g_q.pop();
      }
      if (fn)
        fn();
    }
  }

  namespace linux_smoke_test {
    std::string normalize_accelerator_for_portal(std::string_view accelerator) {
      return normalize_accelerator_for_portal_impl(accelerator);
    }

    bool dbus_menu_model_round_trip() {
      menu_item root;
      root.id = "root";
      root.label = "Root";
      root.submenu.push_back(menu_item{.id = "child", .label = "Child"});
      std::vector<menu_export_node> tree = build_menu_export_tree({root});
      return count_menu_nodes(tree) == 2 && !tree.empty() && tree.front().id == 1 &&
             tree.front().children.front().id == 2;
    }
  } // namespace linux_smoke_test

  // ---- NEW: single-instance handoff / deep-link / file-open helpers -------
  namespace single_instance_detail {
    std::string encode_handoff(int argc, char** argv);
    bool decode_handoff(std::string_view data, std::vector<std::string>& argv, std::string& cwd);
    void dispatch_launch(std::vector<std::string> argv, std::string cwd);
  } // namespace single_instance_detail

  namespace {
    std::atomic<bool> g_single_instance_listener_started{false};

    std::string linux_single_instance_id() {
      std::string id = env_or_empty("FXE_BUNDLE_ID");
      if (id.empty())
        id = executable_stem();
      return sanitize_lock_component(id.empty() ? "fxe" : id);
    }

    std::string linux_single_instance_socket_path() {
      std::string id = linux_single_instance_id();
      std::filesystem::path dir = single_instance_runtime_dir(id);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec)
        return {};
      return (dir / (id + ".sock")).string();
    }

    bool fill_single_instance_addr(const std::string& path, sockaddr_un& addr) {
      if (path.empty() || path.size() >= sizeof(addr.sun_path))
        return false;
      addr = {};
      addr.sun_family = AF_UNIX;
      for (size_t i = 0; i < path.size(); ++i)
        addr.sun_path[i] = path[i];
      addr.sun_path[path.size()] = '\0';
      return true;
    }

    bool write_all_socket(int fd, const std::string& payload) {
      const char* data = payload.data();
      size_t left = payload.size();
      while (left > 0) {
        ssize_t n = ::write(fd, data, left);
        if (n < 0) {
          if (errno == EINTR)
            continue;
          return false;
        }
        data += n;
        left -= static_cast<size_t>(n);
      }
      return true;
    }

    bool forward_to_primary_socket(const std::string& path, const std::string& payload) {
      sockaddr_un addr{};
      if (!fill_single_instance_addr(path, addr))
        return false;
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0)
        return false;
      bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                write_all_socket(fd, payload);
      ::close(fd);
      return ok;
    }

    void handle_single_instance_client(int client) {
      std::string payload;
      char buffer[4096];
      for (;;) {
        ssize_t n = ::read(client, buffer, sizeof(buffer));
        if (n > 0) {
          payload.append(buffer, static_cast<size_t>(n));
          continue;
        }
        if (n < 0 && errno == EINTR)
          continue;
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
        if (!fill_single_instance_addr(path, addr))
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

    bool valid_url_scheme(std::string_view scheme) {
      if (scheme.empty() || !std::isalpha(static_cast<unsigned char>(scheme.front())))
        return false;
      for (char ch : scheme) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '+' && ch != '-' && ch != '.')
          return false;
      }
      return true;
    }

    std::string current_executable_path() {
      std::error_code ec;
      std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
      return ec ? std::string{} : exe.string();
    }

    bool write_desktop_handler(const std::string& mime_type, const std::string& exec_arg) {
      std::string home = home_dir();
      if (home.empty())
        return false;
      std::string data_home = xdg_data_home(home);
      std::filesystem::path app_dir = std::filesystem::path(data_home) / "applications";
      std::error_code ec;
      std::filesystem::create_directories(app_dir, ec);
      if (ec)
        return false;
      std::string id = linux_single_instance_id();
      std::filesystem::path desktop_path = app_dir / (id + ".desktop");
      std::string exe = current_executable_path();
      if (exe.empty())
        return false;
      std::ofstream desktop(desktop_path, std::ios::trunc);
      if (!desktop)
        return false;
      desktop << "[Desktop Entry]\\n"
              << "Type=Application\\n"
              << "Name=" << id << "\\n"
              << "NoDisplay=true\\n"
              << "Exec=" << exe << " " << exec_arg << "\\n"
              << "MimeType=" << mime_type << ";\\n";
      desktop.close();
      if (!desktop)
        return false;
      return run_command_success(
          {"xdg-mime", "default", desktop_path.filename().string(), mime_type});
    }
  } // namespace

  bool single_instance_platform_acquire_or_forward(int argc, char** argv) {
    std::string id = linux_single_instance_id();
    std::string path = linux_single_instance_socket_path();
    if (request_single_instance_lock(id)) {
      start_single_instance_socket_listener(path);
      return true;
    }
    (void)forward_to_primary_socket(path, single_instance_detail::encode_handoff(argc, argv));
    return false;
  }

  void single_instance_platform_install_open_handlers() {}

  bool single_instance_platform_set_default_protocol_client(const std::string& scheme) {
    if (!valid_url_scheme(scheme))
      return false;
    return write_desktop_handler("x-scheme-handler/" + scheme, "%u");
  }

  bool single_instance_platform_set_default_file_handler(const std::string& ext) {
    std::string clean = ext;
    if (!clean.empty() && clean.front() == '.')
      clean.erase(clean.begin());
    if (clean.empty())
      return false;
    for (char& ch : clean) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (!std::isalnum(c) && ch != '_' && ch != '-' && ch != '.')
        return false;
    }
    return write_desktop_handler("application/x-extension-" + clean, "%f");
  }
#endif
} // namespace fxe::os
