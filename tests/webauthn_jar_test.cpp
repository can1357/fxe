#include <fxe/webauthn.hpp>

#include "webauthn/credential_jar.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
  namespace fs = std::filesystem;
  namespace webauthn = fxe::webauthn;
  namespace detail = fxe::webauthn::detail;

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  int test_pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
  }

  void set_env_var(const char* key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
  }

  void unset_env_var(const char* key) {
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
  }

  struct env_guard {
    std::string key;
    bool had_value = false;
    std::string value;

    explicit env_guard(std::string env_key) : key(std::move(env_key)) {
      if (const char* current = std::getenv(key.c_str())) {
        had_value = true;
        value = current;
      }
    }

    ~env_guard() {
      if (had_value)
        set_env_var(key.c_str(), value);
      else
        unset_env_var(key.c_str());
    }
  };

  std::vector<uint8_t> filled(size_t n, uint8_t value) {
    return std::vector<uint8_t>(n, value);
  }

  webauthn::creation_options make_creation_options() {
    webauthn::creation_options create;
    create.rp_id = "example.com";
    create.rp_name = "Example";
    create.user.id = {'u', '-', '1'};
    create.user.name = "alice";
    create.user.display_name = "Alice";
    create.challenge = filled(16u, 0x11);
    create.pub_key_params = {webauthn::cose_algorithm::es256};
    create.attestation = "none";
    return create;
  }

  webauthn::request_options make_request_options(const std::vector<uint8_t>& credential_id) {
    webauthn::request_options request;
    request.rp_id = "example.com";
    request.challenge = filled(16u, 0x22);
    request.allow_credentials = {credential_id};
    request.user_verification = "preferred";
    return request;
  }

  bool same_credential(const webauthn::virtual_credential& lhs,
                       const webauthn::virtual_credential& rhs) {
    return lhs.credential_id == rhs.credential_id && lhs.rp_id_hash == rhs.rp_id_hash &&
           lhs.rp_id == rhs.rp_id && lhs.user.id == rhs.user.id && lhs.user.name == rhs.user.name &&
           lhs.user.display_name == rhs.user.display_name &&
           lhs.private_key_d == rhs.private_key_d && lhs.public_key.alg == rhs.public_key.alg &&
           lhs.public_key.crv == rhs.public_key.crv && lhs.public_key.x == rhs.public_key.x &&
           lhs.public_key.y == rhs.public_key.y && lhs.sign_count == rhs.sign_count &&
           lhs.user_verified == rhs.user_verified && lhs.resident == rhs.resident;
  }

  size_t jar_row_count(const fs::path& path) {
    auto jar = detail::credential_jar::open(path);
    CHECK(jar != nullptr);
    if (!jar)
      return 0;
    return jar->load_all().size();
  }

} // namespace

int main() {
  env_guard env("FXE_WEBAUTHN_JAR_PATH");
  const fs::path jar_path =
      fs::temp_directory_path() / ("fxe_webauthn_jar_" + std::to_string(test_pid()) + ".sqlite");
  const fs::path no_env_path = fs::temp_directory_path() / ("fxe_webauthn_jar_no_env_" +
                                                            std::to_string(test_pid()) + ".sqlite");
  const fs::path seeded_path = fs::temp_directory_path() / ("fxe_webauthn_jar_seeded_" +
                                                            std::to_string(test_pid()) + ".sqlite");
  std::error_code ec;
  fs::remove(jar_path, ec);
  fs::remove(no_env_path, ec);
  fs::remove(seeded_path, ec);

  const auto create = make_creation_options();
  webauthn::virtual_credential persisted;

  set_env_var("FXE_WEBAUTHN_JAR_PATH", jar_path.string());
  {
    webauthn::virtual_authenticator auth;
    webauthn::register_response registered;
    const std::string register_error =
        auth.register_credential(create, "https://example.com", registered);
    CHECK(register_error.empty());
    CHECK(fs::exists(jar_path));
    const auto credentials = auth.list_credentials();
    CHECK(credentials.size() == 1u);
    if (credentials.size() == 1u)
      persisted = credentials[0];
    CHECK(jar_row_count(jar_path) == 1u);
  }

  {
    webauthn::virtual_authenticator auth;
    const auto credentials = auth.list_credentials();
    CHECK(credentials.size() == 1u);
    if (credentials.size() == 1u) {
      CHECK(same_credential(credentials[0], persisted));
      CHECK(credentials[0].sign_count == 0u);
    }
    CHECK(jar_row_count(jar_path) == 1u);

    webauthn::assert_response assertion;
    const std::string assert_error = auth.assert_credential(
        make_request_options(persisted.credential_id), "https://example.com", assertion);
    CHECK(assert_error.empty());
    const auto live_credentials = auth.list_credentials();
    CHECK(live_credentials.size() == 1u);
    if (live_credentials.size() == 1u)
      CHECK(live_credentials[0].sign_count == 1u);
    CHECK(jar_row_count(jar_path) == 1u);
  }

  {
    webauthn::virtual_authenticator auth;
    const auto credentials = auth.list_credentials();
    CHECK(credentials.size() == 1u);
    if (credentials.size() == 1u) {
      CHECK(credentials[0].sign_count == 1u);
      persisted = credentials[0];
    }
    CHECK(jar_row_count(jar_path) == 1u);
    auth.clear();
    CHECK(auth.list_credentials().empty());
    CHECK(jar_row_count(jar_path) == 0u);
  }

  {
    webauthn::virtual_authenticator auth;
    CHECK(auth.list_credentials().empty());
    CHECK(jar_row_count(jar_path) == 0u);
  }

  unset_env_var("FXE_WEBAUTHN_JAR_PATH");
  CHECK(!fs::exists(no_env_path));
  {
    webauthn::virtual_authenticator auth;
    CHECK(auth.list_credentials().empty());
  }
  CHECK(!fs::exists(no_env_path));

  set_env_var("FXE_WEBAUTHN_JAR_PATH", seeded_path.string());
  {
    webauthn::virtual_authenticator auth(0xC0FFEEu);
    webauthn::register_response registered;
    const std::string register_error =
        auth.register_credential(create, "https://example.com", registered);
    CHECK(register_error.empty());
    CHECK(auth.list_credentials().size() == 1u);
  }
  CHECK(!fs::exists(seeded_path));

  fs::remove(jar_path, ec);
  fs::remove(no_env_path, ec);
  fs::remove(seeded_path, ec);
  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
