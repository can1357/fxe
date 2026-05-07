#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fxe::net {

  enum class cookie_same_site {
    strict,
    lax,
    none,
  };

  [[nodiscard]] const char* cookie_same_site_name(cookie_same_site value) noexcept;
  [[nodiscard]] cookie_same_site cookie_same_site_from_string(std::string_view value) noexcept;

  struct cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    std::int64_t expires = 0; // Unix seconds; 0 = in-memory session cookie.
    bool secure = false;
    bool http_only = false;
    bool host_only = true;
    cookie_same_site same_site = cookie_same_site::lax;
  };

  struct cookie_filter {
    std::string name;
    std::string domain;
    std::string url;
  };

  class cookie_jar {
  public:
    cookie_jar() = default;
    explicit cookie_jar(std::string persist_path);

    [[nodiscard]] static std::optional<cookie> parse_set_cookie(std::string_view header,
                                                                std::string_view url = {});

    bool set(cookie c);
    bool set_from_header(std::string_view header, std::string_view url);
    void set(std::string domain, std::string name, std::string value, std::string path = "/",
             std::int64_t expires = 0, bool secure = false, bool http_only = false);
    [[nodiscard]] std::string pick_for_request(std::string_view url) const;
    [[nodiscard]] std::string get(std::string_view url) const {
      return pick_for_request(url);
    }
    [[nodiscard]] std::vector<cookie> get_all(cookie_filter filter = {}) const;
    bool remove(std::string_view name, std::string_view url);
    void clear();

    void persist(std::string path);
    void set_path(std::string path) {
      persist(std::move(path));
    }
    void import_netscape_lines(const std::vector<std::string>& lines);
    [[nodiscard]] const std::string& persist_path() const noexcept {
      return persist_path_;
    }
    [[nodiscard]] const std::string& path() const noexcept {
      return persist_path_;
    }

  private:
    std::string persist_path_;
    std::vector<cookie> cookies_;

    void purge_expired_locked(std::int64_t now);
    bool set_locked(cookie c, bool schedule_save);
    void schedule_save_locked();
    void save_now_snapshot(std::vector<cookie> snapshot, std::string path) const;
    void load_from_disk();
  };

} // namespace fxe::net
