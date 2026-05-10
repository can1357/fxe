#include "http_client.hpp"
#include "runtime/uv_loop.hpp"
#include "runtime/v8/native/https_transport.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fxe/string_utils.hpp>
#include <fxe/types.hpp>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

#ifdef FXE_HAS_CURL
#include <curl/curl.h>
#endif

namespace fxe::net {

  namespace {
    struct parsed_url {
      std::string scheme;
      std::string host;
      std::string path = "/";
    };

#ifdef FXE_HAS_CURL
    parsed_url parse_url_for_cookies(const std::string& url) {
      parsed_url out;
      auto scheme_end = url.find("://");
      usize authority = 0;
      if (scheme_end != std::string::npos) {
        out.scheme = ascii_lower(url.substr(0, scheme_end));
        authority = scheme_end + 3;
      }
      auto path_pos = url.find('/', authority);
      std::string host_port = path_pos == std::string::npos
                                  ? url.substr(authority)
                                  : url.substr(authority, path_pos - authority);
      if (path_pos != std::string::npos)
        out.path = url.substr(path_pos);
      auto at = host_port.rfind('@');
      if (at != std::string::npos)
        host_port.erase(0, at + 1);
      if (!host_port.empty() && host_port.front() == '[') {
        auto end = host_port.find(']');
        out.host = end == std::string::npos ? host_port : host_port.substr(1, end - 1);
      } else {
        auto colon = host_port.rfind(':');
        out.host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
      }
      out.host = ascii_lower(out.host);
      return out;
    }
#endif

  } // namespace

  void multipart_form::add_field(std::string name, std::string value) {
    part p;
    p.name = std::move(name);
    p.value = std::move(value);
    p.file = false;
    parts_.push_back(std::move(p));
  }

  void multipart_form::add_file(std::string name, std::string filename, std::string content_type,
                                std::string bytes) {
    part p;
    p.name = std::move(name);
    p.filename = std::move(filename);
    p.content_type = std::move(content_type);
    p.bytes = std::move(bytes);
    p.file = true;
    parts_.push_back(std::move(p));
  }
  void multipart_form::add_file_stream(std::string name, std::string filename,
                                       std::string content_type, upload_body_source body_source,
                                       std::optional<std::int64_t> size_hint) {
    part p;
    p.name = std::move(name);
    p.filename = std::move(filename);
    p.content_type = std::move(content_type);
    p.body_source = std::move(body_source);
    p.size_hint = size_hint;
    p.file = true;
    parts_.push_back(std::move(p));
  }

  namespace {
    const char* http_error_code_name(http_error code) {
      switch (code) {
      case http_error::ok:
        return "ok";
      case http_error::no_backend:
        return "no_backend";
      case http_error::dns:
        return "dns";
      case http_error::connect:
        return "connect";
      case http_error::tls:
        return "tls";
      case http_error::timeout:
        return "timeout";
      case http_error::response_truncated:
        return "response_truncated";
      case http_error::abort:
        return "abort";
      case http_error::unknown:
        return "unknown";
      }
      return "unknown";
    }

    void set_http_error(http_response& resp, http_error code, std::string message) {
      resp.last_error = code;
      if (code != http_error::ok) {
        message += " (code: ";
        message += http_error_code_name(code);
        message += ")";
      }
      resp.error = std::move(message);
    }
  } // namespace

#ifdef FXE_HAS_CURL

  // HTTPS is delegated to libcurl. When the shared libuv runtime loop is
  // available, this client registers its libcurl-multi progress with that loop
  // so fetch completions are advanced by the normal app/runtime pump. The
  // explicit poll() path remains a truthful fallback for builds or callers
  // without libuv.

  namespace {
    constexpr long kDefaultTotalTimeoutMs = 60000L;
    constexpr long kDefaultConnectTimeoutMs = 30000L;
    constexpr usize kMaxActiveTotal = 64;
    constexpr usize kMaxActivePerOrigin = 6;
    constexpr usize kMaxPendingTotal = 256;

    struct origin_key {
      std::string key;
    };

