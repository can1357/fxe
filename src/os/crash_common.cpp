#include <fxe/crash.hpp>

#include "os.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <fxe/types.hpp>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef FXE_HAS_CURL
#define FXE_HAS_CURL 0
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#if FXE_HAS_CURL
#include <curl/curl.h>
#endif

namespace fxe::os {
  namespace {
    std::mutex& crash_mutex() {
      static std::mutex m;
      return m;
    }

    crash_options& stored_options() {
      static crash_options opts;
      return opts;
    }

    std::string& stored_dir() {
      static std::string dir;
      return dir;
    }

    std::optional<std::string>& stored_last_dump_path() {
      static std::optional<std::string> path;
      return path;
    }

    std::atomic<unsigned long long>& dump_counter() {
      static std::atomic<unsigned long long> c{0};
      return c;
    }

#ifndef _WIN32
    char g_signal_dir[PATH_MAX] = {0};
#endif

    std::string default_crash_dir() {
      std::string base = get_path("userData");
      if (base.empty()) {
        std::error_code ec;
        base = (std::filesystem::temp_directory_path(ec) / "fxe").string();
      }
      if (base.empty())
        return {};
      return (std::filesystem::path(base) / "Crashes").string();
    }

    std::string resolve_dir(const crash_options& opts) {
      if (!opts.crash_dir.empty())
        return opts.crash_dir;
      return default_crash_dir();
    }

    bool ensure_dir(const std::string& dir) {
      if (dir.empty())
        return false;
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      return !ec && std::filesystem::is_directory(dir, ec);
    }

    std::string json_escape(std::string_view value) {
      std::string out;
      out.reserve(value.size() + 8);
      for (char ch : value) {
        switch (ch) {
        case '\\':
          out += "\\\\";
          break;
        case '"':
          out += "\\\"";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          out.push_back(ch);
          break;
        }
      }
      return out;
    }

    std::vector<std::string> parse_json_string_array(const std::string& text) {
      std::vector<std::string> result;
      std::string current;
      bool in_string = false;
      bool escape = false;
      for (char ch : text) {
        if (!in_string) {
          if (ch == '"') {
            in_string = true;
            current.clear();
          }
          continue;
        }
        if (escape) {
          switch (ch) {
          case 'n':
            current.push_back('\n');
            break;
          case 'r':
            current.push_back('\r');
            break;
          case 't':
            current.push_back('\t');
            break;
          default:
            current.push_back(ch);
            break;
          }
          escape = false;
          continue;
        }
        if (ch == '\\') {
          escape = true;
          continue;
        }
        if (ch == '"') {
          in_string = false;
          result.push_back(current);
          continue;
        }
        current.push_back(ch);
      }
      return result;
    }

    std::optional<std::filesystem::path> uploaded_sidecar_path() {
      std::string dir = crash_detail::current_crash_dir();
      if (dir.empty())
        return std::nullopt;
      return std::filesystem::path(dir) / "uploaded.json";
    }

    std::vector<std::string> read_uploaded_sidecar() {
      auto sidecar = uploaded_sidecar_path();
      if (!sidecar)
        return {};
      std::ifstream in(*sidecar, std::ios::binary);
      if (!in)
        return {};
      std::ostringstream ss;
      ss << in.rdbuf();
      return parse_json_string_array(ss.str());
    }

    bool atomic_write_text_file(const std::filesystem::path& path, std::string_view text) {
      std::filesystem::path tmp = path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
          return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out)
          return false;
      }
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
      if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
      }
      return true;
    }

    void record_uploaded(const std::string& path) {
      auto entries = read_uploaded_sidecar();
      if (std::find(entries.begin(), entries.end(), path) == entries.end())
        entries.push_back(path);
      std::ostringstream out;
      out << "[\n";
      for (usize i = 0; i < entries.size(); ++i) {
        out << "  \"" << json_escape(entries[i]) << "\"";
        if (i + 1 < entries.size())
          out << ',';
        out << '\n';
      }
      out << "]\n";
      auto sidecar = uploaded_sidecar_path();
      if (sidecar)
        (void)atomic_write_text_file(*sidecar, out.str());
    }

