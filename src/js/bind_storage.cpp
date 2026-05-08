// Web Storage globals backed by SQLite.

#include "bind_storage.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/v8_strings.hpp>

#include <fxe/v8_helpers.hpp>

#include "../os/os.hpp"

#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fxe/types.hpp>
#include <sqlite3.h>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    enum class storage_kind { local, session };

    struct storage_area {
      storage_kind kind = storage_kind::session;
      sqlite3* db = nullptr;
      bool initialized = false;
      std::string filename;
    };

    struct storage_state {
      storage_area local{storage_kind::local, nullptr, false, {}};
      storage_area session{storage_kind::session, nullptr, false, {}};
    };

    std::unordered_map<Isolate*, storage_state>& state_table() {
      static std::unordered_map<Isolate*, storage_state> table;
      return table;
    }

    Local<String> s(Isolate* iso, std::string_view sv) {
      return String::NewFromUtf8(iso, sv.data(), NewStringType::kNormal,
                                 static_cast<int>(sv.size()))
          .ToLocalChecked();
    }

    Local<String> s(Isolate* iso, const char* z) {
      return String::NewFromUtf8(iso, z ? z : "", NewStringType::kNormal).ToLocalChecked();
    }

    bool is_reserved_name(std::string_view name) {
      return name == "setItem" || name == "getItem" || name == "removeItem" || name == "clear" ||
             name == "key" || name == "length";
    }

    bool throw_sqlite(Isolate* iso, sqlite3* db, const char* fallback = "storage sqlite error") {
      const char* msg = db ? sqlite3_errmsg(db) : fallback;
      if (!msg || !*msg)
        msg = fallback;
      auto err = Exception::Error(s(iso, msg));
      if (db && err->IsObject()) {
        auto ctx = iso->GetCurrentContext();
        auto obj = err.As<Object>();
        (void)obj->Set(ctx, "code"_v8(iso), Integer::New(iso, sqlite3_extended_errcode(db)));
        (void)obj->Set(ctx, "errno"_v8(iso), Integer::New(iso, sqlite3_errcode(db)));
      }
      iso->ThrowException(err);
      return false;
    }

    Maybe<std::string> value_to_string(Isolate* iso, Local<Value> value) {
      auto ctx = iso->GetCurrentContext();
      Local<String> str_value;
      if (!value->ToString(ctx).ToLocal(&str_value))
        return Nothing<std::string>();
      String::Utf8Value utf8(iso, str_value);
      if (!*utf8)
        return Just(std::string{});
      return Just(std::string(*utf8, static_cast<usize>(utf8.length())));
    }

    Maybe<std::string> name_to_string(Isolate* iso, Local<Name> name) {
      if (!name->IsString())
        return Nothing<std::string>();
      String::Utf8Value utf8(iso, name.As<String>());
      if (!*utf8)
        return Just(std::string{});
      return Just(std::string(*utf8, static_cast<usize>(utf8.length())));
    }

    storage_area* area_from_data(Local<Value> data) {
      if (data.IsEmpty() || !data->IsExternal())
        return nullptr;
      return external_ptr<storage_area>(data);
    }

    bool exec_sql(Isolate* iso, sqlite3* db, const char* sql) {
      char* err = nullptr;
      const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
      if (rc == SQLITE_OK)
        return true;
      std::string msg = err ? err : sqlite3_errmsg(db);
      sqlite3_free(err);
      return throw_error(iso, msg);
    }

    bool ensure_db(Isolate* iso, storage_area& area) {
      if (area.initialized)
        return area.db != nullptr;

      sqlite3* db = nullptr;
      std::string filename;
      if (area.kind == storage_kind::local) {
        auto user_data = fxe::os::get_path("userData");
        if (user_data.empty())
          return throw_error(iso, "localStorage cannot resolve App.getPath('userData')");
        std::error_code ec;
        std::filesystem::create_directories(user_data, ec);
        if (ec)
          return throw_error(iso, "localStorage cannot create userData directory: {}",
                             ec.message());
        filename = (std::filesystem::path(user_data) / "storage.sqlite3").string();
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(filename.c_str(), &db, flags, nullptr) != SQLITE_OK) {
          throw_sqlite(iso, db, "localStorage failed to open storage.sqlite3");
          if (db)
            sqlite3_close(db);
          return false;
        }
      } else {
        filename = ":memory:";
        const int flags =
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(filename.c_str(), &db, flags, nullptr) != SQLITE_OK) {
          throw_sqlite(iso, db, "sessionStorage failed to open in-memory database");
          if (db)
            sqlite3_close(db);
          return false;
        }
      }

      if (!exec_sql(iso, db,
                    "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL)")) {
        sqlite3_close(db);
        return false;
      }

      area.db = db;
      area.filename = std::move(filename);
      area.initialized = true;
      return true;
    }

    bool put_value(Isolate* iso, storage_area& area, const std::string& key,
                   const std::string& value) {
      if (!ensure_db(iso, area))
        return false;
      sqlite3_stmt* stmt = nullptr;
      const char* sql = "INSERT INTO kv(k, v) VALUES(?1, ?2) "
                        "ON CONFLICT(k) DO UPDATE SET v = excluded.v";
      if (sqlite3_prepare_v2(area.db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      sqlite3_bind_text(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
      const int rc = sqlite3_step(stmt);
      const int final_rc = sqlite3_finalize(stmt);
      if (rc != SQLITE_DONE)
        return throw_sqlite(iso, area.db);
      if (final_rc != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      return true;
    }

    bool get_value(Isolate* iso, storage_area& area, const std::string& key, std::string& out,
                   bool& found) {
      found = false;
      if (!ensure_db(iso, area))
        return false;
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(area.db, "SELECT v FROM kv WHERE k = ?1", -1, &stmt, nullptr) !=
          SQLITE_OK)
        return throw_sqlite(iso, area.db);
      sqlite3_bind_text(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
      const int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(stmt, 0);
        const int bytes = sqlite3_column_bytes(stmt, 0);
        out.assign(reinterpret_cast<const char*>(text), static_cast<usize>(bytes));
        found = true;
      } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return throw_sqlite(iso, area.db);
      }
      if (sqlite3_finalize(stmt) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      return true;
    }

    bool remove_value(Isolate* iso, storage_area& area, const std::string& key) {
      if (!ensure_db(iso, area))
        return false;
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(area.db, "DELETE FROM kv WHERE k = ?1", -1, &stmt, nullptr) !=
          SQLITE_OK)
        return throw_sqlite(iso, area.db);
      sqlite3_bind_text(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
      const int rc = sqlite3_step(stmt);
      const int final_rc = sqlite3_finalize(stmt);
      if (rc != SQLITE_DONE)
        return throw_sqlite(iso, area.db);
      if (final_rc != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      return true;
    }

    bool clear_values(Isolate* iso, storage_area& area) {
      if (!ensure_db(iso, area))
        return false;
      return exec_sql(iso, area.db, "DELETE FROM kv");
    }

    bool count_values(Isolate* iso, storage_area& area, u32& out) {
      out = 0;
      if (!ensure_db(iso, area))
        return false;
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(area.db, "SELECT COUNT(*) FROM kv", -1, &stmt, nullptr) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      const int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto n = sqlite3_column_int64(stmt, 0);
        out = n > std::numeric_limits<u32>::max() ? std::numeric_limits<u32>::max()
                                                  : static_cast<u32>(n);
      } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return throw_sqlite(iso, area.db);
      }
      if (sqlite3_finalize(stmt) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      return true;
    }

    bool key_at(Isolate* iso, storage_area& area, u32 index, std::string& out, bool& found) {
      found = false;
      if (!ensure_db(iso, area))
        return false;
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(area.db, "SELECT k FROM kv ORDER BY rowid LIMIT 1 OFFSET ?1", -1,
                             &stmt, nullptr) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(index));
      const int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(stmt, 0);
        const int bytes = sqlite3_column_bytes(stmt, 0);
        out.assign(reinterpret_cast<const char*>(text), static_cast<usize>(bytes));
        found = true;
      } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return throw_sqlite(iso, area.db);
      }
      if (sqlite3_finalize(stmt) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      return true;
    }

    bool all_keys(Isolate* iso, storage_area& area, std::vector<std::string>& out) {
      if (!ensure_db(iso, area))
        return false;
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(area.db, "SELECT k FROM kv ORDER BY rowid", -1, &stmt, nullptr) !=
          SQLITE_OK)
        return throw_sqlite(iso, area.db);
      while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
          const auto* text = sqlite3_column_text(stmt, 0);
          const int bytes = sqlite3_column_bytes(stmt, 0);
          out.emplace_back(reinterpret_cast<const char*>(text), static_cast<usize>(bytes));
          continue;
        }
        if (rc == SQLITE_DONE)
          break;
        sqlite3_finalize(stmt);
        return throw_sqlite(iso, area.db);
      }
      if (sqlite3_finalize(stmt) != SQLITE_OK)
        return throw_sqlite(iso, area.db);
      return true;
    }

    Intercepted storage_getter(Local<Name> name, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto maybe_key = name_to_string(iso, name);
      if (maybe_key.IsNothing())
        return Intercepted::kNo;
      auto key = maybe_key.FromJust();
      if (is_reserved_name(key))
        return Intercepted::kNo;
      auto* area = area_from_data(info.Data());
      if (!area)
        return Intercepted::kNo;
      std::string value;
      bool found = false;
      if (!get_value(iso, *area, key, value, found))
        return Intercepted::kYes;
      if (!found)
        return Intercepted::kNo;
      info.GetReturnValue().Set(s(iso, value));
      return Intercepted::kYes;
    }

    Intercepted storage_setter(Local<Name> name, Local<Value> value,
                               const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto maybe_key = name_to_string(iso, name);
      if (maybe_key.IsNothing())
        return Intercepted::kNo;
      auto key = maybe_key.FromJust();
      if (is_reserved_name(key))
        return Intercepted::kNo;
      auto maybe_value = value_to_string(iso, value);
      if (maybe_value.IsNothing())
        return Intercepted::kYes;
      auto* area = area_from_data(info.Data());
      if (!area)
        return Intercepted::kNo;
      (void)put_value(iso, *area, key, maybe_value.FromJust());
      return Intercepted::kYes;
    }

    Intercepted storage_query(Local<Name> name, const PropertyCallbackInfo<Integer>& info) {
      auto* iso = info.GetIsolate();
      auto maybe_key = name_to_string(iso, name);
      if (maybe_key.IsNothing())
        return Intercepted::kNo;
      auto key = maybe_key.FromJust();
      if (is_reserved_name(key))
        return Intercepted::kNo;
      auto* area = area_from_data(info.Data());
      if (!area)
        return Intercepted::kNo;
      std::string value;
      bool found = false;
      if (!get_value(iso, *area, key, value, found))
        return Intercepted::kYes;
      if (!found)
        return Intercepted::kNo;
      info.GetReturnValue().Set(Integer::New(iso, PropertyAttribute::None));
      return Intercepted::kYes;
    }

    Intercepted storage_deleter(Local<Name> name, const PropertyCallbackInfo<Boolean>& info) {
      auto* iso = info.GetIsolate();
      auto maybe_key = name_to_string(iso, name);
      if (maybe_key.IsNothing())
        return Intercepted::kNo;
      auto key = maybe_key.FromJust();
      if (is_reserved_name(key))
        return Intercepted::kNo;
      auto* area = area_from_data(info.Data());
      if (!area)
        return Intercepted::kNo;
      if (!remove_value(iso, *area, key))
        return Intercepted::kYes;
      info.GetReturnValue().Set(true);
      return Intercepted::kYes;
    }

    void storage_enumerator(const PropertyCallbackInfo<Array>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* area = area_from_data(info.Data());
      std::vector<std::string> keys;
      if (area && !all_keys(iso, *area, keys))
        return;
      auto arr = Array::New(iso, static_cast<int>(keys.size()));
      for (u32 i = 0; i < keys.size(); ++i)
        (void)arr->Set(ctx, i, s(iso, keys[i]));
      info.GetReturnValue().Set(arr);
    }

    void storage_set_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 2) {
        (void)throw_type_error(iso, "Storage.setItem requires key and value");
        return;
      }
      auto maybe_key = value_to_string(iso, info[0]);
      auto maybe_value = value_to_string(iso, info[1]);
      if (maybe_key.IsNothing() || maybe_value.IsNothing())
        return;
      auto* area = area_from_data(info.Data());
      if (!area || !put_value(iso, *area, maybe_key.FromJust(), maybe_value.FromJust()))
        return;
    }

    void storage_get_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1) {
        (void)throw_type_error(iso, "Storage.getItem requires key");
        return;
      }
      auto maybe_key = value_to_string(iso, info[0]);
      if (maybe_key.IsNothing())
        return;
      auto* area = area_from_data(info.Data());
      std::string value;
      bool found = false;
      if (!area || !get_value(iso, *area, maybe_key.FromJust(), value, found))
        return;
      if (found)
        info.GetReturnValue().Set(s(iso, value));
      else
        info.GetReturnValue().Set(Null(iso));
    }

    void storage_remove_item(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1) {
        (void)throw_type_error(iso, "Storage.removeItem requires key");
        return;
      }
      auto maybe_key = value_to_string(iso, info[0]);
      if (maybe_key.IsNothing())
        return;
      auto* area = area_from_data(info.Data());
      if (area)
        (void)remove_value(iso, *area, maybe_key.FromJust());
    }

    void storage_clear(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* area = area_from_data(info.Data());
      if (area)
        (void)clear_values(iso, *area);
    }

    void storage_key(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      u32 index = 0;
      if (info.Length() > 0) {
        Maybe<u32> maybe_index = info[0]->Uint32Value(ctx);
        if (maybe_index.IsNothing())
          return;
        index = maybe_index.FromJust();
      }
      auto* area = area_from_data(info.Data());
      std::string key;
      bool found = false;
      if (!area || !key_at(iso, *area, index, key, found))
        return;
      if (found)
        info.GetReturnValue().Set(s(iso, key));
      else
        info.GetReturnValue().Set(Null(iso));
    }

    void storage_length(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* area = area_from_data(info.Data());
      u32 count = 0;
      if (!area || !count_values(iso, *area, count))
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, count));
    }

    void reset_storage_for_isolate(Isolate* iso) {
      auto& table = state_table();
      auto it = table.find(iso);
      if (it == table.end())
        return;
      if (it->second.local.db)
        sqlite3_close(it->second.local.db);
      if (it->second.session.db)
        sqlite3_close(it->second.session.db);
      table.erase(it);
    }

    struct storage_resetter_register {
      storage_resetter_register() {
        register_template_resetter(&reset_storage_for_isolate);
      }
    };
    static storage_resetter_register s_storage_resetter_register;

    Local<ObjectTemplate> make_storage_template(Isolate* iso, storage_area* area) {
      auto t = ObjectTemplate::New(iso);
      auto data = make_external(iso, area);
      NamedPropertyHandlerConfiguration cfg(storage_getter, storage_setter, storage_query,
                                            storage_deleter, storage_enumerator, data);
      t->SetHandler(cfg);
      t->Set(iso, "setItem", FunctionTemplate::New(iso, storage_set_item, data));
      t->Set(iso, "getItem", FunctionTemplate::New(iso, storage_get_item, data));
      t->Set(iso, "removeItem", FunctionTemplate::New(iso, storage_remove_item, data));
      t->Set(iso, "clear", FunctionTemplate::New(iso, storage_clear, data));
      t->Set(iso, "key", FunctionTemplate::New(iso, storage_key, data));
      t->SetAccessorProperty("length"_v8(iso), FunctionTemplate::New(iso, storage_length, data));
      return t;
    }
  } // namespace

  void install_storage_globals(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto& state = state_table()[iso];
    global->Set(iso, "localStorage", make_storage_template(iso, &state.local));
    global->Set(iso, "sessionStorage", make_storage_template(iso, &state.session));
  }
} // namespace fxe::js