    origin_key normalized_origin_key(const std::string& url) {
      auto scheme_end = url.find("://");
      std::string scheme =
          scheme_end == std::string::npos ? "http" : ascii_lower(url.substr(0, scheme_end));
      usize authority = scheme_end == std::string::npos ? 0 : scheme_end + 3;
      auto path_pos = url.find_first_of("/?#", authority);
      std::string host_port = path_pos == std::string::npos
                                  ? url.substr(authority)
                                  : url.substr(authority, path_pos - authority);
      auto at = host_port.rfind('@');
      if (at != std::string::npos)
        host_port.erase(0, at + 1);

      std::string host;
      int port = scheme == "https" ? 443 : 80;
      if (!host_port.empty() && host_port.front() == '[') {
        auto end = host_port.find(']');
        host = end == std::string::npos ? host_port : host_port.substr(1, end - 1);
        if (end != std::string::npos && end + 1 < host_port.size() && host_port[end + 1] == ':') {
          const std::string port_s = host_port.substr(end + 2);
          if (!port_s.empty())
            port = std::atoi(port_s.c_str());
        }
      } else {
        auto colon = host_port.rfind(':');
        if (colon != std::string::npos) {
          host = host_port.substr(0, colon);
          const std::string port_s = host_port.substr(colon + 1);
          if (!port_s.empty() && std::all_of(port_s.begin(), port_s.end(),
                                             [](unsigned char c) { return std::isdigit(c) != 0; }))
            port = std::atoi(port_s.c_str());
        } else {
          host = host_port;
        }
      }

      host = ascii_lower(host);
      if (port <= 0)
        port = scheme == "https" ? 443 : 80;
      return origin_key{scheme + "://" + host + ":" + std::to_string(port)};
    }

    struct multipart_stream_state {
      upload_body_source body_source;
    };

    struct in_flight {
      http_request_id id;
      CURL* easy = nullptr;
      curl_slist* slist = nullptr;
      http_callback cb;
      http_response resp;
      std::string body_buf;
      std::string upload_buf;
      upload_body_source body_source;
      multipart_form multipart;
      std::vector<std::unique_ptr<multipart_stream_state>> multipart_streams;
      std::string request_url;
      std::string origin;
      std::vector<std::string> set_cookie_headers;
      usize upload_off = 0;
      bool aborted = false;
      char error_buf[CURL_ERROR_SIZE] = {};
      curl_mime* mime = nullptr;
      std::string proxy_url;
      std::string proxy_userpwd;
    };

    struct queued_request {
      http_request_id id;
      http_request req;
      http_callback cb;
      std::string origin;
      bool aborted = false;
      bool settled = false;
    };

    static usize write_cb(char* ptr, usize size, usize nmemb, void* user) {
      auto* fl = static_cast<in_flight*>(user);
      const usize n = size * nmemb;
      fl->body_buf.append(ptr, n);
      return n;
    }

    static usize header_cb(char* ptr, usize size, usize nmemb, void* user) {
      auto* fl = static_cast<in_flight*>(user);
      const usize n = size * nmemb;
      std::string line(ptr, n);
      // Trim CRLF
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
      if (line.empty())
        return n;
      // Status-line "HTTP/1.1 200 OK"
      if (line.rfind("HTTP/", 0) == 0) {
        // reset on each status (handles redirects)
        fl->resp.headers.clear();
        auto sp1 = line.find(' ');
        if (sp1 != std::string::npos) {
          auto sp2 = line.find(' ', sp1 + 1);
          if (sp2 != std::string::npos)
            fl->resp.status_text = line.substr(sp2 + 1);
        }
        return n;
      }
      auto colon = line.find(':');
      if (colon == std::string::npos)
        return n;
      std::string name = line.substr(0, colon);
      std::string val = line.substr(colon + 1);
      // ltrim
      usize i = 0;
      while (i < val.size() && (val[i] == ' ' || val[i] == '\t'))
        ++i;
      val.erase(0, i);
      if (ascii_lower(name) == "set-cookie")
        fl->set_cookie_headers.push_back(val);
      fl->resp.headers.emplace_back(std::move(name), std::move(val));
      return n;
    }

    static usize read_cb(char* ptr, usize size, usize nmemb, void* user) {
      auto* fl = static_cast<in_flight*>(user);
      const usize want = size * nmemb;
      if (fl->body_source) {
        auto [n, done] = fl->body_source(reinterpret_cast<unsigned char*>(ptr), want);
        if (n == 0)
          return done ? 0 : CURL_READFUNC_PAUSE;
        return n;
      }
      const usize avail = fl->upload_buf.size() - fl->upload_off;
      const usize n = avail < want ? avail : want;
      if (n)
        std::memcpy(ptr, fl->upload_buf.data() + fl->upload_off, n);
      fl->upload_off += n;
      return n;
    }

    bool header_exists(const header_list& headers, const std::string& lower_name) {
      for (const auto& [k, _] : headers) {
        if (ascii_lower(k) == lower_name)
          return true;
      }
      return false;
    }