#ifdef _WIN32
    std::wstring to_wide(const std::string& value) {
      if (value.empty())
        return {};
      int size =
          MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (size <= 0)
        return {};
      std::wstring out(static_cast<usize>(size), L'\0');
      (void)MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                out.data(), size);
      return out;
    }

    bool upload_with_winhttp(const crash_options& opts, const std::string& path) {
      std::wstring url = to_wide(opts.submit_url);
      if (url.empty())
        return false;

      URL_COMPONENTS parts{};
      parts.dwStructSize = sizeof(parts);
      parts.dwSchemeLength = static_cast<DWORD>(-1);
      parts.dwHostNameLength = static_cast<DWORD>(-1);
      parts.dwUrlPathLength = static_cast<DWORD>(-1);
      parts.dwExtraInfoLength = static_cast<DWORD>(-1);
      if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        return false;

      std::ifstream in(path, std::ios::binary);
      if (!in)
        return false;
      std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

      HINTERNET session = WinHttpOpen(L"fxe-crash-reporter/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
      if (!session)
        return false;
      std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
      HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
      if (!connect) {
        WinHttpCloseHandle(session);
        return false;
      }
      std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
      if (parts.dwExtraInfoLength > 0)
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
      DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
      HINTERNET request =
          WinHttpOpenRequest(connect, L"POST", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
      if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
      }
      const wchar_t* headers = L"Content-Type: application/octet-stream\r\n";
      DWORD body_size = body.size() > static_cast<usize>(std::numeric_limits<DWORD>::max())
                            ? 0
                            : static_cast<DWORD>(body.size());
      bool ok = body_size > 0 || body.empty();
      ok = ok && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                                    body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data(), body_size,
                                    body_size, 0);
      ok = ok && WinHttpReceiveResponse(request, nullptr);
      DWORD status = 0;
      DWORD status_size = sizeof(status);
      if (ok) {
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX) &&
             status >= 200 && status < 300;
      }
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return ok;
    }
#endif

#if FXE_HAS_CURL
    bool upload_with_curl(const crash_options& opts, const std::string& path) {
      CURL* curl = curl_easy_init();
      if (!curl)
        return false;

      curl_mime* mime = curl_mime_init(curl);
      if (!mime) {
        curl_easy_cleanup(curl);
        return false;
      }
      curl_mimepart* part = curl_mime_addpart(mime);
      curl_mime_name(part, "upload_file_minidump");
      curl_mime_filedata(part, path.c_str());
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "prod");
      curl_mime_data(part, opts.product_name.c_str(), CURL_ZERO_TERMINATED);
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "ver");
      curl_mime_data(part, opts.product_version.c_str(), CURL_ZERO_TERMINATED);

      curl_easy_setopt(curl, CURLOPT_URL, opts.submit_url.c_str());
      curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      CURLcode rc = curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_mime_free(mime);
      curl_easy_cleanup(curl);
      return rc == CURLE_OK && status >= 200 && status < 300;
    }
#endif

#if !defined(_WIN32) && !FXE_HAS_CURL
    std::string shell_quote(std::string_view value) {
      std::string out = "'";
      for (char ch : value) {
        if (ch == '\'') {
          out += "'\\''";
        } else {
          out.push_back(ch);
        }
      }
      out += "'";
      return out;
    }

    bool upload_with_system_curl(const crash_options& opts, const std::string& path) {
      std::string command = "curl -fsS --max-time 10 -X POST";
      command += " -F upload_file_minidump=@" + shell_quote(path);
      command += " -F prod=" + shell_quote(opts.product_name);
      command += " -F ver=" + shell_quote(opts.product_version);
      command += " " + shell_quote(opts.submit_url);
      int rc = std::system(command.c_str());
      return rc == 0;
    }
