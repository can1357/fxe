#include "http_client.hpp"
#include "runtime/uv_loop.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    std::string ascii_lower_copy(std::string s) {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    parsed_url parse_url_for_cookies(const std::string& url) {
      parsed_url out;
      auto scheme_end = url.find("://");
      std::size_t authority = 0;
      if (scheme_end != std::string::npos) {
        out.scheme = ascii_lower_copy(url.substr(0, scheme_end));
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
      out.host = ascii_lower_copy(out.host);
      return out;
    }

  } // namespace

  void multipart_form::add_field(std::string name, std::string value) {
    parts_.push_back(part{.name = std::move(name), .value = std::move(value), .file = false});
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
    struct in_flight {
      http_request_id id;
      CURL* easy = nullptr;
      curl_slist* slist = nullptr;
      http_callback cb;
      http_response resp;
      std::string body_buf;
      std::string upload_buf;
      std::string request_url;
      std::vector<std::string> set_cookie_headers;
      std::size_t upload_off = 0;
      bool aborted = false;
      char error_buf[CURL_ERROR_SIZE] = {};
      curl_mime* mime = nullptr;
      std::string proxy_url;
      std::string proxy_userpwd;
    };

    static std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* user) {
      auto* fl = static_cast<in_flight*>(user);
      const std::size_t n = size * nmemb;
      fl->body_buf.append(ptr, n);
      return n;
    }

    static std::size_t header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* user) {
      auto* fl = static_cast<in_flight*>(user);
      const std::size_t n = size * nmemb;
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
      std::size_t i = 0;
      while (i < val.size() && (val[i] == ' ' || val[i] == '\t'))
        ++i;
      val.erase(0, i);
      if (ascii_lower_copy(name) == "set-cookie")
        fl->set_cookie_headers.push_back(val);
      fl->resp.headers.emplace_back(std::move(name), std::move(val));
      return n;
    }

    static std::size_t read_cb(char* ptr, std::size_t size, std::size_t nmemb, void* user) {
      auto* fl = static_cast<in_flight*>(user);
      const std::size_t avail = fl->upload_buf.size() - fl->upload_off;
      const std::size_t want = size * nmemb;
      const std::size_t n = avail < want ? avail : want;
      if (n)
        std::memcpy(ptr, fl->upload_buf.data() + fl->upload_off, n);
      fl->upload_off += n;
      return n;
    }

    bool header_exists(const header_list& headers, const std::string& lower_name) {
      for (const auto& [k, _] : headers) {
        if (ascii_lower_copy(k) == lower_name)
          return true;
      }
      return false;
    }

    std::string trim_copy(std::string s) {
      auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
      while (!s.empty() && is_ws(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
      while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))
        s.pop_back();
      return s;
    }

    bool no_proxy_matches(const std::string& no_proxy, const std::string& url) {
      if (no_proxy.empty())
        return false;
      const auto parsed = parse_url_for_cookies(url);
      std::stringstream ss(no_proxy);
      std::string token;
      while (std::getline(ss, token, ',')) {
        token = ascii_lower_copy(trim_copy(std::move(token)));
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
      const auto lower = ascii_lower_copy(proxy);
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
          curl_mime_data(part, p.bytes.data(), p.bytes.size());
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
      for (std::size_t i = 0; i < sizeof(k_https) - 1; ++i) {
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
    std::unordered_map<http_request_id, in_flight*> by_id;
    std::unordered_map<CURL*, in_flight*> by_easy;
    cookie_jar& jar;

    std::size_t pump_callback_id = 0;
    explicit impl(cookie_jar& cookie_store) : jar(cookie_store) {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
        multi = curl_multi_init();
        ok = (multi != nullptr);
        supports_https = curl_supports_https();
      }
      if (ok) {
        pump_callback_id = fxe::runtime::uv_loop_runtime::instance().register_pump_callback(
            [this] { poll_once(); });
      }
    }
    ~impl() {
      fxe::runtime::uv_loop_runtime::instance().unregister_pump_callback(pump_callback_id);
      for (auto& [_, fl] : by_id) {
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
      if (multi)
        curl_multi_cleanup(multi);
      curl_global_cleanup();
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
        in_flight* fl = it->second;
        long status = 0;
        curl_easy_getinfo(fl->easy, CURLINFO_RESPONSE_CODE, &status);
        char* eff_url = nullptr;
        curl_easy_getinfo(fl->easy, CURLINFO_EFFECTIVE_URL, &eff_url);
        fl->resp.status = status;
        fl->resp.body = std::move(fl->body_buf);
        if (eff_url)
          fl->resp.final_url = eff_url;
        if (m->data.result != CURLE_OK) {
          set_http_error(fl->resp, http_error_from_curl(m->data.result),
                         curl_error_message(m->data.result, *fl));
        }
        const std::string cookie_url =
            !fl->resp.final_url.empty() ? fl->resp.final_url : fl->request_url;
        if (m->data.result == CURLE_OK) {
          for (const auto& header : fl->set_cookie_headers)
            (void)jar.set_from_header(header, cookie_url);
        }
        curl_multi_remove_handle(multi, fl->easy);
        if (fl->mime)
          curl_mime_free(fl->mime);
        curl_easy_cleanup(fl->easy);
        if (fl->slist)
          curl_slist_free_all(fl->slist);
        auto cb = std::move(fl->cb);
        auto resp = std::move(fl->resp);
        by_id.erase(fl->id);
        by_easy.erase(it);
        delete fl;
        if (cb)
          cb(std::move(resp));
      }
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
    auto* fl = new in_flight();
    fl->id = id;
    fl->cb = std::move(cb);
    fl->request_url = req.url;
    fl->upload_buf = std::move(req.body);
    fl->easy = curl_easy_init();
    if (!fl->easy) {
      http_response r;
      set_http_error(r, http_error::unknown, "curl_easy_init failed");
      if (fl->cb)
        fl->cb(std::move(r));
      delete fl;
      return id;
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
    configure_proxy(fl->easy, *fl, req);
    if (req.timeout_ms > 0) {
      const long timeout_ms = static_cast<long>(req.timeout_ms);
      curl_easy_setopt(fl->easy, CURLOPT_TIMEOUT_MS, timeout_ms);
      curl_easy_setopt(fl->easy, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    }
    if (!req.range.empty() && !header_exists(req.headers, "range"))
      req.headers.emplace_back("Range", req.range);
    const std::string jar_cookie = cookies_.pick_for_request(req.url);
    if (!jar_cookie.empty()) {
      bool merged_cookie = false;
      for (auto& [k, v] : req.headers) {
        if (ascii_lower_copy(k) == "cookie") {
          v = v.empty() ? jar_cookie : jar_cookie + "; " + v;
          merged_cookie = true;
          break;
        }
      }
      if (!merged_cookie)
        req.headers.emplace_back("Cookie", jar_cookie);
    }

    // Method handling
    const std::string& m = req.method;
    if (!req.multipart.empty()) {
      if (!configure_multipart(fl->easy, *fl, req.multipart)) {
        http_response r;
        set_http_error(r, http_error::unknown, "curl_mime_init failed");
        if (fl->cb)
          fl->cb(std::move(r));
        if (fl->mime)
          curl_mime_free(fl->mime);
        curl_easy_cleanup(fl->easy);
        delete fl;
        return id;
      }
      if (!m.empty() && m != "POST")
        curl_easy_setopt(fl->easy, CURLOPT_CUSTOMREQUEST, m.c_str());
    } else if (m == "GET" || m == "") {
      curl_easy_setopt(fl->easy, CURLOPT_HTTPGET, 1L);
    } else if (m == "HEAD") {
      curl_easy_setopt(fl->easy, CURLOPT_NOBODY, 1L);
    } else if (m == "POST") {
      curl_easy_setopt(fl->easy, CURLOPT_POST, 1L);
      curl_easy_setopt(fl->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(fl->upload_buf.size()));
      curl_easy_setopt(fl->easy, CURLOPT_POSTFIELDS, fl->upload_buf.data());
    } else {
      curl_easy_setopt(fl->easy, CURLOPT_CUSTOMREQUEST, m.c_str());
      if (!fl->upload_buf.empty()) {
        curl_easy_setopt(fl->easy, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(fl->easy, CURLOPT_READFUNCTION, read_cb);
        curl_easy_setopt(fl->easy, CURLOPT_READDATA, fl);
        curl_easy_setopt(fl->easy, CURLOPT_INFILESIZE_LARGE,
                         static_cast<curl_off_t>(fl->upload_buf.size()));
      }
    }

    for (auto& [k, v] : req.headers) {
      std::string line = k + ": " + v;
      fl->slist = curl_slist_append(fl->slist, line.c_str());
    }
    if (fl->slist)
      curl_easy_setopt(fl->easy, CURLOPT_HTTPHEADER, fl->slist);

    p_->by_id[id] = fl;
    p_->by_easy[fl->easy] = fl;
    curl_multi_add_handle(p_->multi, fl->easy);
    return id;
  }

  void http_client::abort(http_request_id id) {
    auto it = p_->by_id.find(id);
    if (it == p_->by_id.end())
      return;
    it->second->aborted = true;
    // Mark as failed during the next poll.
    curl_multi_remove_handle(p_->multi, it->second->easy);
    in_flight* fl = it->second;
    p_->by_id.erase(it);
    p_->by_easy.erase(fl->easy);
    set_http_error(fl->resp, http_error::abort, "aborted");
    auto cb = std::move(fl->cb);
    auto resp = std::move(fl->resp);
    if (fl->mime)
      curl_mime_free(fl->mime);
    curl_easy_cleanup(fl->easy);
    if (fl->slist)
      curl_slist_free_all(fl->slist);
    delete fl;
    if (cb)
      cb(std::move(resp));
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
      for (std::size_t i = 0; i < sizeof(k_https) - 1; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        if (c != k_https[i])
          return false;
      }
      return true;
    }
  } // namespace

  struct http_client::impl {};
  http_client::http_client() : cookies_(), p_(nullptr) {}
  http_client::~http_client() {
    (void)p_;
  }
  bool http_client::available() {
    return false;
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
    http_response r;
    if (is_https_url_without_curl(req.url)) {
      set_http_error(
          r, http_error::no_backend,
          "fetch unavailable: HTTPS requires libcurl built with TLS and https protocol support");
    } else {
      set_http_error(r, http_error::no_backend, "fetch unavailable: libcurl not linked");
    }
    if (cb)
      cb(std::move(r));
    return 0;
  }
  void http_client::abort(http_request_id) {}
  void http_client::poll() {}

#endif

} // namespace fxe::net