    bool no_proxy_matches(const std::string& no_proxy, const std::string& url) {
      if (no_proxy.empty())
        return false;
      const auto parsed = parse_url_for_cookies(url);
      std::stringstream ss(no_proxy);
      std::string token;
      while (std::getline(ss, token, ',')) {
        token = ascii_lower(std::string(trim(token)));
        if (token.empty())
          continue;
        if (token == "*")
          return true;
        if (!token.empty() && token.front() == '.')
          token.erase(0, 1);
        if (parsed.host == token)
          return true;
        if (parsed.host.size() > token.size() &&
            parsed.host.compare(parsed.host.size() - token.size(), token.size(), token) == 0 &&
            parsed.host[parsed.host.size() - token.size() - 1] == '.')
          return true;
      }
      return false;
    }

    const char* getenv_any(const char* upper, const char* lower) {
      if (const char* v = std::getenv(upper); v && *v)
        return v;
      if (const char* v = std::getenv(lower); v && *v)
        return v;
      return nullptr;
    }

    std::string proxy_from_env(const std::string& url, bool* disabled_by_no_proxy) {
      *disabled_by_no_proxy = false;
      const char* no_proxy = getenv_any("NO_PROXY", "no_proxy");
      if (no_proxy && no_proxy_matches(no_proxy, url)) {
        *disabled_by_no_proxy = true;
        return {};
      }
      const auto parsed = parse_url_for_cookies(url);
      const char* proxy = nullptr;
      if (parsed.scheme == "https")
        proxy = getenv_any("HTTPS_PROXY", "https_proxy");
      if (!proxy)
        proxy = getenv_any("HTTP_PROXY", "http_proxy");
      return proxy ? std::string(proxy) : std::string();
    }

    long proxy_type_from_url(const std::string& proxy) {
      const auto lower = ascii_lower(proxy);
      if (lower.rfind("socks5h://", 0) == 0)
        return CURLPROXY_SOCKS5_HOSTNAME;
      if (lower.rfind("socks5://", 0) == 0)
        return CURLPROXY_SOCKS5;
      if (lower.rfind("socks4a://", 0) == 0)
        return CURLPROXY_SOCKS4A;
      if (lower.rfind("socks4://", 0) == 0)
        return CURLPROXY_SOCKS4;
      return CURLPROXY_HTTP;
    }

    void configure_proxy(CURL* easy, in_flight& fl, const http_request& req) {
      bool disabled_by_no_proxy = false;
      fl.proxy_url = req.proxy.empty() ? proxy_from_env(req.url, &disabled_by_no_proxy) : req.proxy;
      if (disabled_by_no_proxy) {
        curl_easy_setopt(easy, CURLOPT_PROXY, "");
        return;
      }
      if (fl.proxy_url.empty())
        return;
      auto scheme_end = fl.proxy_url.find("://");
      auto authority = scheme_end == std::string::npos ? 0 : scheme_end + 3;
      auto at = fl.proxy_url.find('@', authority);
      if (at != std::string::npos) {
        fl.proxy_userpwd = fl.proxy_url.substr(authority, at - authority);
        fl.proxy_url.erase(authority, at - authority + 1);
      }
      curl_easy_setopt(easy, CURLOPT_PROXY, fl.proxy_url.c_str());
      curl_easy_setopt(easy, CURLOPT_PROXYTYPE, proxy_type_from_url(fl.proxy_url));
      if (!fl.proxy_userpwd.empty())
        curl_easy_setopt(easy, CURLOPT_PROXYUSERPWD, fl.proxy_userpwd.c_str());
    }

    static usize mime_read_cb(char* ptr, usize size, usize nmemb, void* user) {
      auto* state = static_cast<multipart_stream_state*>(user);
      if (!state || !state->body_source)
        return 0;
      const usize want = size * nmemb;
      auto [n, done] = state->body_source(reinterpret_cast<unsigned char*>(ptr), want);
      if (n == 0)
        return done ? 0 : CURL_READFUNC_PAUSE;
      return n;
    }

    static int mime_seek_cb(void* user, curl_off_t offset, int origin) {
      (void)user;
      (void)offset;
      (void)origin;
      return CURL_SEEKFUNC_CANTSEEK;
    }

