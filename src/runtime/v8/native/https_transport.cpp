#include "runtime/v8/native/https_transport.hpp"

#include "net/tls_client.hpp"

#include <fxe/string_utils.hpp>
#include <fxe/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fxe::runtime {
  namespace {
    using clock_type = std::chrono::steady_clock;
    constexpr int kPollSliceMs = 100;

    struct parsed_url {
      std::string scheme;
      std::string host;
      u16 port = 443;
      std::string path = "/";

      [[nodiscard]] std::string origin() const {
        return scheme + "://" + host + (port == 443 ? std::string{} : ":" + std::to_string(port));
      }
    };

    struct parsed_response {
      fxe::net::http_response response;
      std::vector<std::string> set_cookie_headers;
      bool has_content_length = false;
      usize content_length = 0;
    };

    enum class cancel_reason {
      none,
      abort,
      timeout,
    };

    struct shared_state {
      explicit shared_state(int timeout_ms_in)
          : timeout_ms(timeout_ms_in),
            deadline(timeout_ms_in > 0
                         ? clock_type::now() + std::chrono::milliseconds(timeout_ms_in)
                         : clock_type::time_point::max()) {}

      std::mutex mu;
      std::unique_ptr<fxe::net::tls_client> active_client;
      fxe::net::http_response result;
      cancel_reason cancel = cancel_reason::none;
      bool completed = false;
      bool consumed = false;
      int timeout_ms = 0;
      clock_type::time_point deadline;
    };

    bool header_exists(const fxe::net::header_list& headers, std::string_view lower_name) {
      for (const auto& [key, _] : headers) {
        if (ascii_lower(key) == lower_name)
          return true;
      }
      return false;
    }

    std::optional<std::string> header_value(const fxe::net::header_list& headers,
                                            std::string_view lower_name) {
      for (const auto& [key, value] : headers) {
        if (ascii_lower(key) == lower_name)
          return value;
      }
      return std::nullopt;
    }

    const char* http_error_name(fxe::net::http_error error) {
      switch (error) {
      case fxe::net::http_error::ok:
        return "ok";
      case fxe::net::http_error::no_backend:
        return "no_backend";
      case fxe::net::http_error::dns:
        return "dns";
      case fxe::net::http_error::connect:
        return "connect";
      case fxe::net::http_error::tls:
        return "tls";
      case fxe::net::http_error::timeout:
        return "timeout";
      case fxe::net::http_error::response_truncated:
        return "response_truncated";
      case fxe::net::http_error::abort:
        return "abort";
      case fxe::net::http_error::unknown:
        return "unknown";
      }
      return "unknown";
    }

    void set_http_error(fxe::net::http_response& response, fxe::net::http_error error,
                        std::string message) {
      response.last_error = error;
      response.error = std::move(message) + " (code: " + http_error_name(error) + ")";
    }

    bool is_https_url(std::string_view url) {
      return url.size() >= 8 && ascii_lower(std::string(url.substr(0, 8))) == "https://";
    }

    std::optional<parsed_url> parse_https_url(std::string_view url, std::string& error) {
      error.clear();
      constexpr std::string_view prefix = "https://";
      if (!url.starts_with(prefix)) {
        error = "native HTTPS transport requires an https:// URL";
        return std::nullopt;
      }
      url.remove_prefix(prefix.size());
      const auto path_start = url.find_first_of("/?#");
      const auto authority = path_start == std::string_view::npos ? url : url.substr(0, path_start);
      parsed_url out;
      out.scheme = "https";
      out.path = path_start == std::string_view::npos ? "/" : std::string(url.substr(path_start));
      if (out.path.empty())
        out.path = "/";
      if (authority.empty()) {
        error = "HTTPS URL missing host";
        return std::nullopt;
      }
      if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos) {
          error = "invalid HTTPS URL host";
          return std::nullopt;
        }
        out.host = std::string(authority.substr(1, end - 1));
        if (end + 1 < authority.size()) {
          if (authority[end + 1] != ':') {
            error = "invalid HTTPS URL host";
            return std::nullopt;
          }
          unsigned int port = 0;
          auto port_text = authority.substr(end + 2);
          auto [ptr, ec] =
              std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
          if (ec != std::errc{} || ptr != port_text.data() + port_text.size() || port > 65535) {
            error = "invalid HTTPS URL port";
            return std::nullopt;
          }
          out.port = static_cast<u16>(port);
        }
      } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
          out.host = std::string(authority.substr(0, colon));
          unsigned int port = 0;
          auto port_text = authority.substr(colon + 1);
          auto [ptr, ec] =
              std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
          if (ec != std::errc{} || ptr != port_text.data() + port_text.size() || port > 65535) {
            error = "invalid HTTPS URL port";
            return std::nullopt;
          }
          out.port = static_cast<u16>(port);
        } else {
          out.host = std::string(authority);
        }
      }
      if (out.host.empty()) {
        error = "HTTPS URL missing host";
        return std::nullopt;
      }
      return out;
    }

    std::string remove_dot_segments(std::string path) {
      std::vector<std::string> parts;
      usize start = 0;
      while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string::npos ? path.size() : slash;
        const auto part = path.substr(start, end - start);
        if (part == "..") {
          if (!parts.empty())
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
          parts.push_back(part);
        }
        if (slash == std::string::npos)
          break;
        start = slash + 1;
      }
      std::string out = "/";
      for (usize i = 0; i < parts.size(); ++i) {
        if (i != 0)
          out.push_back('/');
        out.append(parts[i]);
      }
      if (!path.empty() && path.back() == '/' && out.back() != '/')
        out.push_back('/');
      return out;
    }

    std::optional<std::string> resolve_redirect_url(const std::string& current_url,
                                                    std::string_view location, std::string& error) {
      error.clear();
      if (location.empty()) {
        error = "redirect response missing Location header";
        return std::nullopt;
      }
      if (location.starts_with("https://"))
        return std::string(location);
      if (location.starts_with("http://")) {
        error = "native HTTPS transport cannot follow redirects to http:// URLs";
        return std::nullopt;
      }
      auto current = parse_https_url(current_url, error);
      if (!current)
        return std::nullopt;
      if (location.starts_with("//"))
        return current->scheme + ":" + std::string(location);
      if (location.front() == '/')
        return current->origin() + std::string(location);
      if (location.front() == '?') {
        auto base_path = current->path;
        const auto query = base_path.find('?');
        if (query != std::string::npos)
          base_path.erase(query);
        return current->origin() + base_path + std::string(location);
      }
      std::string base_path = current->path;
      const auto query = base_path.find('?');
      if (query != std::string::npos)
        base_path.erase(query);
      const auto slash = base_path.rfind('/');
      base_path = slash == std::string::npos ? "/" : base_path.substr(0, slash + 1);
      return current->origin() + remove_dot_segments(base_path + std::string(location));
    }

    std::optional<usize> parse_content_length(const fxe::net::header_list& headers) {
      const auto value = header_value(headers, "content-length");
      if (!value)
        return std::nullopt;
      usize out = 0;
      auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), out);
      if (ec != std::errc{} || ptr != value->data() + value->size())
        return std::nullopt;
      return out;
    }

    bool request_cancelled(const std::shared_ptr<shared_state>& state,
                           cancel_reason* why = nullptr) {
      std::lock_guard<std::mutex> lock(state->mu);
      if (why)
        *why = state->cancel;
      return state->cancel != cancel_reason::none;
    }

    bool deadline_expired(const std::shared_ptr<shared_state>& state) {
      if (state->timeout_ms <= 0)
        return false;
      return clock_type::now() >= state->deadline;
    }

    void cancel_request(const std::shared_ptr<shared_state>& state, cancel_reason why) {
      std::lock_guard<std::mutex> lock(state->mu);
      if (state->completed || state->cancel != cancel_reason::none)
        return;
      state->cancel = why;
      if (state->active_client)
        state->active_client->close();
    }

    int next_read_timeout_ms(const std::shared_ptr<shared_state>& state) {
      if (state->timeout_ms <= 0)
        return kPollSliceMs;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(state->deadline - clock_type::now())
              .count();
      if (remaining <= 0)
        return 1;
      return static_cast<int>(std::min<long long>(remaining, kPollSliceMs));
    }

    fxe::net::http_response cancelled_response(cancel_reason why, std::string url) {
      fxe::net::http_response response;
      response.final_url = std::move(url);
      set_http_error(response,
                     why == cancel_reason::timeout ? fxe::net::http_error::timeout
                                                   : fxe::net::http_error::abort,
                     why == cancel_reason::timeout ? "request timed out" : "aborted");
      return response;
    }

    fxe::net::http_error classify_connect_error(const std::string& error) {
      const auto lower = ascii_lower(error);
      if (lower.find("resolve") != std::string::npos || lower.find("name") != std::string::npos)
        return fxe::net::http_error::dns;
      if (lower.find("connect") != std::string::npos || lower.find("refused") != std::string::npos)
        return fxe::net::http_error::connect;
      return fxe::net::http_error::tls;
    }

    bool write_all(fxe::net::tls_client& client, const std::string& bytes,
                   const std::shared_ptr<shared_state>& state, std::string& error) {
      usize written = 0;
      while (written < bytes.size()) {
        cancel_reason why = cancel_reason::none;
        if (request_cancelled(state, &why)) {
          error = why == cancel_reason::timeout ? "request timed out" : "aborted";
          return false;
        }
        if (deadline_expired(state)) {
          cancel_request(state, cancel_reason::timeout);
          error = "request timed out";
          return false;
        }
        const auto n = client.write(bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
          error = client.last_error().empty() ? "TLS write failed" : client.last_error();
          return false;
        }
        written += static_cast<usize>(n);
      }
      return true;
    }

    std::optional<parsed_response> parse_http_response(const std::string& raw, std::string& error) {
      const auto header_end = raw.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        error = "native HTTPS response missing headers";
        return std::nullopt;
      }
      parsed_response out;
      out.response.body.assign(raw.data() + header_end + 4, raw.size() - header_end - 4);
      std::string_view header_block(raw.data(), header_end);
      auto line_end = header_block.find("\r\n");
      auto status_line = header_block.substr(
          0, line_end == std::string_view::npos ? header_block.size() : line_end);
      if (!status_line.starts_with("HTTP/")) {
        error = "malformed HTTP response status line";
        return std::nullopt;
      }
      const auto first_space = status_line.find(' ');
      const auto second_space = first_space == std::string_view::npos
                                    ? std::string_view::npos
                                    : status_line.find(' ', first_space + 1);
      if (first_space == std::string_view::npos) {
        error = "malformed HTTP response status line";
        return std::nullopt;
      }
      auto code = status_line.substr(first_space + 1, second_space == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : second_space - first_space - 1);
      (void)std::from_chars(code.data(), code.data() + code.size(), out.response.status);
      if (second_space != std::string_view::npos)
        out.response.status_text = std::string(status_line.substr(second_space + 1));
      usize line_start = line_end == std::string_view::npos ? header_block.size() : line_end + 2;
      while (line_start < header_block.size()) {
        line_end = header_block.find("\r\n", line_start);
        if (line_end == std::string_view::npos)
          line_end = header_block.size();
        auto line = header_block.substr(line_start, line_end - line_start);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
          std::string name(trim(line.substr(0, colon)));
          std::string value(trim(line.substr(colon + 1)));
          if (!name.empty()) {
            if (ascii_lower(name) == "set-cookie")
              out.set_cookie_headers.push_back(value);
            out.response.headers.emplace_back(std::move(name), std::move(value));
          }
        }
        line_start = line_end + 2;
      }
      if (auto content_length = parse_content_length(out.response.headers)) {
        out.has_content_length = true;
        out.content_length = *content_length;
      }
      return out;
    }

    std::optional<parsed_response> read_http_response(fxe::net::tls_client& client,
                                                      const std::shared_ptr<shared_state>& state,
                                                      std::string& error) {
      std::string raw;
      std::array<char, 4096> chunk{};
      std::optional<usize> expected_total;
      for (;;) {
        cancel_reason why = cancel_reason::none;
        if (request_cancelled(state, &why)) {
          error = why == cancel_reason::timeout ? "request timed out" : "aborted";
          return std::nullopt;
        }
        if (deadline_expired(state)) {
          cancel_request(state, cancel_reason::timeout);
          error = "request timed out";
          return std::nullopt;
        }
        if (expected_total && raw.size() >= *expected_total)
          break;
        const auto n =
            client.read_with_timeout(chunk.data(), chunk.size(), next_read_timeout_ms(state));
        if (n == fxe::net::tls_client::read_timed_out)
          continue;
        if (n < 0) {
          error = client.last_error().empty() ? "TLS read failed" : client.last_error();
          return std::nullopt;
        }
        if (n == 0)
          break;
        raw.append(chunk.data(), static_cast<usize>(n));
        if (raw.size() > 32 * 1024 * 1024) {
          error = "HTTP response too large";
          return std::nullopt;
        }
        if (!expected_total) {
          std::string parse_error;
          auto parsed = parse_http_response(raw, parse_error);
          if (parsed && parsed->has_content_length)
            expected_total = raw.find("\r\n\r\n") + 4 + parsed->content_length;
        }
      }
      auto parsed = parse_http_response(raw, error);
      if (!parsed)
        return std::nullopt;
      if (parsed->has_content_length && parsed->response.body.size() < parsed->content_length) {
        error = "HTTP response body truncated";
        return std::nullopt;
      }
      if (parsed->has_content_length && parsed->response.body.size() > parsed->content_length)
        parsed->response.body.resize(parsed->content_length);
      return parsed;
    }

    bool is_redirect_status(long status) {
      return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    void prepare_redirect_request(fxe::net::http_request& request, long status) {
      if (status == 303 || ((status == 301 || status == 302) && request.method != "GET" &&
                            request.method != "HEAD")) {
        request.method = request.method == "HEAD" ? "HEAD" : "GET";
        request.body.clear();
        request.body_source = {};
        request.body_size_hint.reset();
      }
    }

    std::string build_request_head(const parsed_url& url, fxe::net::http_request& request,
                                   fxe::net::cookie_jar* jar) {
      fxe::net::header_list headers;
      headers.reserve(request.headers.size() + 6);
      std::string explicit_cookie;
      for (const auto& [key, value] : request.headers) {
        const auto lower = ascii_lower(key);
        if (lower == "cookie") {
          explicit_cookie = value;
          continue;
        }
        if (lower == "host" || lower == "connection")
          continue;
        if (lower == "content-length" && request.body.empty())
          continue;
        headers.emplace_back(key, value);
      }
      if (!request.range.empty() && !header_exists(headers, "range"))
        headers.emplace_back("Range", request.range);
      headers.emplace_back(
          "Host", url.host + (url.port == 443 ? std::string{} : ":" + std::to_string(url.port)));
      headers.emplace_back("Connection", "close");

      const auto jar_cookie = jar ? jar->pick_for_request(request.url) : std::string{};
      std::string merged_cookie = jar_cookie;
      if (!explicit_cookie.empty())
        merged_cookie =
            merged_cookie.empty() ? explicit_cookie : merged_cookie + "; " + explicit_cookie;
      if (!merged_cookie.empty())
        headers.emplace_back("Cookie", std::move(merged_cookie));
      if (!request.body.empty() && !header_exists(headers, "content-length"))
        headers.emplace_back("Content-Length", std::to_string(request.body.size()));
      if (request.body.empty() && (request.method == "POST" || request.method == "PUT") &&
          !header_exists(headers, "content-length"))
        headers.emplace_back("Content-Length", "0");

      std::ostringstream request_text;
      request_text << (request.method.empty() ? "GET" : request.method) << ' ' << url.path
                   << " HTTP/1.1\r\n";
      for (const auto& [key, value] : headers)
        request_text << key << ": " << value << "\r\n";
      request_text << "\r\n";
      return request_text.str();
    }

    fxe::net::http_response execute_request(fxe::net::http_request request,
                                            fxe::net::cookie_jar* jar,
                                            native_https_request_options options,
                                            const std::shared_ptr<shared_state>& state) {
      fxe::net::http_response response;
      if (!is_https_url(request.url)) {
        set_http_error(response, fxe::net::http_error::no_backend,
                       "native HTTPS transport requires an https:// URL");
        return response;
      }
      if (!request.proxy.empty()) {
        set_http_error(response, fxe::net::http_error::no_backend,
                       "native HTTPS transport does not support proxies");
        return response;
      }
      if (!request.multipart.empty()) {
        set_http_error(response, fxe::net::http_error::no_backend,
                       "native HTTPS transport does not support multipart/form-data");
        return response;
      }
      if (request.body_source) {
        set_http_error(response, fxe::net::http_error::no_backend,
                       "native HTTPS transport does not support streaming uploads");
        return response;
      }

      int redirects = 0;
      std::string current_url = request.url;
      while (true) {
        cancel_reason why = cancel_reason::none;
        if (request_cancelled(state, &why))
          return cancelled_response(why, current_url);
        if (deadline_expired(state)) {
          cancel_request(state, cancel_reason::timeout);
          return cancelled_response(cancel_reason::timeout, current_url);
        }

        std::string parse_error;
        auto url = parse_https_url(current_url, parse_error);
        if (!url) {
          response.final_url = current_url;
          set_http_error(response, fxe::net::http_error::unknown, parse_error);
          return response;
        }

        fxe::net::tls_options tls_options;
        tls_options.host = url->host;
        tls_options.port = url->port;
        tls_options.ca_pem = options.ca_pem;
        tls_options.ca_path = options.ca_path;
        tls_options.reject_unauthorized = options.reject_unauthorized;
        tls_options.session_namespace = options.session_namespace;
        tls_options.alpn = {"http/1.1"};

        std::string err;
        auto client = fxe::net::tls_client::connect(tls_options, err);
        if (!client) {
          if (request_cancelled(state, &why))
            return cancelled_response(why, current_url);
          response.final_url = current_url;
          set_http_error(response, classify_connect_error(err),
                         err.empty() ? "native HTTPS request connect failed" : err);
          return response;
        }
        {
          std::lock_guard<std::mutex> lock(state->mu);
          if (state->cancel != cancel_reason::none) {
            client->close();
            return cancelled_response(state->cancel, current_url);
          }
          state->active_client = std::move(client);
        }

        fxe::net::tls_client* active = nullptr;
        {
          std::lock_guard<std::mutex> lock(state->mu);
          active = state->active_client.get();
        }

        request.url = current_url;
        const auto request_head = build_request_head(*url, request, jar);
        if (!write_all(*active, request_head, state, err) ||
            (!request.body.empty() && !write_all(*active, request.body, state, err))) {
          if (request_cancelled(state, &why))
            return cancelled_response(why, current_url);
          response.final_url = current_url;
          set_http_error(response, fxe::net::http_error::tls,
                         err.empty() ? "native HTTPS request write failed" : err);
          return response;
        }

        auto parsed = read_http_response(*active, state, err);
        {
          std::lock_guard<std::mutex> lock(state->mu);
          if (state->active_client)
            state->active_client->close();
          state->active_client.reset();
        }
        if (request_cancelled(state, &why))
          return cancelled_response(why, current_url);
        if (!parsed) {
          response.final_url = current_url;
          if (err == "HTTP response body truncated") {
            set_http_error(response, fxe::net::http_error::response_truncated, err);
          } else if (err == "request timed out") {
            set_http_error(response, fxe::net::http_error::timeout, err);
          } else {
            set_http_error(response, fxe::net::http_error::tls, err);
          }
          return response;
        }

        parsed->response.final_url = current_url;
        for (const auto& header : parsed->set_cookie_headers) {
          if (jar && jar->set_from_header(header, current_url)) {
            FXE_TRACE("net.cookies", "stored Set-Cookie for url={}", current_url);
          }
        }
        if (!request.follow_redirects || !is_redirect_status(parsed->response.status))
          return std::move(parsed->response);

        const auto location = header_value(parsed->response.headers, "location");
        if (!location)
          return std::move(parsed->response);
        if (redirects >= options.max_redirects) {
          FXE_WARN("net.http", "redirect_limit_exceeded url={} redirects={}", current_url,
                   redirects);
          set_http_error(parsed->response, fxe::net::http_error::unknown, "too many redirects");
          return std::move(parsed->response);
        }
        auto next_url = resolve_redirect_url(current_url, *location, err);
        if (!next_url) {
          set_http_error(parsed->response, fxe::net::http_error::no_backend, err);
          return std::move(parsed->response);
        }
        FXE_TRACE("net.https", "following redirect status={} from={} to={}",
                  parsed->response.status, current_url, *next_url);
        prepare_redirect_request(request, parsed->response.status);
        current_url = std::move(*next_url);
        ++redirects;
      }
    }

    void store_completed(const std::shared_ptr<shared_state>& state,
                         fxe::net::http_response response) {
      std::lock_guard<std::mutex> lock(state->mu);
      if (state->completed)
        return;
      state->result = std::move(response);
      state->completed = true;
      state->active_client.reset();
    }

    class native_https_request_handle_impl final : public native_https_request_handle {
    public:
      native_https_request_handle_impl(fxe::net::http_request request, fxe::net::cookie_jar* jar,
                                       native_https_request_options options)
          : state_(std::make_shared<shared_state>(request.timeout_ms)) {
        worker_ = std::thread([state = state_, request = std::move(request), jar,
                               options = std::move(options)]() mutable {
          auto response = execute_request(std::move(request), jar, std::move(options), state);
          store_completed(state, std::move(response));
        });
      }

      ~native_https_request_handle_impl() override {
        cancel_request(state_, cancel_reason::abort);
        if (worker_.joinable())
          worker_.join();
      }

      bool poll(fxe::net::http_response& out) override {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->completed || state_->consumed)
          return false;
        out = std::move(state_->result);
        state_->consumed = true;
        return true;
      }

      void abort() override {
        FXE_TRACE("net.http", "aborting native HTTPS request");
        cancel_request(state_, cancel_reason::abort);
      }

    private:
      std::shared_ptr<shared_state> state_;
      std::thread worker_;
    };
  } // namespace

  native_https_request_handle::~native_https_request_handle() = default;

  std::unique_ptr<native_https_request_handle>
  start_native_https_request(fxe::net::http_request req, fxe::net::cookie_jar* jar,
                             native_https_request_options opts) {
    return std::make_unique<native_https_request_handle_impl>(std::move(req), jar, std::move(opts));
  }

  fxe::net::http_response perform_native_https_request(fxe::net::http_request req,
                                                       fxe::net::cookie_jar* jar,
                                                       native_https_request_options opts) {
    auto handle = start_native_https_request(std::move(req), jar, std::move(opts));
    fxe::net::http_response response;
    while (!handle->poll(response))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return response;
  }

} // namespace fxe::runtime
