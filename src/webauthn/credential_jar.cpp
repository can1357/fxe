#include "webauthn/credential_jar.hpp"

#include <fxe/log.hpp>
#include <fxe/webauthn.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/private_access.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fxe::webauthn::detail {
  namespace {

    constexpr int k_busy_attempts = 3;
    constexpr auto k_busy_sleep = std::chrono::milliseconds(1);
    constexpr std::string_view k_schema_sql =
        "CREATE TABLE IF NOT EXISTS credentials ("
        "credential_id BLOB PRIMARY KEY,"
        "rp_id TEXT NOT NULL,"
        "rp_id_hash BLOB NOT NULL,"
        "user_id BLOB NOT NULL,"
        "user_name TEXT NOT NULL,"
        "display_name TEXT NOT NULL,"
        "private_key BLOB NOT NULL,"
        "public_x BLOB NOT NULL,"
        "public_y BLOB NOT NULL,"
        "alg INTEGER NOT NULL,"
        "sign_count INTEGER NOT NULL,"
        "resident INTEGER NOT NULL,"
        "user_verified INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_credentials_rp ON credentials(rp_id);"
        "PRAGMA user_version = 1;";

    constexpr std::string_view k_select_sql =
        "SELECT credential_id, rp_id, rp_id_hash, user_id, user_name, display_name, private_key,"
        " public_x, public_y, alg, sign_count, resident, user_verified, created_at"
        " FROM credentials ORDER BY created_at ASC, rowid ASC";
    constexpr std::string_view k_upsert_sql =
        "INSERT INTO credentials ("
        "credential_id, rp_id, rp_id_hash, user_id, user_name, display_name, private_key, public_x,"
        " public_y, alg, sign_count, resident, user_verified, created_at"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14) "
        "ON CONFLICT(credential_id) DO UPDATE SET "
        "rp_id = excluded.rp_id, "
        "rp_id_hash = excluded.rp_id_hash, "
        "user_id = excluded.user_id, "
        "user_name = excluded.user_name, "
        "display_name = excluded.display_name, "
        "private_key = excluded.private_key, "
        "public_x = excluded.public_x, "
        "public_y = excluded.public_y, "
        "alg = excluded.alg, "
        "sign_count = excluded.sign_count, "
        "resident = excluded.resident, "
        "user_verified = excluded.user_verified";
    constexpr std::string_view k_bump_sql =
        "UPDATE credentials SET sign_count = ?1 WHERE credential_id = ?2";
    constexpr std::string_view k_remove_sql = "DELETE FROM credentials WHERE credential_id = ?1";
    constexpr std::string_view k_clear_sql = "DELETE FROM credentials";

    struct pk_guard {
      mbedtls_pk_context ctx;
      pk_guard() {
        mbedtls_pk_init(&ctx);
      }
      ~pk_guard() {
        mbedtls_pk_free(&ctx);
      }
    };

    struct rng_guard {
      mbedtls_entropy_context entropy;
      mbedtls_ctr_drbg_context ctr_drbg;

      rng_guard() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        static constexpr unsigned char personalization[] = "fxe-webauthn-jar";
        const int rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                             personalization, sizeof(personalization) - 1u);
        if (rc != 0) {
          FXE_ERROR("webauthn.jar", "failed to seed jar RNG: {}", rc);
        }
      }

      ~rng_guard() {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
      }
    };

    std::string mbedtls_err_str(int rc) {
      std::array<char, 256> buf{};
      mbedtls_strerror(rc, buf.data(), buf.size());
      return buf.data();
    }

    template <typename Fn> int with_busy_retry(Fn&& fn) {
      int rc = SQLITE_ERROR;
      for (int attempt = 0; attempt < k_busy_attempts; ++attempt) {
        rc = fn();
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
          return rc;
        std::this_thread::sleep_for(k_busy_sleep);
      }
      return rc;
    }

    void log_sqlite(sqlite3* db, std::string_view action) {
      FXE_ERROR("webauthn.jar", "{} failed: {}", action,
                db ? sqlite3_errmsg(db) : "sqlite unavailable");
    }

    bool exec_sql(sqlite3* db, std::string_view sql, std::string_view action) {
      char* message = nullptr;
      const int rc = with_busy_retry([&] {
        if (message != nullptr) {
          sqlite3_free(message);
          message = nullptr;
        }
        return sqlite3_exec(db, sql.data(), nullptr, nullptr, &message);
      });
      if (rc == SQLITE_OK) {
        sqlite3_free(message);
        return true;
      }
      FXE_ERROR("webauthn.jar", "{} failed: {}", action, message ? message : sqlite3_errmsg(db));
      sqlite3_free(message);
      return false;
    }

    bool prepare_stmt(sqlite3* db, std::string_view sql, sqlite3_stmt** out,
                      std::string_view action) {
      const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), out, nullptr);
      if (rc == SQLITE_OK)
        return true;
      log_sqlite(db, action);
      return false;
    }

    void reset_stmt(sqlite3_stmt* stmt) {
      if (stmt == nullptr)
        return;
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    }

    bool bind_blob(sqlite3_stmt* stmt, int index, std::span<const uint8_t> value) {
      if (value.empty())
        return sqlite3_bind_zeroblob(stmt, index, 0) == SQLITE_OK;
      return sqlite3_bind_blob(stmt, index, value.data(), static_cast<int>(value.size()),
                               SQLITE_TRANSIENT) == SQLITE_OK;
    }

    bool bind_text(sqlite3_stmt* stmt, int index, std::string_view value) {
      return sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()),
                               SQLITE_TRANSIENT) == SQLITE_OK;
    }

    std::vector<uint8_t> column_blob(sqlite3_stmt* stmt, int index) {
      const int bytes = sqlite3_column_bytes(stmt, index);
      const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, index));
      if (bytes <= 0)
        return {};
      return std::vector<uint8_t>(data, data + bytes);
    }

    bool parse_private_key_der(std::span<const uint8_t> der, pk_guard& pk) {
      rng_guard rng;
#if MBEDTLS_VERSION_MAJOR >= 3
      const int rc = mbedtls_pk_parse_key(&pk.ctx, der.data(), der.size(), nullptr, 0,
                                          mbedtls_ctr_drbg_random, &rng.ctr_drbg);
#else
      const int rc = mbedtls_pk_parse_key(&pk.ctx, der.data(), der.size(), nullptr, 0);
#endif
      if (rc != 0) {
        FXE_ERROR("webauthn.jar", "failed to parse PKCS8 private key: {}", mbedtls_err_str(rc));
        return false;
      }
      return true;
    }

    std::vector<uint8_t> private_key_der_from_credential(const virtual_credential& cred) {
      pk_guard pk;
      if (mbedtls_pk_setup(&pk.ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
        return {};
      auto* ec = mbedtls_pk_ec(pk.ctx);
      if (ec == nullptr)
        return {};

      int rc = mbedtls_ecp_group_load(&ec->MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
      if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
                                     cred.public_key.x.data(), cred.public_key.x.size());
      }
      if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                                     cred.public_key.y.data(), cred.public_key.y.size());
      }
      if (rc == 0)
        rc = mbedtls_mpi_lset(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1);
      if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&ec->MBEDTLS_PRIVATE(d), cred.private_key_d.data(),
                                     cred.private_key_d.size());
      }
      if (rc != 0)
        return {};

      std::array<unsigned char, 512> buf{};
      const int written = mbedtls_pk_write_key_der(&pk.ctx, buf.data(), buf.size());
      if (written < 0)
        return {};
      return std::vector<uint8_t>(buf.end() - written, buf.end());
    }

    std::optional<virtual_credential> credential_from_row(sqlite3_stmt* stmt) {
      virtual_credential cred;
      cred.credential_id = column_blob(stmt, 0);
      cred.rp_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      cred.rp_id_hash = column_blob(stmt, 2);
      cred.user.id = column_blob(stmt, 3);
      cred.user.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
      cred.user.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
      const auto private_key_der = column_blob(stmt, 6);
      const auto public_x = column_blob(stmt, 7);
      const auto public_y = column_blob(stmt, 8);
      const auto alg = sqlite3_column_int(stmt, 9);
      const auto sign_count = sqlite3_column_int64(stmt, 10);
      cred.resident = sqlite3_column_int(stmt, 11) != 0;
      cred.user_verified = sqlite3_column_int(stmt, 12) != 0;

      if (cred.credential_id.empty() || cred.rp_id_hash.size() != 32u || public_x.size() != 32u ||
          public_y.size() != 32u || alg != static_cast<int>(cose_algorithm::es256) ||
          sign_count < 0 || sign_count > UINT32_MAX) {
        FXE_WARN("webauthn.jar", "dropping invalid credential row");
        return std::nullopt;
      }

      pk_guard parsed;
      if (!parse_private_key_der(private_key_der, parsed))
        return std::nullopt;
      if (mbedtls_pk_get_type(&parsed.ctx) != MBEDTLS_PK_ECKEY &&
          mbedtls_pk_get_type(&parsed.ctx) != MBEDTLS_PK_ECKEY_DH) {
        FXE_WARN("webauthn.jar", "dropping non-EC credential row");
        return std::nullopt;
      }
      auto* ec = mbedtls_pk_ec(parsed.ctx);
      if (ec == nullptr || ec->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1) {
        FXE_WARN("webauthn.jar", "dropping non-P256 credential row");
        return std::nullopt;
      }

      cred.private_key_d.resize(32u);
      const int rc = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(d), cred.private_key_d.data(),
                                              cred.private_key_d.size());
      if (rc != 0) {
        FXE_WARN("webauthn.jar", "dropping credential with unreadable private key: {}",
                 mbedtls_err_str(rc));
        return std::nullopt;
      }

      cred.public_key.alg = cose_algorithm::es256;
      cred.public_key.crv = 1;
      std::memcpy(cred.public_key.x.data(), public_x.data(), public_x.size());
      std::memcpy(cred.public_key.y.data(), public_y.data(), public_y.size());
      cred.sign_count = static_cast<uint32_t>(sign_count);
      return cred;
    }

    int64_t created_at_now() {
      const auto now = std::chrono::system_clock::now();
      return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

  } // namespace

  struct credential_jar::impl {
    sqlite3* db = nullptr;
    sqlite3_stmt* load_stmt = nullptr;
    sqlite3_stmt* upsert_stmt = nullptr;
    sqlite3_stmt* bump_stmt = nullptr;
    sqlite3_stmt* remove_stmt = nullptr;
    sqlite3_stmt* clear_stmt = nullptr;

    ~impl() {
      sqlite3_finalize(load_stmt);
      sqlite3_finalize(upsert_stmt);
      sqlite3_finalize(bump_stmt);
      sqlite3_finalize(remove_stmt);
      sqlite3_finalize(clear_stmt);
      if (db != nullptr) {
        const int rc = sqlite3_close(db);
        if (rc != SQLITE_OK)
          log_sqlite(db, "sqlite3_close");
      }
    }
  };

  credential_jar::credential_jar(std::unique_ptr<impl> i) : impl_(std::move(i)) {}

  credential_jar::~credential_jar() = default;

  std::unique_ptr<credential_jar> credential_jar::open(const std::filesystem::path& path) {
    try {
      const auto parent = path.parent_path();
      if (!parent.empty() && !std::filesystem::exists(parent)) {
        FXE_ERROR("webauthn.jar", "credential jar directory does not exist: {}", parent.string());
        return nullptr;
      }
      if (!parent.empty() && !std::filesystem::is_directory(parent)) {
        FXE_ERROR("webauthn.jar", "credential jar parent is not a directory: {}", parent.string());
        return nullptr;
      }
    } catch (const std::exception& e) {
      FXE_ERROR("webauthn.jar", "credential jar path check failed: {}", e.what());
      return nullptr;
    }

    auto state = std::make_unique<impl>();
    const std::string filename = path.string();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int open_rc = sqlite3_open_v2(filename.c_str(), &state->db, flags, nullptr);
    if (open_rc != SQLITE_OK) {
      log_sqlite(state->db, "open credential jar");
      if (state->db != nullptr) {
        sqlite3_close(state->db);
        state->db = nullptr;
      }
      return nullptr;
    }

    if (!exec_sql(state->db, "PRAGMA journal_mode = WAL;", "set journal_mode") ||
        !exec_sql(state->db, "PRAGMA synchronous = NORMAL;", "set synchronous") ||
        !exec_sql(state->db, "PRAGMA foreign_keys = ON;", "set foreign_keys") ||
        !exec_sql(state->db, k_schema_sql, "initialize credential jar schema") ||
        !prepare_stmt(state->db, k_select_sql, &state->load_stmt, "prepare load statement") ||
        !prepare_stmt(state->db, k_upsert_sql, &state->upsert_stmt, "prepare upsert statement") ||
        !prepare_stmt(state->db, k_bump_sql, &state->bump_stmt, "prepare bump statement") ||
        !prepare_stmt(state->db, k_remove_sql, &state->remove_stmt, "prepare remove statement") ||
        !prepare_stmt(state->db, k_clear_sql, &state->clear_stmt, "prepare clear statement")) {
      return nullptr;
    }

    return std::unique_ptr<credential_jar>(new credential_jar(std::move(state)));
  }

  std::vector<virtual_credential> credential_jar::load_all() {
    std::vector<virtual_credential> out;
    try {
      reset_stmt(impl_->load_stmt);
      for (;;) {
        const int rc = with_busy_retry([&] { return sqlite3_step(impl_->load_stmt); });
        if (rc == SQLITE_DONE)
          break;
        if (rc != SQLITE_ROW) {
          log_sqlite(impl_->db, "load credentials");
          out.clear();
          break;
        }
        if (auto cred = credential_from_row(impl_->load_stmt))
          out.push_back(std::move(*cred));
      }
      reset_stmt(impl_->load_stmt);
    } catch (const std::exception& e) {
      FXE_ERROR("webauthn.jar", "load_all failed: {}", e.what());
      out.clear();
    }
    return out;
  }

  bool credential_jar::upsert(const virtual_credential& cred) {
    try {
      reset_stmt(impl_->upsert_stmt);
      const auto private_key_der = private_key_der_from_credential(cred);
      if (private_key_der.empty()) {
        FXE_ERROR("webauthn.jar", "failed to encode PKCS8 private key for credential");
        return false;
      }
      if (!bind_blob(impl_->upsert_stmt, 1, cred.credential_id) ||
          !bind_text(impl_->upsert_stmt, 2, cred.rp_id) ||
          !bind_blob(impl_->upsert_stmt, 3, cred.rp_id_hash) ||
          !bind_blob(impl_->upsert_stmt, 4, cred.user.id) ||
          !bind_text(impl_->upsert_stmt, 5, cred.user.name) ||
          !bind_text(impl_->upsert_stmt, 6, cred.user.display_name) ||
          !bind_blob(impl_->upsert_stmt, 7, private_key_der) ||
          !bind_blob(impl_->upsert_stmt, 8, cred.public_key.x) ||
          !bind_blob(impl_->upsert_stmt, 9, cred.public_key.y) ||
          sqlite3_bind_int(impl_->upsert_stmt, 10, static_cast<int>(cred.public_key.alg)) !=
              SQLITE_OK ||
          sqlite3_bind_int64(impl_->upsert_stmt, 11, static_cast<sqlite3_int64>(cred.sign_count)) !=
              SQLITE_OK ||
          sqlite3_bind_int(impl_->upsert_stmt, 12, cred.resident ? 1 : 0) != SQLITE_OK ||
          sqlite3_bind_int(impl_->upsert_stmt, 13, cred.user_verified ? 1 : 0) != SQLITE_OK ||
          sqlite3_bind_int64(impl_->upsert_stmt, 14, created_at_now()) != SQLITE_OK) {
        log_sqlite(impl_->db, "bind upsert credential");
        reset_stmt(impl_->upsert_stmt);
        return false;
      }
      const int rc = with_busy_retry([&] { return sqlite3_step(impl_->upsert_stmt); });
      reset_stmt(impl_->upsert_stmt);
      if (rc == SQLITE_DONE)
        return true;
      log_sqlite(impl_->db, "upsert credential");
    } catch (const std::exception& e) {
      FXE_ERROR("webauthn.jar", "upsert failed: {}", e.what());
    }
    return false;
  }

  bool credential_jar::bump_sign_count(const std::vector<uint8_t>& credential_id,
                                       uint32_t new_value) {
    try {
      reset_stmt(impl_->bump_stmt);
      if (sqlite3_bind_int64(impl_->bump_stmt, 1, static_cast<sqlite3_int64>(new_value)) !=
              SQLITE_OK ||
          !bind_blob(impl_->bump_stmt, 2, credential_id)) {
        log_sqlite(impl_->db, "bind sign count update");
        reset_stmt(impl_->bump_stmt);
        return false;
      }
      const int rc = with_busy_retry([&] { return sqlite3_step(impl_->bump_stmt); });
      const bool changed = rc == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
      reset_stmt(impl_->bump_stmt);
      if (rc == SQLITE_DONE)
        return changed;
      log_sqlite(impl_->db, "update sign count");
    } catch (const std::exception& e) {
      FXE_ERROR("webauthn.jar", "bump_sign_count failed: {}", e.what());
    }
    return false;
  }

  bool credential_jar::remove(const std::vector<uint8_t>& credential_id) {
    try {
      reset_stmt(impl_->remove_stmt);
      if (!bind_blob(impl_->remove_stmt, 1, credential_id)) {
        log_sqlite(impl_->db, "bind remove credential");
        reset_stmt(impl_->remove_stmt);
        return false;
      }
      const int rc = with_busy_retry([&] { return sqlite3_step(impl_->remove_stmt); });
      const bool changed = rc == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
      reset_stmt(impl_->remove_stmt);
      if (rc == SQLITE_DONE)
        return changed;
      log_sqlite(impl_->db, "remove credential");
    } catch (const std::exception& e) {
      FXE_ERROR("webauthn.jar", "remove failed: {}", e.what());
    }
    return false;
  }

  bool credential_jar::clear() {
    try {
      reset_stmt(impl_->clear_stmt);
      const int rc = with_busy_retry([&] { return sqlite3_step(impl_->clear_stmt); });
      reset_stmt(impl_->clear_stmt);
      if (rc == SQLITE_DONE)
        return true;
      log_sqlite(impl_->db, "clear credential jar");
    } catch (const std::exception& e) {
      FXE_ERROR("webauthn.jar", "clear failed: {}", e.what());
    }
    return false;
  }

} // namespace fxe::webauthn::detail