#endif

    [[maybe_unused]] bool is_dump_parseable(const std::string& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in)
        return false;
      char magic[16] = {0};
      in.read(magic, sizeof(magic));
      std::streamsize n = in.gcount();
      if (n >= 4 && std::string_view(magic, 4) == "MDMP")
        return true;
      if (n >= 8 && std::string_view(magic, 8) == "FXELMDP1")
        return true;
      if (n >= 14 && std::string_view(magic, 14) == "fxe crash dump")
        return true;
      return false;
    }
  } // namespace

  bool crash_start(const crash_options& input) {
    crash_options opts = input;
    std::string dir = resolve_dir(opts);
    if (!ensure_dir(dir))
      return false;
    opts.crash_dir = dir;

    {
      std::lock_guard<std::mutex> lock(crash_mutex());
      stored_options() = opts;
      stored_dir() = dir;
#ifndef _WIN32
      std::snprintf(g_signal_dir, sizeof(g_signal_dir), "%s", dir.c_str());
#endif
    }

    return crash_detail::platform_install_handlers(opts);
  }

  std::vector<std::string> crash_list_uploaded() {
    return read_uploaded_sidecar();
  }

  std::optional<std::string> crash_get_last_dump_path() {
    std::lock_guard<std::mutex> lock(crash_mutex());
    return stored_last_dump_path();
  }

  crash_self_test_result crash_self_test() {
    crash_self_test_result result;
#ifdef NDEBUG
    result.error = "crash self-test is disabled in release builds";
    return result;
#else
    crash_options opts = crash_detail::current_options();
    if (opts.crash_dir.empty()) {
      opts.product_name = opts.product_name.empty() ? "fxe" : opts.product_name;
      opts.product_version = opts.product_version.empty() ? "self-test" : opts.product_version;
      opts.upload_to_server = false;
      if (!crash_start(opts)) {
        result.error = "failed to start crash reporter";
        return result;
      }
    }

    std::vector<std::string> before = crash_detail::list_dump_paths();
    if (!crash_detail::platform_self_test()) {
      result.error = "platform crash self-test did not produce a clean trap dump";
      return result;
    }

    std::vector<std::string> after = crash_detail::list_dump_paths();
    for (const auto& candidate : after) {
      if (std::find(before.begin(), before.end(), candidate) != before.end())
        continue;
      if (!is_dump_parseable(candidate))
        continue;
      result.ok = true;
      result.dump_path = candidate;
      std::error_code ec;
      std::filesystem::remove(candidate, ec);
      return result;
    }
    result.error = "self-test dump was not parseable";
    return result;
#endif
  }

  namespace crash_detail {
    std::string current_crash_dir() {
      std::lock_guard<std::mutex> lock(crash_mutex());
      return stored_dir();
    }

    crash_options current_options() {
      std::lock_guard<std::mutex> lock(crash_mutex());
      return stored_options();
    }

    std::string next_dump_path(const char* extension) {
      auto now = std::chrono::system_clock::now();
      auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
#ifdef _WIN32
      unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
      unsigned long pid = static_cast<unsigned long>(getpid());
#endif
      char filename[128] = {0};
      std::snprintf(filename, sizeof(filename), "crash-%lu-%lld.%s", pid,
                    static_cast<long long>(millis.count()), extension ? extension : "dmp");
      return (std::filesystem::path(current_crash_dir()) / filename).string();
    }

    void record_last_dump_path(std::string path) {
      std::lock_guard<std::mutex> lock(crash_mutex());
      stored_last_dump_path() = std::move(path);
    }

    bool write_dump_bytes(const char* extension, const void* data, usize size) {
      if (!data && size > 0)
        return false;
      std::string path = next_dump_path(extension);
      std::filesystem::path final_path(path);
      std::filesystem::path tmp = final_path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
          return false;
        if (size > 0)
          out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out)
          return false;
      }
      std::error_code ec;
      std::filesystem::rename(tmp, final_path, ec);
      if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
      }
      record_last_dump_path(path);
      (void)upload_last_dump_if_requested(path);
      return true;
    }

    bool write_dump_text(const char* extension, std::string_view text) {
      return write_dump_bytes(extension, text.data(), text.size());
    }

    bool upload_last_dump_if_requested(const std::string& path) {
      crash_options opts = current_options();
      if (!opts.upload_to_server || opts.submit_url.empty() || path.empty())
        return false;
#ifdef _WIN32
      bool ok = upload_with_winhttp(opts, path);
#elif FXE_HAS_CURL
      bool ok = upload_with_curl(opts, path);
#else
      bool ok = upload_with_system_curl(opts, path);
#endif
      if (ok)
        record_uploaded(path);
      return ok;
    }

    std::vector<std::string> list_dump_paths() {
      std::vector<std::string> dumps;
      std::string dir = current_crash_dir();
      if (dir.empty())
        return dumps;
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
          break;
        if (!entry.is_regular_file(ec))
          continue;
        std::string ext = entry.path().extension().string();
        if (ext == ".dmp" || ext == ".txt")
          dumps.push_back(entry.path().string());
      }
      std::sort(dumps.begin(), dumps.end());
      return dumps;
    }

#ifndef _WIN32
    bool signal_next_dump_path(const char* extension, char* out, usize out_size) noexcept {
      if (!out || out_size == 0 || g_signal_dir[0] == '\0')
        return false;
      unsigned long long counter = dump_counter().fetch_add(1, std::memory_order_relaxed) + 1;
      long pid = static_cast<long>(getpid());
      long long epoch = static_cast<long long>(time(nullptr));
      int written = std::snprintf(out, out_size, "%s/crash-%ld-%lld-%llu.%s", g_signal_dir, pid,
                                  epoch, counter, extension ? extension : "dmp");
      return written > 0 && static_cast<usize>(written) < out_size;
    }

    void write_signal_dump(int signal_number, const void* address, const char* extension) noexcept {
      char path[PATH_MAX] = {0};
      if (!signal_next_dump_path(extension, path, sizeof(path)))
        return;
      int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
      if (fd < 0)
        return;
      char buf[256] = {0};
      int n = std::snprintf(buf, sizeof(buf),
                            "fxe crash dump\nsignal=%d\naddress=%p\nformat=signal-text\n",
                            signal_number, address);
      if (n > 0)
        (void)write(fd, buf, static_cast<usize>(n));
      (void)close(fd);
    }
#endif
  } // namespace crash_detail
} // namespace fxe::os
