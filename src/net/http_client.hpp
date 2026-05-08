// Minimal HTTP client used by bind_fetch.
//
// Single-threaded API: caller submits a request, polls periodically, and the
// completion callback fires from inside poll(). Backed by libcurl-multi when
// FXE_HAS_CURL is defined; HTTPS/TLS verification is delegated to libcurl until
// the native mbedTLS transport exists. Without libcurl, requests fail
// synchronously with a clear "fetch unavailable" error so callers stay
// observable.
// TODO(net): Phase 9/future work: streaming bodies and native transport parity.
#pragma once
#include "cookie_jar.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <fxe/types.hpp>
#include <string>
#include <utility>
#include <vector>

namespace fxe::net {

  using header_list = std::vector<std::pair<std::string, std::string>>;

  class multipart_form {
  public:
    struct part {
      std::string name;
      std::string value;
      std::string filename;
      std::string content_type;
      std::string bytes;
      bool file = false;
    };

    void add_field(std::string name, std::string value);
    void add_file(std::string name, std::string filename, std::string content_type,
                  std::string bytes);
    bool empty() const noexcept {
      return parts_.empty();
    }
    const std::vector<part>& parts() const noexcept {
      return parts_;
    }

  private:
    std::vector<part> parts_;
  };

  enum class http_error {
    ok,
    no_backend,
    dns,
    connect,
    tls,
    timeout,
    response_truncated,
    abort,
    unknown,
  };

  struct http_request {
    std::string method = "GET";
    std::string url;
    header_list headers;
    std::string body; // raw bytes
    bool follow_redirects = true;
    int timeout_ms = 0; // 0 = library default
    std::string proxy;  // explicit proxy URL; empty = auto-detect environment
    std::string range;  // e.g. "bytes=0-1023"; sent as Range header
    multipart_form multipart;
  };

  struct http_response {
    long status = 0;
    std::string status_text;
    header_list headers;
    std::string body;
    std::string final_url;
    std::string error; // empty on success; includes last_error code on failure
    http_error last_error = http_error::ok;
  };

  using http_callback = std::function<void(http_response)>;
  using http_request_id = u64;

  // Process-wide singleton.
  class http_client {
  public:
    static http_client& instance();

    // Submit a request. Returns an id usable with abort(). The callback fires
    // from inside poll(), on the same thread that calls poll().
    http_request_id submit(http_request req, http_callback cb);

    // Cancel an in-flight request. The callback still fires (with an error
    // populated) so callers can drop their bookkeeping.
    void abort(http_request_id id);

    // Drive completion. Cheap to call every frame.
    void poll();

    // True when libcurl is linked and initialised successfully.
    static bool available();
    cookie_jar& cookies();
    void set_cookie_file_path(std::string path);

    http_client(const http_client&) = delete;
    http_client& operator=(const http_client&) = delete;

  private:
    http_client();
    ~http_client();
    struct impl;
    cookie_jar cookies_;
    impl* p_;
  };

} // namespace fxe::net
