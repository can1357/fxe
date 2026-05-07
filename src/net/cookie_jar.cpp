#include "cookie_jar.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace fxe::net {
  namespace {
    std::mutex& jar_mutex() {
      static std::mutex mutex;
      return mutex;
    }

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

    bool ascii_ieq(std::string_view a, std::string_view b) {
      if (a.size() != b.size())
        return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
          return false;
      }
      return true;
    }

    std::string trim_copy(std::string_view in) {
      auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
      while (!in.empty() && is_ws(static_cast<unsigned char>(in.front())))
        in.remove_prefix(1);
      while (!in.empty() && is_ws(static_cast<unsigned char>(in.back())))
        in.remove_suffix(1);
      return std::string(in);
    }

    parsed_url parse_url(std::string_view url) {
      parsed_url out;
      auto scheme_end = url.find("://");
      std::size_t authority = 0;
      if (scheme_end != std::string_view::npos) {
        out.scheme = ascii_lower_copy(std::string(url.substr(0, scheme_end)));
        authority = scheme_end + 3;
      }
      auto path_pos = url.find('/', authority);
      auto query_pos = url.find('?', authority);
      auto frag_pos = url.find('#', authority);
      std::size_t path_start = path_pos;
      if (path_start == std::string_view::npos ||
          (query_pos != std::string_view::npos && query_pos < path_start) ||
          (frag_pos != std::string_view::npos && frag_pos < path_start)) {
        path_start = std::min(query_pos == std::string_view::npos ? url.size() : query_pos,
                              frag_pos == std::string_view::npos ? url.size() : frag_pos);
      }
      std::string host_port(url.substr(authority, path_start - authority));
      if (path_pos != std::string_view::npos) {
        std::size_t path_end =
            std::min(query_pos == std::string_view::npos ? url.size() : query_pos,
                     frag_pos == std::string_view::npos ? url.size() : frag_pos);
        out.path = std::string(url.substr(path_pos, path_end - path_pos));
        if (out.path.empty())
          out.path = "/";
      }
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

    std::string default_cookie_path(const std::string& request_path) {
      if (request_path.empty() || request_path.front() != '/')
        return "/";
      auto last = request_path.rfind('/');
      if (last == 0 || last == std::string::npos)
        return "/";
      return request_path.substr(0, last);
    }

    std::int64_t now_unix_seconds() {
      using namespace std::chrono;
      return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    bool parse_int64(std::string_view value, std::int64_t& out) {
      try {
        std::size_t consumed = 0;
        auto parsed = std::stoll(std::string(trim_copy(value)), &consumed, 10);
        if (consumed == trim_copy(value).size()) {
          out = parsed;
          return true;
        }
      } catch (...) {
      }
      return false;
    }

    std::int64_t portable_timegm(std::tm* tm) {
#if defined(_WIN32)
      return static_cast<std::int64_t>(_mkgmtime(tm));
#else
      return static_cast<std::int64_t>(timegm(tm));
#endif
    }

    bool parse_http_date(std::string_view value, std::int64_t& out) {
      std::string s = trim_copy(value);
      static constexpr std::array<const char*, 3> formats{
          "%a, %d %b %Y %H:%M:%S GMT",
          "%A, %d-%b-%y %H:%M:%S GMT",
          "%a %b %d %H:%M:%S %Y",
      };
      for (const char* format : formats) {
        std::tm tm{};
        std::istringstream in(s);
        in.imbue(std::locale::classic());
        in >> std::get_time(&tm, format);
        if (!in.fail()) {
          out = portable_timegm(&tm);
          return true;
        }
      }
      return parse_int64(s, out);
    }

    std::string normalized_domain(std::string domain) {
      domain = ascii_lower_copy(trim_copy(domain));
      if (domain.rfind("#httponly_", 0) == 0)
        domain.erase(0, 10);
      while (!domain.empty() && domain.front() == '.')
        domain.erase(domain.begin());
      if (!domain.empty() && domain.back() == '.')
        domain.pop_back();
      return domain;
    }

    bool domain_matches(std::string_view cookie_domain, bool host_only, std::string_view host) {
      if (cookie_domain.empty() || host.empty())
        return false;
      if (host_only)
        return host == cookie_domain;
      if (host == cookie_domain)
        return true;
      return host.size() > cookie_domain.size() &&
             host.compare(host.size() - cookie_domain.size(), cookie_domain.size(),
                          cookie_domain) == 0 &&
             host[host.size() - cookie_domain.size() - 1] == '.';
    }

    bool path_matches(std::string_view cookie_path, std::string_view request_path) {
      if (cookie_path.empty())
        cookie_path = "/";
      if (request_path.empty())
        request_path = "/";
      if (request_path == cookie_path)
        return true;
      if (request_path.size() < cookie_path.size() ||
          request_path.compare(0, cookie_path.size(), cookie_path) != 0)
        return false;
      return cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
    }

    bool parse_netscape_cookie_line(std::string line, cookie& out) {
      out = cookie{};
      if (line.rfind("#HttpOnly_", 0) == 0) {
        out.http_only = true;
        line.erase(0, 10);
      } else if (line.empty() || line.front() == '#') {
        return false;
      }
      std::vector<std::string> fields;
      std::stringstream ss(line);
      std::string field;
      while (std::getline(ss, field, '\t'))
        fields.push_back(field);
      if (fields.size() < 7)
        return false;
      out.domain = normalized_domain(fields[0]);
      out.host_only = fields[1] != "TRUE";
      out.path = fields[2].empty() ? "/" : fields[2];
      out.secure = fields[3] == "TRUE";
      parse_int64(fields[4], out.expires);
      out.name = fields[5];
      out.value = fields[6];
      return !out.domain.empty() && !out.name.empty();
    }
  } // namespace

  const char* cookie_same_site_name(cookie_same_site value) noexcept {
    switch (value) {
    case cookie_same_site::strict:
      return "Strict";
    case cookie_same_site::lax:
      return "Lax";
    case cookie_same_site::none:
      return "None";
    }
    return "Lax";
  }

  cookie_same_site cookie_same_site_from_string(std::string_view value) noexcept {
    if (ascii_ieq(value, "strict"))
      return cookie_same_site::strict;
    if (ascii_ieq(value, "none") || ascii_ieq(value, "no_restriction"))
      return cookie_same_site::none;
    return cookie_same_site::lax;
  }

  cookie_jar::cookie_jar(std::string persist_path) : persist_path_(std::move(persist_path)) {
    load_from_disk();
  }

  std::optional<cookie> cookie_jar::parse_set_cookie(std::string_view header,
                                                     std::string_view url) {
    auto parsed = parse_url(url);
    std::string_view rest = header;
    auto semi = rest.find(';');
    std::string_view pair = semi == std::string_view::npos ? rest : rest.substr(0, semi);
    rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);

    auto eq = pair.find('=');
    if (eq == std::string_view::npos)
      return std::nullopt;
    cookie out;
    out.name = trim_copy(pair.substr(0, eq));
    out.value = trim_copy(pair.substr(eq + 1));
    out.domain = parsed.host;
    out.path = default_cookie_path(parsed.path);
    if (out.name.empty())
      return std::nullopt;

    bool saw_max_age = false;
    while (!rest.empty()) {
      auto next = rest.find(';');
      std::string attr = trim_copy(next == std::string_view::npos ? rest : rest.substr(0, next));
      rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
      if (attr.empty())
        continue;
      auto attr_eq = attr.find('=');
      std::string key = ascii_lower_copy(
          trim_copy(attr_eq == std::string::npos ? std::string_view(attr)
                                                 : std::string_view(attr).substr(0, attr_eq)));
      std::string val = attr_eq == std::string::npos
                            ? std::string{}
                            : trim_copy(std::string_view(attr).substr(attr_eq + 1));
      if (key == "domain") {
        auto domain = normalized_domain(std::move(val));
        if (domain.empty())
          return std::nullopt;
        if (!parsed.host.empty() && !domain_matches(domain, false, parsed.host))
          return std::nullopt;
        out.domain = std::move(domain);
        out.host_only = false;
      } else if (key == "path") {
        out.path = !val.empty() && val.front() == '/' ? std::move(val) : "/";
      } else if (key == "expires") {
        std::int64_t expires = 0;
        if (!saw_max_age && parse_http_date(val, expires))
          out.expires = expires;
      } else if (key == "max-age") {
        std::int64_t delta = 0;
        if (parse_int64(val, delta)) {
          saw_max_age = true;
          out.expires = delta <= 0 ? 1 : now_unix_seconds() + delta;
        }
      } else if (key == "secure") {
        out.secure = true;
      } else if (key == "httponly") {
        out.http_only = true;
      } else if (key == "samesite") {
        out.same_site = cookie_same_site_from_string(val);
      }
    }

    if (out.domain.empty())
      return std::nullopt;
    if (out.same_site == cookie_same_site::none && !out.secure)
      return std::nullopt;
    return out;
  }

  bool cookie_jar::set(cookie c) {
    std::lock_guard<std::mutex> lock(jar_mutex());
    return set_locked(std::move(c), true);
  }

  bool cookie_jar::set_from_header(std::string_view header, std::string_view url) {
    auto parsed = parse_set_cookie(header, url);
    if (!parsed)
      return false;
    return set(std::move(*parsed));
  }

  void cookie_jar::set(std::string domain, std::string name, std::string value, std::string path,
                       std::int64_t expires, bool secure, bool http_only) {
    cookie c;
    c.domain = std::move(domain);
    c.name = std::move(name);
    c.value = std::move(value);
    c.path = std::move(path);
    c.expires = expires;
    c.secure = secure;
    c.http_only = http_only;
    c.host_only = c.domain.empty() || c.domain.front() != '.';
    (void)set(std::move(c));
  }

  bool cookie_jar::set_locked(cookie c, bool schedule_save) {
    c.name = trim_copy(c.name);
    c.domain = normalized_domain(std::move(c.domain));
    if (c.path.empty() || c.path.front() != '/')
      c.path = "/";
    if (c.name.empty() || c.domain.empty())
      return false;
    if (c.same_site == cookie_same_site::none && !c.secure)
      return false;

    auto now = now_unix_seconds();
    purge_expired_locked(now);
    auto matches = [&](const cookie& existing) {
      return existing.name == c.name && existing.domain == c.domain && existing.path == c.path;
    };

    if (c.expires > 0 && c.expires <= now) {
      const auto old_size = cookies_.size();
      cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(), matches), cookies_.end());
      if (schedule_save && cookies_.size() != old_size)
        schedule_save_locked();
      return true;
    }

    auto it = std::find_if(cookies_.begin(), cookies_.end(), matches);
    if (it == cookies_.end())
      cookies_.push_back(std::move(c));
    else
      *it = std::move(c);
    if (schedule_save)
      schedule_save_locked();
    return true;
  }

  std::string cookie_jar::pick_for_request(std::string_view url) const {
    const auto parsed = parse_url(url);
    const bool https = parsed.scheme == "https";
    const auto now = now_unix_seconds();
    std::vector<cookie> matches;
    {
      std::lock_guard<std::mutex> lock(jar_mutex());
      for (const auto& c : cookies_) {
        if (c.expires > 0 && c.expires <= now)
          continue;
        if (c.secure && !https)
          continue;
        if (!domain_matches(c.domain, c.host_only, parsed.host))
          continue;
        if (!path_matches(c.path, parsed.path))
          continue;
        matches.push_back(c);
      }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const cookie& a, const cookie& b) {
      return a.path.size() > b.path.size();
    });
    std::string out;
    for (const auto& c : matches) {
      if (!out.empty())
        out += "; ";
      out += c.name;
      out += '=';
      out += c.value;
    }
    return out;
  }

  std::vector<cookie> cookie_jar::get_all(cookie_filter filter) const {
    const auto parsed = filter.url.empty() ? parsed_url{} : parse_url(filter.url);
    const auto now = now_unix_seconds();
    std::vector<cookie> out;
    std::lock_guard<std::mutex> lock(jar_mutex());
    for (const auto& c : cookies_) {
      if (c.expires > 0 && c.expires <= now)
        continue;
      if (!filter.name.empty() && c.name != filter.name)
        continue;
      if (!filter.domain.empty() && c.domain != normalized_domain(filter.domain))
        continue;
      if (!filter.url.empty() && (!domain_matches(c.domain, c.host_only, parsed.host) ||
                                  !path_matches(c.path, parsed.path)))
        continue;
      out.push_back(c);
    }
    return out;
  }

  bool cookie_jar::remove(std::string_view name, std::string_view url) {
    const auto parsed = parse_url(url);
    bool removed = false;
    std::lock_guard<std::mutex> lock(jar_mutex());
    auto it = cookies_.begin();
    while (it != cookies_.end()) {
      if (it->name == name && domain_matches(it->domain, it->host_only, parsed.host) &&
          path_matches(it->path, parsed.path)) {
        it = cookies_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    if (removed)
      schedule_save_locked();
    return removed;
  }

  void cookie_jar::clear() {
    std::lock_guard<std::mutex> lock(jar_mutex());
    cookies_.clear();
    schedule_save_locked();
  }

  void cookie_jar::persist(std::string path) {
    std::lock_guard<std::mutex> lock(jar_mutex());
    persist_path_ = std::move(path);
    cookies_.clear();
    load_from_disk();
  }

  void cookie_jar::import_netscape_lines(const std::vector<std::string>& lines) {
    std::lock_guard<std::mutex> lock(jar_mutex());
    cookies_.clear();
    for (const auto& line : lines) {
      cookie c;
      if (parse_netscape_cookie_line(line, c))
        (void)set_locked(std::move(c), false);
    }
    schedule_save_locked();
  }

  void cookie_jar::purge_expired_locked(std::int64_t now) {
    cookies_.erase(
        std::remove_if(cookies_.begin(), cookies_.end(),
                       [&](const cookie& c) { return c.expires > 0 && c.expires <= now; }),
        cookies_.end());
  }

  void cookie_jar::schedule_save_locked() {
    if (persist_path_.empty())
      return;
    save_now_snapshot(cookies_, persist_path_);
  }

  void cookie_jar::save_now_snapshot(std::vector<cookie> snapshot, std::string path) const {
    if (path.empty())
      return;
    std::ofstream out(path, std::ios::trunc);
    if (!out)
      return;
    out << "# Netscape HTTP Cookie File\n";
    for (const auto& c : snapshot) {
      std::string domain = c.domain;
      if (c.http_only)
        domain = "#HttpOnly_" + domain;
      out << domain << '\t' << (c.host_only ? "FALSE" : "TRUE") << '\t'
          << (c.path.empty() ? "/" : c.path) << '\t' << (c.secure ? "TRUE" : "FALSE") << '\t'
          << c.expires << '\t' << c.name << '\t' << c.value << '\n';
    }
  }

  void cookie_jar::load_from_disk() {
    if (persist_path_.empty())
      return;
    std::ifstream in(persist_path_);
    if (!in)
      return;
    std::string line;
    while (std::getline(in, line)) {
      cookie c;
      if (parse_netscape_cookie_line(line, c))
        (void)set_locked(std::move(c), false);
    }
  }

} // namespace fxe::net