    bool configure_multipart(CURL* easy, in_flight& fl, const multipart_form& multipart) {
      if (multipart.empty())
        return false;
      fl.mime = curl_mime_init(easy);
      if (!fl.mime)
        return false;
      for (const auto& p : multipart.parts()) {
        curl_mimepart* part = curl_mime_addpart(fl.mime);
        if (!part)
          return false;
        curl_mime_name(part, p.name.c_str());
        if (p.file) {
          curl_mime_filename(part, p.filename.c_str());
          if (!p.content_type.empty())
            curl_mime_type(part, p.content_type.c_str());
          if (p.body_source) {
            auto stream_state = std::make_unique<multipart_stream_state>();
            stream_state->body_source = p.body_source;
            auto* raw_stream_state = stream_state.get();
            fl.multipart_streams.push_back(std::move(stream_state));
            const curl_off_t size_hint =
                p.size_hint.has_value() ? static_cast<curl_off_t>(*p.size_hint) : -1;
            curl_mime_data_cb(part, size_hint, mime_read_cb, mime_seek_cb, nullptr,
                              raw_stream_state);
          } else {
            curl_mime_data(part, p.bytes.data(), p.bytes.size());
          }
        } else {
          curl_mime_data(part, p.value.c_str(), p.value.size());
        }
      }
      curl_easy_setopt(easy, CURLOPT_MIMEPOST, fl.mime);
      return true;
    }

    bool is_https_url(const std::string& url) {
      constexpr char k_https[] = "https://";
      if (url.size() < sizeof(k_https) - 1)
        return false;
      for (usize i = 0; i < sizeof(k_https) - 1; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        if (c != k_https[i])
          return false;
      }
      return true;
    }

    bool curl_supports_https() {
      auto* info = curl_version_info(CURLVERSION_NOW);
      if (!info || (info->features & CURL_VERSION_SSL) == 0)
        return false;
      if (!info->protocols)
        return false;
      for (const char* const* proto = info->protocols; *proto; ++proto) {
        if (std::strcmp(*proto, "https") == 0)
          return true;
      }
      return false;
    }

    std::string curl_error_message(CURLcode code, const in_flight& fl) {
      std::string msg = "fetch failed: libcurl error ";
      msg += std::to_string(static_cast<int>(code));
      msg += " (";
      msg += curl_easy_strerror(code);
      msg += ")";
      if (fl.error_buf[0] != '\0') {
        msg += ": ";
        msg += fl.error_buf;
      }
      return msg;
    }

    http_error http_error_from_curl(CURLcode code) {
      switch (code) {
      case CURLE_OK:
        return http_error::ok;
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_RESOLVE_PROXY:
        return http_error::dns;
      case CURLE_COULDNT_CONNECT:
        return http_error::connect;
      case CURLE_OPERATION_TIMEDOUT:
        return http_error::timeout;
      case CURLE_PARTIAL_FILE:
        return http_error::response_truncated;
      case CURLE_ABORTED_BY_CALLBACK:
        return http_error::abort;
      case CURLE_SSL_CONNECT_ERROR:
      case CURLE_PEER_FAILED_VERIFICATION:
      case CURLE_SSL_CERTPROBLEM:
      case CURLE_SSL_CIPHER:
      case CURLE_SSL_ENGINE_NOTFOUND:
      case CURLE_SSL_ENGINE_SETFAILED:
      case CURLE_SSL_ENGINE_INITFAILED:
      case CURLE_SSL_CACERT_BADFILE:
      case CURLE_SSL_SHUTDOWN_FAILED:
      case CURLE_USE_SSL_FAILED:
      case CURLE_SSL_CRL_BADFILE:
      case CURLE_SSL_ISSUER_ERROR:
      case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
      case CURLE_SSL_INVALIDCERTSTATUS:
      case CURLE_SSL_CLIENTCERT:
        return http_error::tls;
      default:
        return http_error::unknown;
      }
    }
  } // namespace

  struct http_client::impl {
    CURLM* multi = nullptr;
    bool ok = false;
    bool supports_https = false;
    std::atomic<http_request_id> next_id{1};
    std::mutex in_flight_mu;
    std::unordered_map<http_request_id, in_flight*> by_id;
    std::unordered_map<CURL*, in_flight*> by_easy;
    std::unordered_map<std::string, usize> active_by_origin;
    std::deque<std::shared_ptr<queued_request>> pending_fifo;
    std::unordered_map<http_request_id, std::shared_ptr<queued_request>> pending_by_id;
    usize active_total = 0;
    usize pending_total = 0;
    cookie_jar& jar;

    usize pump_callback_id = 0;
    explicit impl(cookie_jar& cookie_store) : jar(cookie_store) {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
        multi = curl_multi_init();
        ok = (multi != nullptr);
        supports_https = curl_supports_https();
      }
      if (ok) {
        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                          static_cast<long>(kMaxActiveTotal));
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                          static_cast<long>(kMaxActivePerOrigin));
        pump_callback_id = fxe::runtime::uv_loop_runtime::instance().register_pump_callback(
            [this] { poll_once(); });
      }
    }
    ~impl() {
      fxe::runtime::uv_loop_runtime::instance().unregister_pump_callback(pump_callback_id);
      pending_by_id.clear();
      pending_fifo.clear();
      for (auto& [_, fl] : by_id)
        cleanup_in_flight(fl);
      if (multi)
        curl_multi_cleanup(multi);
      curl_global_cleanup();
    }

    bool can_start(const std::string& origin) const {
      if (active_total >= kMaxActiveTotal)
        return false;
      auto it = active_by_origin.find(origin);
      const usize active_for_origin = it == active_by_origin.end() ? 0 : it->second;
      return active_for_origin < kMaxActivePerOrigin;
    }

    void note_active(const std::string& origin) {
      ++active_total;
      ++active_by_origin[origin];
    }

    void release_active(const std::string& origin) {
      if (active_total == 0) {
        fxe::runtime::uv_loop_runtime::instance().report_error(
            "http_client pool release underflow");
      } else {
        --active_total;
      }
      auto it = active_by_origin.find(origin);
      if (it == active_by_origin.end() || it->second == 0) {
        fxe::runtime::uv_loop_runtime::instance().report_error(
            "http_client origin release underflow: " + origin);
        return;
      }
      --it->second;
      if (it->second == 0)
        active_by_origin.erase(it);
    }

    void cleanup_in_flight(in_flight* fl) {
      if (!fl)
        return;
      if (fl->easy) {
        curl_multi_remove_handle(multi, fl->easy);
        if (fl->mime)
          curl_mime_free(fl->mime);
        curl_easy_cleanup(fl->easy);
      }
      if (fl->slist)
        curl_slist_free_all(fl->slist);
      delete fl;
    }

    void reject_request(http_callback cb, http_error code, std::string message) {
      http_response r;
      set_http_error(r, code, std::move(message));
      if (cb)
        cb(std::move(r));
    }

    bool start_request(http_request_id id, http_request req, http_callback cb, std::string origin) {
      auto* fl = new in_flight();
      fl->id = id;
      fl->cb = std::move(cb);
      fl->request_url = req.url;
      fl->origin = std::move(origin);
      fl->upload_buf = std::move(req.body);
      fl->multipart = std::move(req.multipart);
      fl->body_source = std::move(req.body_source);
      fl->easy = curl_easy_init();
      if (!fl->easy) {
        auto fail_cb = std::move(fl->cb);
        delete fl;
        reject_request(std::move(fail_cb), http_error::unknown, "curl_easy_init failed");
        return false;
      }

      curl_easy_setopt(fl->easy, CURLOPT_URL, req.url.c_str());
      curl_easy_setopt(fl->easy, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
      curl_easy_setopt(fl->easy, CURLOPT_MAXREDIRS, 20L);
      curl_easy_setopt(fl->easy, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(fl->easy, CURLOPT_WRITEFUNCTION, write_cb);
      curl_easy_setopt(fl->easy, CURLOPT_WRITEDATA, fl);
      curl_easy_setopt(fl->easy, CURLOPT_HEADERFUNCTION, header_cb);
      curl_easy_setopt(fl->easy, CURLOPT_HEADERDATA, fl);
      curl_easy_setopt(fl->easy, CURLOPT_ERRORBUFFER, fl->error_buf);
      curl_easy_setopt(fl->easy, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(fl->easy, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(fl->easy, CURLOPT_MAXCONNECTS, static_cast<long>(kMaxActiveTotal));
      configure_proxy(fl->easy, *fl, req);

      const long total_timeout_ms =
          req.timeout_ms > 0 ? static_cast<long>(req.timeout_ms) : kDefaultTotalTimeoutMs;
      const long connect_timeout_ms =
          req.timeout_ms > 0 ? static_cast<long>(req.timeout_ms) : kDefaultConnectTimeoutMs;
      curl_easy_setopt(fl->easy, CURLOPT_TIMEOUT_MS, total_timeout_ms);
      curl_easy_setopt(fl->easy, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);

      if (!req.range.empty() && !header_exists(req.headers, "range"))
        req.headers.emplace_back("Range", req.range);
      const std::string jar_cookie = jar.pick_for_request(req.url);
      if (!jar_cookie.empty()) {
        bool merged_cookie = false;
        for (auto& [k, v] : req.headers) {
          if (ascii_lower(k) == "cookie") {
            v = v.empty() ? jar_cookie : jar_cookie + "; " + v;
            merged_cookie = true;
            break;
          }
        }
        if (!merged_cookie)
          req.headers.emplace_back("Cookie", jar_cookie);
      }

      const std::string& m = req.method;
      if (!fl->multipart.empty()) {
        if (!configure_multipart(fl->easy, *fl, fl->multipart)) {
          auto fail_cb = std::move(fl->cb);
          cleanup_in_flight(fl);
          reject_request(std::move(fail_cb), http_error::unknown, "curl_mime_init failed");
          return false;
        }
        if (!m.empty() && m != "POST")
          curl_easy_setopt(fl->easy, CURLOPT_CUSTOMREQUEST, m.c_str());
      } else if (m == "GET" || m == "") {
        curl_easy_setopt(fl->easy, CURLOPT_HTTPGET, 1L);
      } else if (m == "HEAD") {
        curl_easy_setopt(fl->easy, CURLOPT_NOBODY, 1L);
      } else if (m == "POST") {
        if (fl->body_source) {
          curl_easy_setopt(fl->easy, CURLOPT_CUSTOMREQUEST, m.c_str());
          curl_easy_setopt(fl->easy, CURLOPT_UPLOAD, 1L);
          curl_easy_setopt(fl->easy, CURLOPT_READFUNCTION, read_cb);
          curl_easy_setopt(fl->easy, CURLOPT_READDATA, fl);
          if (req.body_size_hint.has_value()) {
            curl_easy_setopt(fl->easy, CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(*req.body_size_hint));
          }
        } else {
          curl_easy_setopt(fl->easy, CURLOPT_POST, 1L);
          curl_easy_setopt(fl->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                           static_cast<curl_off_t>(fl->upload_buf.size()));
          curl_easy_setopt(fl->easy, CURLOPT_POSTFIELDS, fl->upload_buf.data());
        }
      } else {
        curl_easy_setopt(fl->easy, CURLOPT_CUSTOMREQUEST, m.c_str());
        if (fl->body_source || !fl->upload_buf.empty()) {
          curl_easy_setopt(fl->easy, CURLOPT_UPLOAD, 1L);
          curl_easy_setopt(fl->easy, CURLOPT_READFUNCTION, read_cb);
          curl_easy_setopt(fl->easy, CURLOPT_READDATA, fl);
          if (req.body_size_hint.has_value()) {
            curl_easy_setopt(fl->easy, CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(*req.body_size_hint));
          } else if (!fl->body_source) {
            curl_easy_setopt(fl->easy, CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(fl->upload_buf.size()));
          }
        }
      }

      for (auto& [k, v] : req.headers) {
        std::string line = k + ": " + v;
        fl->slist = curl_slist_append(fl->slist, line.c_str());
      }
      if (fl->slist)
        curl_easy_setopt(fl->easy, CURLOPT_HTTPHEADER, fl->slist);

      const CURLMcode add_rc = curl_multi_add_handle(multi, fl->easy);
      if (add_rc != CURLM_OK) {
        fxe::runtime::uv_loop_runtime::instance().report_error(
            std::string("http_client curl_multi_add_handle failed: ") +
            curl_multi_strerror(add_rc));
        auto fail_cb = std::move(fl->cb);
        cleanup_in_flight(fl);
        reject_request(std::move(fail_cb), http_error::unknown, "curl_multi_add_handle failed");
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(in_flight_mu);
        by_id[id] = fl;
        by_easy[fl->easy] = fl;
      }
      note_active(fl->origin);
      return true;
    }

    void admit_pending() {
      while (active_total < kMaxActiveTotal && !pending_fifo.empty()) {
        bool admitted_any = false;
        const usize batch = pending_fifo.size();
        for (usize i = 0; i < batch && active_total < kMaxActiveTotal; ++i) {
          auto queued = pending_fifo.front();
          pending_fifo.pop_front();
          if (!queued || queued->settled || queued->aborted ||
              pending_by_id.find(queued->id) == pending_by_id.end())
            continue;
          if (!can_start(queued->origin)) {
            pending_fifo.push_back(std::move(queued));
            continue;
          }

          pending_by_id.erase(queued->id);
          if (pending_total > 0)
            --pending_total;
          queued->settled = true;
          auto req = std::move(queued->req);
          auto cb = std::move(queued->cb);
          auto origin = std::move(queued->origin);
          (void)start_request(queued->id, std::move(req), std::move(cb), std::move(origin));
          admitted_any = true;
        }
        if (!admitted_any)
          break;
      }
    }

    void finish_active(in_flight* fl, CURLcode result) {
      long status = 0;
      curl_easy_getinfo(fl->easy, CURLINFO_RESPONSE_CODE, &status);
      char* eff_url = nullptr;
      curl_easy_getinfo(fl->easy, CURLINFO_EFFECTIVE_URL, &eff_url);
      fl->resp.status = status;
      fl->resp.body = std::move(fl->body_buf);
      if (eff_url)
        fl->resp.final_url = eff_url;
      if (result != CURLE_OK) {
        set_http_error(fl->resp, http_error_from_curl(result), curl_error_message(result, *fl));
      }
      const std::string cookie_url =
          !fl->resp.final_url.empty() ? fl->resp.final_url : fl->request_url;
      if (result == CURLE_OK) {
        for (const auto& header : fl->set_cookie_headers)
          (void)jar.set_from_header(header, cookie_url);
      }

      auto cb = std::move(fl->cb);
      auto resp = std::move(fl->resp);
      const http_request_id id = fl->id;
      const std::string origin = fl->origin;
      {
        std::lock_guard<std::mutex> lock(in_flight_mu);
        by_easy.erase(fl->easy);
        by_id.erase(id);
      }
      release_active(origin);
      cleanup_in_flight(fl);
      if (cb)
        cb(std::move(resp));
      admit_pending();
    }

    void poll_once() {
      if (!ok)
        return;
      int still_running = 0;
      curl_multi_perform(multi, &still_running);

      int msgs = 0;
      while (CURLMsg* m = curl_multi_info_read(multi, &msgs)) {
        if (m->msg != CURLMSG_DONE)
          continue;
        auto it = by_easy.find(m->easy_handle);
        if (it == by_easy.end()) {
          curl_multi_remove_handle(multi, m->easy_handle);
          curl_easy_cleanup(m->easy_handle);
          continue;
        }
        finish_active(it->second, m->data.result);
      }
    }

    void resume_upload(http_request_id id) {
      if (!ok)
        return;
      CURL* easy = nullptr;
      {
        std::lock_guard<std::mutex> lock(in_flight_mu);
        auto it = by_id.find(id);
        if (it == by_id.end() || !it->second || !it->second->easy)
          return;
        easy = it->second->easy;
      }
      (void)curl_easy_pause(easy, CURLPAUSE_CONT);
    }
  };

  http_client::http_client() : cookies_(), p_(new impl(cookies_)) {}
  http_client::~http_client() {
    delete p_;
  }

  bool http_client::available() {
    return instance().p_->ok;
  }

  http_client& http_client::instance() {
    static http_client c;
    return c;
  }

  cookie_jar& http_client::cookies() {
    return cookies_;
  }

  void http_client::set_cookie_file_path(std::string path) {
    cookies_.persist(std::move(path));
  }

  http_request_id http_client::submit(http_request req, http_callback cb) {
    auto id = p_->next_id.fetch_add(1);
    if (!p_->ok) {
      http_response r;
      if (is_https_url(req.url)) {
        set_http_error(
            r, http_error::no_backend,
            "fetch unavailable: HTTPS requires libcurl built with TLS and https protocol support");
      } else {
        set_http_error(r, http_error::no_backend, "fetch unavailable: libcurl not linked");
      }
      if (cb)
        cb(std::move(r));
      return id;
    }
    if (is_https_url(req.url) && !p_->supports_https) {
      http_response r;
      set_http_error(
          r, http_error::tls,
          "fetch unavailable: HTTPS requires libcurl built with TLS and https protocol support");
      if (cb)
        cb(std::move(r));
      return id;
    }

    const std::string origin = normalized_origin_key(req.url).key;
    if (p_->can_start(origin)) {
      (void)p_->start_request(id, std::move(req), std::move(cb), origin);
      return id;
    }

    if (p_->pending_total >= kMaxPendingTotal) {
      http_response r;
      set_http_error(r, http_error::unknown, "NetworkError: request queue full");
      if (cb)
        cb(std::move(r));
      return id;
    }

    auto queued = std::make_shared<queued_request>();
    queued->id = id;
    queued->req = std::move(req);
    queued->cb = std::move(cb);
    queued->origin = origin;
    p_->pending_by_id[id] = queued;
    p_->pending_fifo.push_back(std::move(queued));
    ++p_->pending_total;
    return id;
  }

  void http_client::abort(http_request_id id) {
    auto pending = p_->pending_by_id.find(id);
    if (pending != p_->pending_by_id.end()) {
      auto queued = pending->second;
      p_->pending_by_id.erase(pending);
      if (p_->pending_total > 0)
        --p_->pending_total;
      if (!queued->settled) {
        queued->aborted = true;
        queued->settled = true;
        http_response r;
        set_http_error(r, http_error::abort, "aborted");
        auto cb = std::move(queued->cb);
        if (cb)
          cb(std::move(r));
      }
      return;
    }

    in_flight* fl = nullptr;
    {
      std::lock_guard<std::mutex> lock(p_->in_flight_mu);
      auto it = p_->by_id.find(id);
      if (it == p_->by_id.end())
        return;
      fl = it->second;
      p_->by_easy.erase(fl->easy);
      p_->by_id.erase(it);
    }
    p_->release_active(fl->origin);
    set_http_error(fl->resp, http_error::abort, "aborted");
    auto cb = std::move(fl->cb);
    auto resp = std::move(fl->resp);
    p_->cleanup_in_flight(fl);
    if (cb)
      cb(std::move(resp));
    p_->admit_pending();
  }

  void http_client::resume_upload(http_request_id id) {
    p_->resume_upload(id);
  }

  void http_client::poll() {
    p_->poll_once();
  }
#else // !FXE_HAS_CURL

  namespace {
    bool is_https_url_without_curl(const std::string& url) {
      constexpr char k_https[] = "https://";
      if (url.size() < sizeof(k_https) - 1)
        return false;
      for (usize i = 0; i < sizeof(k_https) - 1; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        if (c != k_https[i])
          return false;
      }
      return true;
    }

#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    struct native_in_flight {
      http_request_id id = 0;
      std::unique_ptr<fxe::runtime::native_https_request_handle> handle;
      http_callback cb;
    };
#endif
  } // namespace

  struct http_client::impl {
    std::atomic<http_request_id> next_id{1};
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    std::mutex mu;
    std::unordered_map<http_request_id, std::unique_ptr<native_in_flight>> by_id;
    cookie_jar& jar;

    explicit impl(cookie_jar& cookies) : jar(cookies) {}
#else
    explicit impl(cookie_jar&) {}
#endif
  };

  http_client::http_client() : cookies_(), p_(new impl(cookies_)) {}
  http_client::~http_client() {
    delete p_;
  }
  bool http_client::available() {
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    return true;
#else
    return false;
#endif
  }
  http_client& http_client::instance() {
    static http_client c;
    return c;
  }

  cookie_jar& http_client::cookies() {
    return cookies_;
  }

  void http_client::set_cookie_file_path(std::string path) {
    cookies_.persist(std::move(path));
  }

  http_request_id http_client::submit(http_request req, http_callback cb) {
    const auto id = p_->next_id.fetch_add(1);
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    if (is_https_url_without_curl(req.url)) {
      auto in_flight = std::make_unique<native_in_flight>();
      in_flight->id = id;
      in_flight->cb = std::move(cb);
      in_flight->handle = fxe::runtime::start_native_https_request(std::move(req), &cookies_);
      std::lock_guard<std::mutex> lock(p_->mu);
      p_->by_id[id] = std::move(in_flight);
      return id;
    }
#endif

    http_response r;
    if (is_https_url_without_curl(req.url)) {
      set_http_error(r, http_error::no_backend,
                     "fetch unavailable: HTTPS requires native TLS support in this build");
    } else {
      set_http_error(r, http_error::no_backend, "fetch unavailable: libcurl not linked");
    }
    if (cb)
      cb(std::move(r));
    return id;
  }

  void http_client::abort(http_request_id id) {
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    std::lock_guard<std::mutex> lock(p_->mu);
    auto it = p_->by_id.find(id);
    if (it == p_->by_id.end() || !it->second || !it->second->handle)
      return;
    it->second->handle->abort();
#else
    (void)id;
#endif
  }

  void http_client::resume_upload(http_request_id id) {
    (void)id;
  }

  void http_client::poll() {
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    std::vector<std::pair<http_callback, http_response>> completed;
    {
      std::lock_guard<std::mutex> lock(p_->mu);
      for (auto it = p_->by_id.begin(); it != p_->by_id.end();) {
        http_response response;
        if (!it->second || !it->second->handle || !it->second->handle->poll(response)) {
          ++it;
          continue;
        }
        completed.emplace_back(std::move(it->second->cb), std::move(response));
        it = p_->by_id.erase(it);
      }
    }
    for (auto& [cb, response] : completed) {
      if (cb)
        cb(std::move(response));
    }
#endif
  }

#endif

} // namespace fxe::net
