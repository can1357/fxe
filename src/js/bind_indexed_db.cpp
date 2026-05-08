// IndexedDB binding (sqlite-backed).
//
// Each database lives in `${userData}/idb/<safe(name)>.sqlite3`. The schema is:
//   __meta(schema_version INTEGER, user_version INTEGER, name TEXT)
//   __stores(name TEXT PRIMARY KEY, key_path TEXT, auto_increment INTEGER, auto_seq INTEGER)
//   __indexes(store TEXT, name TEXT, key_path TEXT, is_unique INTEGER, is_multi INTEGER, PRIMARY
//   KEY(store, name)) store_<name>(k BLOB PRIMARY KEY, v BLOB NOT NULL) idx_<store>_<name>(ik BLOB,
//   pk BLOB, PRIMARY KEY(ik, pk))
//
// Keys are encoded with a binary-comparable scheme (0x10=number, 0x20=Date,
// 0x30=string, 0x40=binary) so SQLite's default BLOB ordering matches IDB key
// order. Values are stored as v8::ValueSerializer blobs (structured clone).
//
// Scope:
//   * Key types: number, string, Date, ArrayBuffer. Arrays-as-keys NOT supported in v1.
//   * Object stores: put/add/get/getAll/getKey/getAllKeys/delete/clear/count/index/openCursor.
//   * Indexes: get/getAll/getKey/getAllKeys/count/openCursor.
//   * Cursors: forward-only continue() (no key arg), update(), delete().
//     advance(N) and continue(key) are documented as not yet supported.
//   * Transactions: BEGIN at construction; explicit commit()/abort(); GC = ROLLBACK.
//   * Open with upgradeneeded; single-process so no `blocked`/`versionchange` cross-conn events.

#include "bind_indexed_db.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/v8_strings.hpp>

#include "../os/os.hpp"
#include "weak_holder.hpp"
#include <fxe/v8_helpers.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <v8.h>

namespace fxe::js {
  using namespace v8;
  namespace {

    constexpr u32 TAG_IDB_KEY_RANGE = 0x494B524Eu;   // 'IKRN'
    constexpr u32 TAG_IDB_REQUEST = 0x49524551u;     // 'IREQ'
    constexpr u32 TAG_IDB_TRANSACTION = 0x4954584Eu; // 'ITXN'
    constexpr u32 TAG_IDB_STORE = 0x4953544Fu;       // 'ISTO'
    constexpr u32 TAG_IDB_CURSOR = 0x49435253u;      // 'ICRS'
    constexpr u32 TAG_IDB_DATABASE = 0x49444241u;    // 'IDBA'
    // ============================== V8 helpers ==============================

    Local<String> s(Isolate* iso, std::string_view sv) {
      return String::NewFromUtf8(iso, sv.data(), NewStringType::kNormal,
                                 static_cast<int>(sv.size()))
          .ToLocalChecked();
    }
    Local<String> s(Isolate* iso, const char* z) {
      return String::NewFromUtf8(iso, z ? z : "", NewStringType::kNormal).ToLocalChecked();
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      if (v.IsEmpty() || !v->IsString())
        return {};
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, static_cast<size_t>(u.length())) : std::string{};
    }

    void set_prop(Local<Context> ctx, Local<Object> obj, const char* key, Local<Value> v) {
      auto* iso = Isolate::GetCurrent();
      (void)obj->Set(ctx, s(iso, key), v);
    }
    void set_str(Local<Context> ctx, Local<Object> obj, const char* key, std::string_view v) {
      set_prop(ctx, obj, key, s(Isolate::GetCurrent(), v));
    }
    void set_int(Local<Context> ctx, Local<Object> obj, const char* key, int64_t v) {
      set_prop(ctx, obj, key, Number::New(Isolate::GetCurrent(), static_cast<double>(v)));
    }
    void throw_msg(Isolate* iso, std::string_view msg, const char* name = "Error") {
      auto err = Exception::Error(s(iso, msg)).As<Object>();
      auto ctx = iso->GetCurrentContext();
      (void)err->Set(ctx, "name"_v8(iso), s(iso, name));
      iso->ThrowException(err);
    }

    Local<Value> make_dom_error(Isolate* iso, std::string_view name, std::string_view msg) {
      auto err = Exception::Error(s(iso, msg)).As<Object>();
      auto ctx = iso->GetCurrentContext();
      (void)err->Set(ctx, "name"_v8(iso), s(iso, name));
      return err;
    }
    // ============================== Key encoding ==============================
    // Tag bytes (lower = sorts first per IDB key order):
    //   0x10 number, 0x20 Date, 0x30 string, 0x40 binary.
    // Number/Date: 8-byte big-endian double with bit twiddle:
    //   if sign bit set (negative): flip all 64 bits
    //   if positive: flip only the sign bit
    // String/binary: UTF-8 / raw bytes with 0x00 escaped to 0x00 0x01,
    //   terminated by 0x00 0x00. (BLOB compare is bytewise; this preserves order.)

    void encode_double_bytes(double d, uint8_t out[8]) {
      uint64_t u;
      std::memcpy(&u, &d, sizeof(u));
      if (u & (uint64_t{1} << 63)) {
        u = ~u; // negative: flip all bits
      } else {
        u ^= (uint64_t{1} << 63); // positive: flip sign bit
      }
      for (int i = 0; i < 8; ++i)
        out[7 - i] = static_cast<uint8_t>(u >> (i * 8));
    }

    bool decode_double_bytes(const uint8_t in[8], double& out) {
      uint64_t u = 0;
      for (int i = 0; i < 8; ++i)
        u = (u << 8) | in[i];
      if (u & (uint64_t{1} << 63))
        u ^= (uint64_t{1} << 63);
      else
        u = ~u;
      std::memcpy(&out, &u, sizeof(out));
      return true;
    }

    void encode_byte_run(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
      for (size_t i = 0; i < len; ++i) {
        out.push_back(data[i]);
        if (data[i] == 0x00)
          out.push_back(0x01); // escape NUL → 0x00 0x01
      }
      out.push_back(0x00);
      out.push_back(0x00); // terminator
    }

    bool decode_byte_run(const uint8_t* data, size_t len, size_t& pos,
                         std::vector<uint8_t>& bytes_out) {
      while (pos < len) {
        uint8_t b = data[pos++];
        if (b == 0x00) {
          if (pos >= len)
            return false;
          uint8_t b2 = data[pos++];
          if (b2 == 0x00)
            return true; // terminator
          if (b2 == 0x01) {
            bytes_out.push_back(0x00);
          } else {
            return false;
          }
        } else {
          bytes_out.push_back(b);
        }
      }
      return false;
    }

    // Returns true on successful encode. Throws JS TypeError + returns false on
    // unsupported key (NaN, array, object, etc).
    bool encode_key(Isolate* iso, Local<Value> v, std::vector<uint8_t>& out) {
      auto ctx = iso->GetCurrentContext();
      if (v.IsEmpty() || v->IsUndefined() || v->IsNull()) {
        throw_msg(iso, "IndexedDB key is required", "DataError");
        return false;
      }
      if (v->IsNumber()) {
        double d = v->NumberValue(ctx).FromMaybe(std::nan(""));
        if (std::isnan(d)) {
          throw_msg(iso, "IndexedDB key cannot be NaN", "DataError");
          return false;
        }
        out.push_back(0x10);
        uint8_t buf[8];
        encode_double_bytes(d, buf);
        out.insert(out.end(), buf, buf + 8);
        return true;
      }
      if (v->IsDate()) {
        double d = v.As<Date>()->ValueOf();
        if (std::isnan(d)) {
          throw_msg(iso, "IndexedDB Date key cannot be NaN", "DataError");
          return false;
        }
        out.push_back(0x20);
        uint8_t buf[8];
        encode_double_bytes(d, buf);
        out.insert(out.end(), buf, buf + 8);
        return true;
      }
      if (v->IsString()) {
        out.push_back(0x30);
        String::Utf8Value u(iso, v);
        if (!*u) {
          out.push_back(0x00);
          out.push_back(0x00);
          return true;
        }
        encode_byte_run(reinterpret_cast<const uint8_t*>(*u), static_cast<size_t>(u.length()), out);
        return true;
      }
      if (v->IsArrayBuffer() || v->IsArrayBufferView()) {
        out.push_back(0x40);
        if (v->IsArrayBuffer()) {
          auto buf = v.As<ArrayBuffer>();
          auto store = buf->GetBackingStore();
          encode_byte_run(static_cast<const uint8_t*>(store->Data()), store->ByteLength(), out);
        } else {
          auto view = v.As<ArrayBufferView>();
          auto store = view->Buffer()->GetBackingStore();
          encode_byte_run(static_cast<const uint8_t*>(store->Data()) + view->ByteOffset(),
                          view->ByteLength(), out);
        }
        return true;
      }
      throw_msg(iso, "IndexedDB key must be number, string, Date, or ArrayBuffer (v1 omits arrays)",
                "DataError");
      return false;
    }

    Local<Value> decode_key(Isolate* iso, const uint8_t* data, size_t len) {
      auto ctx = iso->GetCurrentContext();
      if (len == 0)
        return Undefined(iso);
      uint8_t tag = data[0];
      size_t pos = 1;
      if ((tag == 0x10 || tag == 0x20) && len >= 9) {
        double d = 0;
        decode_double_bytes(data + 1, d);
        if (tag == 0x20)
          return Date::New(ctx, d).ToLocalChecked();
        return Number::New(iso, d);
      }
      if (tag == 0x30) {
        std::vector<uint8_t> bytes;
        if (!decode_byte_run(data, len, pos, bytes))
          return Undefined(iso);
        return String::NewFromUtf8(iso, reinterpret_cast<const char*>(bytes.data()),
                                   NewStringType::kNormal, static_cast<int>(bytes.size()))
            .ToLocalChecked();
      }
      if (tag == 0x40) {
        std::vector<uint8_t> bytes;
        if (!decode_byte_run(data, len, pos, bytes))
          return Undefined(iso);
        auto store = ArrayBuffer::NewBackingStore(iso, bytes.size());
        if (!bytes.empty())
          std::memcpy(store->Data(), bytes.data(), bytes.size());
        return ArrayBuffer::New(iso, std::move(store));
      }
      return Undefined(iso);
    }

    int compare_blobs(const uint8_t* a, size_t alen, const uint8_t* b, size_t blen) {
      size_t n = std::min(alen, blen);
      int c = std::memcmp(a, b, n);
      if (c != 0)
        return c < 0 ? -1 : 1;
      if (alen == blen)
        return 0;
      return alen < blen ? -1 : 1;
    }

    // ============================== Key-path resolution ==============================

    Local<Value> resolve_key_path_segment(Isolate* iso, Local<Value> v, std::string_view seg) {
      auto ctx = iso->GetCurrentContext();
      if (!v->IsObject())
        return Undefined(iso);
      auto obj = v.As<Object>();
      Local<Value> out;
      if (!obj->Get(ctx, s(iso, seg)).ToLocal(&out))
        return Undefined(iso);
      return out;
    }

    Local<Value> resolve_key_path(Isolate* iso, Local<Value> root, std::string_view path) {
      if (path.empty())
        return root;
      Local<Value> cur = root;
      size_t start = 0;
      while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string_view seg = path.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        cur = resolve_key_path_segment(iso, cur, seg);
        if (cur->IsUndefined())
          return cur;
        if (dot == std::string_view::npos)
          break;
        start = dot + 1;
      }
      return cur;
    }

    // Inject a value at keypath into obj. Used to write the auto-increment key
    // back into the value before serialisation.
    bool inject_key_path(Isolate* iso, Local<Object> obj, std::string_view path,
                         Local<Value> value) {
      auto ctx = iso->GetCurrentContext();
      if (path.empty())
        return false;
      size_t start = 0;
      Local<Object> cur = obj;
      while (true) {
        size_t dot = path.find('.', start);
        std::string_view seg = path.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (dot == std::string_view::npos) {
          (void)cur->Set(ctx, s(iso, seg), value);
          return true;
        }
        Local<Value> next;
        if (!cur->Get(ctx, s(iso, seg)).ToLocal(&next) || !next->IsObject()) {
          auto fresh = Object::New(iso);
          (void)cur->Set(ctx, s(iso, seg), fresh);
          cur = fresh;
        } else {
          cur = next.As<Object>();
        }
        start = dot + 1;
      }
    }

    // ============================== Value serialization ==============================

    bool serialize_value(Isolate* iso, Local<Context> ctx, Local<Value> v,
                         std::vector<uint8_t>& out, std::string& err) {
      ValueSerializer set(iso);
      set.WriteHeader();
      if (!set.WriteValue(ctx, v).FromMaybe(false)) {
        err = "value could not be structured-cloned";
        return false;
      }
      auto pair = set.Release();
      out.assign(pair.first, pair.first + pair.second);
      free(pair.first);
      return true;
    }

    MaybeLocal<Value> deserialize_value(Isolate* iso, Local<Context> ctx, const uint8_t* data,
                                        size_t len) {
      ValueDeserializer deser(iso, data, len);
      if (!deser.ReadHeader(ctx).FromMaybe(false))
        return MaybeLocal<Value>();
      return deser.ReadValue(ctx);
    }

    // ============================== Database state ==============================

    struct index_meta {
      std::string name;
      std::string key_path;
      bool unique = false;
      bool multi = false;
    };
    struct store_meta {
      std::string name;
      std::optional<std::string> key_path;
      bool auto_increment = false;
      int64_t auto_seq = 0;
      std::unordered_map<std::string, index_meta> indexes;
    };

    struct database_state {
      std::string name;
      std::string filename;
      sqlite3* db = nullptr;
      int64_t version = 0;
      bool closed = false;
      int open_connections = 0;
      std::unordered_map<std::string, store_meta> stores;
      std::mutex mu;
    };

    // global registry: per-isolate map of name → database_state shared_ptr.
    struct isolate_registry {
      std::unordered_map<std::string, std::shared_ptr<database_state>> by_name;
    };

    std::unordered_map<Isolate*, isolate_registry>& registry_table() {
      static std::unordered_map<Isolate*, isolate_registry> table;
      return table;
    }

    // ============================== SQL helpers ==============================

    bool exec_sql(database_state* st, const char* sql, std::string& err) {
      char* msg = nullptr;
      int rc = sqlite3_exec(st->db, sql, nullptr, nullptr, &msg);
      if (rc == SQLITE_OK)
        return true;
      err = msg ? msg : sqlite3_errmsg(st->db);
      sqlite3_free(msg);
      return false;
    }

    struct stmt_guard {
      sqlite3_stmt* stmt = nullptr;
      ~stmt_guard() {
        if (stmt)
          sqlite3_finalize(stmt);
      }
    };

    void bind_blob(sqlite3_stmt* stmt, int i, const std::vector<uint8_t>& blob) {
      if (blob.empty())
        sqlite3_bind_zeroblob(stmt, i, 0);
      else
        sqlite3_bind_blob(stmt, i, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    }

    // ============================== Schema bootstrap ==============================

    bool ensure_schema(database_state* st, std::string& err) {
      const char* ddl =
          "CREATE TABLE IF NOT EXISTS __meta (schema_version INTEGER NOT NULL,"
          " user_version INTEGER NOT NULL, name TEXT NOT NULL);"
          "CREATE TABLE IF NOT EXISTS __stores (name TEXT PRIMARY KEY,"
          " key_path TEXT, auto_increment INTEGER NOT NULL DEFAULT 0,"
          " auto_seq INTEGER NOT NULL DEFAULT 0);"
          "CREATE TABLE IF NOT EXISTS __indexes (store TEXT NOT NULL, name TEXT NOT NULL,"
          " key_path TEXT NOT NULL, is_unique INTEGER NOT NULL DEFAULT 0,"
          " is_multi INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(store, name));";
      if (!exec_sql(st, ddl, err))
        return false;

      // Initialize __meta if empty.
      sqlite3_stmt* sel = nullptr;
      if (sqlite3_prepare_v2(st->db, "SELECT user_version FROM __meta LIMIT 1", -1, &sel,
                             nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(st->db);
        return false;
      }
      stmt_guard sg{sel};
      int rc = sqlite3_step(sel);
      if (rc == SQLITE_ROW) {
        st->version = sqlite3_column_int64(sel, 0);
      } else if (rc == SQLITE_DONE) {
        sqlite3_stmt* ins = nullptr;
        if (sqlite3_prepare_v2(
                st->db, "INSERT INTO __meta(schema_version, user_version, name) VALUES (1, 0, ?1)",
                -1, &ins, nullptr) != SQLITE_OK) {
          err = sqlite3_errmsg(st->db);
          return false;
        }
        stmt_guard sg2{ins};
        sqlite3_bind_text(ins, 1, st->name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(ins) != SQLITE_DONE) {
          err = sqlite3_errmsg(st->db);
          return false;
        }
        st->version = 0;
      } else {
        err = sqlite3_errmsg(st->db);
        return false;
      }

      // Load stores.
      st->stores.clear();
      sqlite3_stmt* ls = nullptr;
      if (sqlite3_prepare_v2(st->db,
                             "SELECT name, key_path, auto_increment, auto_seq FROM __stores", -1,
                             &ls, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(st->db);
        return false;
      }
      stmt_guard sg3{ls};
      while ((rc = sqlite3_step(ls)) == SQLITE_ROW) {
        store_meta m;
        m.name = reinterpret_cast<const char*>(sqlite3_column_text(ls, 0));
        if (sqlite3_column_type(ls, 1) != SQLITE_NULL)
          m.key_path = reinterpret_cast<const char*>(sqlite3_column_text(ls, 1));
        m.auto_increment = sqlite3_column_int(ls, 2) != 0;
        m.auto_seq = sqlite3_column_int64(ls, 3);
        st->stores[m.name] = std::move(m);
      }
      if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(st->db);
        return false;
      }

      // Load indexes.
      sqlite3_stmt* li = nullptr;
      if (sqlite3_prepare_v2(st->db,
                             "SELECT store, name, key_path, is_unique, is_multi FROM __indexes", -1,
                             &li, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(st->db);
        return false;
      }
      stmt_guard sg4{li};
      while ((rc = sqlite3_step(li)) == SQLITE_ROW) {
        std::string store_name = reinterpret_cast<const char*>(sqlite3_column_text(li, 0));
        index_meta idx;
        idx.name = reinterpret_cast<const char*>(sqlite3_column_text(li, 1));
        idx.key_path = reinterpret_cast<const char*>(sqlite3_column_text(li, 2));
        idx.unique = sqlite3_column_int(li, 3) != 0;
        idx.multi = sqlite3_column_int(li, 4) != 0;
        auto it = st->stores.find(store_name);
        if (it != st->stores.end())
          it->second.indexes[idx.name] = std::move(idx);
      }
      if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(st->db);
        return false;
      }

      return true;
    }

    std::string sanitize_db_filename(std::string_view name) {
      std::string out;
      out.reserve(name.size());
      for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
          out.push_back(c);
        else
          out.push_back('_');
      }
      if (out.empty())
        out = "_default";
      return out;
    }

    std::optional<std::string> resolve_db_filename(std::string_view name, std::string& err) {
      auto user_data = fxe::os::get_path("userData");
      if (user_data.empty()) {
        err = "App.getPath('userData') is unavailable";
        return std::nullopt;
      }
      std::error_code ec;
      auto idb_dir = std::filesystem::path(user_data) / "idb";
      std::filesystem::create_directories(idb_dir, ec);
      if (ec) {
        err = "cannot create idb directory: " + ec.message();
        return std::nullopt;
      }
      return (idb_dir / (sanitize_db_filename(name) + ".sqlite3")).string();
    }

    // ============================== Forward decls ==============================

    struct request_state;
    struct transaction_state;
    struct database_handle;
    struct store_handle;

    struct idb_templates {
      Global<FunctionTemplate> request_tpl;
      Global<FunctionTemplate> open_request_tpl;
      Global<FunctionTemplate> database_tpl;
      Global<FunctionTemplate> transaction_tpl;
      Global<FunctionTemplate> object_store_tpl;
      Global<FunctionTemplate> index_tpl;
      Global<FunctionTemplate> cursor_tpl;
      Global<FunctionTemplate> cursor_with_value_tpl;
      Global<FunctionTemplate> key_range_tpl;
    };

    std::unordered_map<Isolate*, idb_templates>& templates_table() {
      static std::unordered_map<Isolate*, idb_templates> table;
      return table;
    }
    idb_templates& tpl_for(Isolate* iso) {
      return templates_table()[iso];
    }

    Local<Object> new_instance_from(Isolate* iso, Local<Context> ctx, Global<FunctionTemplate>& tpl,
                                    int internal_count = 1) {
      (void)internal_count;
      auto t = tpl.Get(iso);
      auto inst = t->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      return inst;
    }

    // ============================== IDBKeyRange ==============================

    struct key_range : weak_holder<key_range> {
      bool has_lower = false;
      bool lower_open = false;
      std::vector<uint8_t> lower;
      bool has_upper = false;
      bool upper_open = false;
      std::vector<uint8_t> upper;
    };

    Local<Object> wrap_key_range(Isolate* iso, Local<Context> ctx, key_range range) {
      auto inst = new_instance_from(iso, ctx, tpl_for(iso).key_range_tpl);
      auto* state = new key_range(std::move(range));
      set_native(iso, inst, state, TAG_IDB_KEY_RANGE);
      state->bind(iso, inst);
      // Mirror key fields as JS-visible properties.
      if (state->has_lower) {
        (void)inst->Set(ctx, "lower"_v8(iso),
                        decode_key(iso, state->lower.data(), state->lower.size()));
      } else {
        (void)inst->Set(ctx, "lower"_v8(iso), Undefined(iso));
      }
      if (state->has_upper) {
        (void)inst->Set(ctx, "upper"_v8(iso),
                        decode_key(iso, state->upper.data(), state->upper.size()));
      } else {
        (void)inst->Set(ctx, "upper"_v8(iso), Undefined(iso));
      }
      (void)inst->Set(ctx, "lowerOpen"_v8(iso), Boolean::New(iso, state->lower_open));
      (void)inst->Set(ctx, "upperOpen"_v8(iso), Boolean::New(iso, state->upper_open));
      return inst;
    }

    key_range* unwrap_key_range(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 1)
        return nullptr;
      return static_cast<key_range*>(unwrap(obj, TAG_IDB_KEY_RANGE));
    }

    void key_range_only(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1) {
        throw_msg(iso, "IDBKeyRange.only requires a value", "TypeError");
        return;
      }
      key_range r;
      if (!encode_key(iso, info[0], r.lower))
        return;
      r.has_lower = true;
      r.has_upper = true;
      r.upper = r.lower;
      info.GetReturnValue().Set(wrap_key_range(iso, iso->GetCurrentContext(), std::move(r)));
    }
    void key_range_lower_bound(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_msg(iso, "IDBKeyRange.lowerBound requires a value", "TypeError");
        return;
      }
      key_range r;
      if (!encode_key(iso, info[0], r.lower))
        return;
      r.has_lower = true;
      r.lower_open = info.Length() > 1 && info[1]->BooleanValue(iso);
      info.GetReturnValue().Set(wrap_key_range(iso, ctx, std::move(r)));
    }
    void key_range_upper_bound(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_msg(iso, "IDBKeyRange.upperBound requires a value", "TypeError");
        return;
      }
      key_range r;
      if (!encode_key(iso, info[0], r.upper))
        return;
      r.has_upper = true;
      r.upper_open = info.Length() > 1 && info[1]->BooleanValue(iso);
      info.GetReturnValue().Set(wrap_key_range(iso, ctx, std::move(r)));
    }
    void key_range_bound(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2) {
        throw_msg(iso, "IDBKeyRange.bound requires lower and upper", "TypeError");
        return;
      }
      key_range r;
      if (!encode_key(iso, info[0], r.lower) || !encode_key(iso, info[1], r.upper))
        return;
      r.has_lower = true;
      r.has_upper = true;
      r.lower_open = info.Length() > 2 && info[2]->BooleanValue(iso);
      r.upper_open = info.Length() > 3 && info[3]->BooleanValue(iso);
      int c = compare_blobs(r.lower.data(), r.lower.size(), r.upper.data(), r.upper.size());
      if (c > 0 || (c == 0 && (r.lower_open || r.upper_open))) {
        throw_msg(iso, "IDBKeyRange.bound: lower must be <= upper", "DataError");
        return;
      }
      info.GetReturnValue().Set(wrap_key_range(iso, ctx, std::move(r)));
    }
    void key_range_includes(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_key_range(info.This());
      if (!r) {
        info.GetReturnValue().Set(false);
        return;
      }
      if (info.Length() < 1) {
        info.GetReturnValue().Set(false);
        return;
      }
      std::vector<uint8_t> k;
      if (!encode_key(iso, info[0], k))
        return;
      bool ok = true;
      if (r->has_lower) {
        int c = compare_blobs(k.data(), k.size(), r->lower.data(), r->lower.size());
        if (r->lower_open ? c <= 0 : c < 0)
          ok = false;
      }
      if (ok && r->has_upper) {
        int c = compare_blobs(k.data(), k.size(), r->upper.data(), r->upper.size());
        if (r->upper_open ? c >= 0 : c > 0)
          ok = false;
      }
      (void)ctx;
      info.GetReturnValue().Set(ok);
    }

    // Decode an "argument" that is either a key (encoded as bound) or an IDBKeyRange.
    bool decode_query(Isolate* iso, Local<Value> v, key_range& out, bool allow_undefined) {
      if (v.IsEmpty() || v->IsUndefined() || v->IsNull()) {
        if (!allow_undefined) {
          throw_msg(iso, "IndexedDB query is required", "TypeError");
          return false;
        }
        return true;
      }
      if (v->IsObject()) {
        auto obj = v.As<Object>();
        // Is it an IDBKeyRange instance?
        auto* range = unwrap_key_range(obj);
        if (range) {
          out = *range;
          return true;
        }
      }
      // Treat as a single key; key range = only(value).
      std::vector<uint8_t> k;
      if (!encode_key(iso, v, k))
        return false;
      out.has_lower = true;
      out.has_upper = true;
      out.lower = k;
      out.upper = std::move(k);
      return true;
    }

    // ============================== Request / Open Request ==============================
    // IDBRequest is a plain object with callable property listeners (onsuccess /
    // onerror / onupgradeneeded). We synchronously perform work then dispatch
    // success/error in a microtask so callers can wire listeners after the call
    // returns (matches IDB semantics).

    enum class request_kind { generic, open };

    struct request_state {
      request_kind kind = request_kind::generic;
      Global<Object> self;        // the JS request object
      Global<Object> source;      // source (store / index / null)
      Global<Object> transaction; // transaction (or empty)
      Global<Value> result;
      Global<Value> error;
      bool dispatched = false;
    };

    void request_finalizer(const WeakCallbackInfo<request_state>& info) {
      auto* st = info.GetParameter();
      if (st)
        st->self.Reset();
      delete st;
    }

    void invoke_listener(Isolate* iso, Local<Context> ctx, Local<Object> req, const char* prop_name,
                         Local<Value> arg) {
      Local<Value> handler;
      if (!req->Get(ctx, s(iso, prop_name)).ToLocal(&handler))
        return;
      if (!handler->IsFunction())
        return;
      auto fn = handler.As<Function>();
      Local<Value> argv[1] = {arg};
      TryCatch tc(iso);
      (void)fn->Call(ctx, req, 1, argv);
      if (tc.HasCaught()) {
        // Surface uncaught errors via the isolate.
        iso->ThrowException(tc.Exception());
      }
    }

    void make_event(Isolate* iso, Local<Context> ctx, Local<Object> req, const char* type,
                    Local<Object>& out) {
      auto evt = Object::New(iso);
      set_str(ctx, evt, "type", type);
      (void)evt->Set(ctx, "target"_v8(iso), req);
      out = evt;
    }

    Local<Object> create_request(Isolate* iso, Local<Context> ctx, request_kind kind,
                                 Local<Object> source, Local<Object> transaction) {
      auto& tpl_ref =
          (kind == request_kind::open) ? tpl_for(iso).open_request_tpl : tpl_for(iso).request_tpl;
      auto inst = new_instance_from(iso, ctx, tpl_ref);
      auto* st = new request_state();
      st->kind = kind;
      st->self.Reset(iso, inst);
      st->self.SetWeak(st, request_finalizer, WeakCallbackType::kParameter);
      if (!source.IsEmpty())
        st->source.Reset(iso, source);
      if (!transaction.IsEmpty())
        st->transaction.Reset(iso, transaction);
      set_native(iso, inst, st, TAG_IDB_REQUEST);
      (void)inst->Set(ctx, "result"_v8(iso), Undefined(iso));
      (void)inst->Set(ctx, "error"_v8(iso), Null(iso));
      (void)inst->Set(ctx, "readyState"_v8(iso), "pending"_v8(iso));
      (void)inst->Set(ctx, "source"_v8(iso),
                      source.IsEmpty() ? Local<Value>(Null(iso)) : source.As<Value>());
      (void)inst->Set(ctx, "transaction"_v8(iso),
                      transaction.IsEmpty() ? Local<Value>(Null(iso)) : transaction.As<Value>());
      (void)inst->Set(ctx, "onsuccess"_v8(iso), Null(iso));
      (void)inst->Set(ctx, "onerror"_v8(iso), Null(iso));
      if (kind == request_kind::open) {
        (void)inst->Set(ctx, "onupgradeneeded"_v8(iso), Null(iso));
        (void)inst->Set(ctx, "onblocked"_v8(iso), Null(iso));
      }
      return inst;
    }

    void resolve_request(Isolate* iso, Local<Context> ctx, Local<Object> req, Local<Value> result) {
      (void)req->Set(ctx, "result"_v8(iso), result);
      (void)req->Set(ctx, "readyState"_v8(iso), "done"_v8(iso));
      Local<Object> evt;
      make_event(iso, ctx, req, "success", evt);
      invoke_listener(iso, ctx, req, "onsuccess", evt);
    }

    void reject_request(Isolate* iso, Local<Context> ctx, Local<Object> req, Local<Value> error) {
      (void)req->Set(ctx, "error"_v8(iso), error);
      (void)req->Set(ctx, "readyState"_v8(iso), "done"_v8(iso));
      Local<Object> evt;
      make_event(iso, ctx, req, "error", evt);
      invoke_listener(iso, ctx, req, "onerror", evt);
    }

    // Microtask helper: dispatch in next tick so JS can register listeners.
    struct dispatch_payload {
      Global<Object> req;
      bool success = false;
      Global<Value> result;
      Global<Value> error;
    };

    void dispatch_callback(void* data) {
      auto* p = static_cast<dispatch_payload*>(data);
      auto* iso = Isolate::GetCurrent();
      Isolate::Scope is(iso);
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto req = p->req.Get(iso);
      if (p->success) {
        resolve_request(iso, ctx, req,
                        p->result.IsEmpty() ? Local<Value>(Undefined(iso)) : p->result.Get(iso));
      } else {
        reject_request(iso, ctx, req,
                       p->error.IsEmpty() ? Local<Value>(Undefined(iso)) : p->error.Get(iso));
      }
      delete p;
    }

    void schedule_resolve(Isolate* iso, Local<Object> req, Local<Value> result) {
      auto* p = new dispatch_payload();
      p->req.Reset(iso, req);
      p->success = true;
      p->result.Reset(iso, result);
      iso->EnqueueMicrotask(&dispatch_callback, p);
    }

    void schedule_reject(Isolate* iso, Local<Object> req, Local<Value> err) {
      auto* p = new dispatch_payload();
      p->req.Reset(iso, req);
      p->success = false;
      p->error.Reset(iso, err);
      iso->EnqueueMicrotask(&dispatch_callback, p);
    }

    // ============================== Transaction state ==============================

    struct transaction_state {
      std::shared_ptr<database_state> db;
      std::string mode;      // "readonly" | "readwrite" | "versionchange"
      bool active = false;   // BEGIN issued, no COMMIT/ROLLBACK yet
      bool finished = false; // COMMIT or ROLLBACK done
      bool errored = false;
      std::vector<std::string> store_names;
      Global<Promise::Resolver> done_resolver;
      Global<Object> self;
    };

    void tx_finalizer(const WeakCallbackInfo<transaction_state>& info) {
      auto* st = info.GetParameter();
      if (!st) {
        return;
      }
      if (st->active && !st->finished && st->db && st->db->db) {
        std::string err;
        exec_sql(st->db.get(), "ROLLBACK", err);
      }
      st->self.Reset();
      delete st;
    }

    void fire_tx_event(Isolate* iso, Local<Context> ctx, Local<Object> tx, const char* type) {
      Local<Object> evt;
      make_event(iso, ctx, tx, type, evt);
      char prop[32];
      std::snprintf(prop, sizeof(prop), "on%s", type);
      invoke_listener(iso, ctx, tx, prop, evt);
    }

    transaction_state* unwrap_tx(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 1)
        return nullptr;
      return static_cast<transaction_state*>(unwrap(obj, TAG_IDB_TRANSACTION));
    }

    bool tx_begin(transaction_state* tx, std::string& err) {
      const char* sql = (tx->mode == "readonly")        ? "BEGIN DEFERRED"
                        : (tx->mode == "versionchange") ? "BEGIN EXCLUSIVE"
                                                        : "BEGIN IMMEDIATE";
      if (!exec_sql(tx->db.get(), sql, err))
        return false;
      tx->active = true;
      return true;
    }

    void tx_commit_internal(Isolate* iso, Local<Context> ctx, Local<Object> tx_obj,
                            transaction_state* tx) {
      if (!tx || !tx->active || tx->finished)
        return;
      std::string err;
      if (!exec_sql(tx->db.get(), "COMMIT", err)) {
        tx->errored = true;
        tx->finished = true;
        if (!tx->done_resolver.IsEmpty()) {
          auto r = tx->done_resolver.Get(iso);
          (void)r->Reject(ctx, make_dom_error(iso, "AbortError", err));
        }
        fire_tx_event(iso, ctx, tx_obj, "error");
        fire_tx_event(iso, ctx, tx_obj, "abort");
        return;
      }
      tx->finished = true;
      if (!tx->done_resolver.IsEmpty()) {
        auto r = tx->done_resolver.Get(iso);
        (void)r->Resolve(ctx, Undefined(iso));
      }
      fire_tx_event(iso, ctx, tx_obj, "complete");
    }

    void tx_abort_internal(Isolate* iso, Local<Context> ctx, Local<Object> tx_obj,
                           transaction_state* tx) {
      if (!tx || tx->finished)
        return;
      if (tx->active) {
        std::string err;
        exec_sql(tx->db.get(), "ROLLBACK", err);
      }
      tx->finished = true;
      if (!tx->done_resolver.IsEmpty()) {
        auto r = tx->done_resolver.Get(iso);
        (void)r->Reject(ctx, make_dom_error(iso, "AbortError", "transaction aborted"));
      }
      fire_tx_event(iso, ctx, tx_obj, "abort");
    }

    Local<Object> create_transaction(Isolate* iso, Local<Context> ctx,
                                     std::shared_ptr<database_state> db,
                                     const std::vector<std::string>& store_names,
                                     const std::string& mode) {
      auto inst = new_instance_from(iso, ctx, tpl_for(iso).transaction_tpl);
      auto* st = new transaction_state();
      st->db = std::move(db);
      st->store_names = store_names;
      st->mode = mode;
      st->self.Reset(iso, inst);
      st->self.SetWeak(st, tx_finalizer, WeakCallbackType::kParameter);
      set_native(iso, inst, st, TAG_IDB_TRANSACTION);
      auto names = Array::New(iso, static_cast<int>(store_names.size()));
      for (size_t i = 0; i < store_names.size(); ++i)
        (void)names->Set(ctx, static_cast<uint32_t>(i), s(iso, store_names[i]));
      (void)inst->Set(ctx, "objectStoreNames"_v8(iso), names);
      (void)inst->Set(ctx, "mode"_v8(iso), s(iso, mode));
      (void)inst->Set(ctx, "oncomplete"_v8(iso), Null(iso));
      (void)inst->Set(ctx, "onerror"_v8(iso), Null(iso));
      (void)inst->Set(ctx, "onabort"_v8(iso), Null(iso));
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      st->done_resolver.Reset(iso, resolver);
      (void)inst->Set(ctx, "done"_v8(iso), resolver->GetPromise());
      std::string err;
      if (!tx_begin(st, err)) {
        st->errored = true;
        st->finished = true;
        (void)resolver->Reject(ctx, make_dom_error(iso, "UnknownError", err));
        throw_msg(iso, err);
        return inst;
      }
      return inst;
    }

    // ============================== Object store / Index ==============================

    struct store_handle : weak_holder<store_handle> {
      std::shared_ptr<database_state> db;
      Global<Object> tx; // owning transaction
      std::string name;
      // index mode (when accessed via store.index(...))
      std::string index_name;
      bool is_index = false;
    };

    store_handle* unwrap_store(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 1)
        return nullptr;
      return static_cast<store_handle*>(unwrap(obj, TAG_IDB_STORE));
    }

    Local<Object> create_object_store_handle(Isolate* iso, Local<Context> ctx,
                                             std::shared_ptr<database_state> db,
                                             Local<Object> tx_obj, const std::string& name) {
      auto inst = new_instance_from(iso, ctx, tpl_for(iso).object_store_tpl);
      auto* h = new store_handle();
      h->db = std::move(db);
      h->tx.Reset(iso, tx_obj);
      h->name = name;
      set_native(iso, inst, h, TAG_IDB_STORE);
      h->bind(iso, inst);
      auto& meta = h->db->stores.at(name);
      (void)inst->Set(ctx, "name"_v8(iso), s(iso, name));
      (void)inst->Set(ctx, "keyPath"_v8(iso),
                      meta.key_path ? s(iso, *meta.key_path).As<Value>() : Local<Value>(Null(iso)));
      (void)inst->Set(ctx, "autoIncrement"_v8(iso), Boolean::New(iso, meta.auto_increment));
      auto idx_names = Array::New(iso, static_cast<int>(meta.indexes.size()));
      uint32_t k = 0;
      for (auto& [iname, _] : meta.indexes)
        (void)idx_names->Set(ctx, k++, s(iso, iname));
      (void)inst->Set(ctx, "indexNames"_v8(iso), idx_names);
      (void)inst->Set(ctx, "transaction"_v8(iso), tx_obj);
      return inst;
    }

    Local<Object> create_index_handle(Isolate* iso, Local<Context> ctx,
                                      std::shared_ptr<database_state> db, Local<Object> tx_obj,
                                      const std::string& store, const std::string& index) {
      auto inst = new_instance_from(iso, ctx, tpl_for(iso).index_tpl);
      auto* h = new store_handle();
      h->db = std::move(db);
      h->tx.Reset(iso, tx_obj);
      h->name = store;
      h->index_name = index;
      h->is_index = true;
      set_native(iso, inst, h, TAG_IDB_STORE);
      h->bind(iso, inst);
      auto& smeta = h->db->stores.at(store);
      auto& imeta = smeta.indexes.at(index);
      (void)inst->Set(ctx, "name"_v8(iso), s(iso, index));
      (void)inst->Set(ctx, "keyPath"_v8(iso), s(iso, imeta.key_path));
      (void)inst->Set(ctx, "unique"_v8(iso), Boolean::New(iso, imeta.unique));
      (void)inst->Set(ctx, "multiEntry"_v8(iso), Boolean::New(iso, imeta.multi));
      // objectStore reference
      auto store_obj = create_object_store_handle(iso, ctx, h->db, tx_obj, store);
      (void)inst->Set(ctx, "objectStore"_v8(iso), store_obj);
      return inst;
    }

    // SQL utility: extract the next-key from an auto-increment store.
    int64_t next_auto_key(database_state* db, store_meta& meta) {
      meta.auto_seq += 1;
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db->db, "UPDATE __stores SET auto_seq=?1 WHERE name=?2", -1, &stmt,
                             nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, meta.auto_seq);
        sqlite3_bind_text(stmt, 2, meta.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
      return meta.auto_seq;
    }

    // Index population on put. Builds index rows for any indexes on the store.
    void update_indexes_for_put(database_state* db, store_meta& meta,
                                const std::vector<uint8_t>& pk, Local<Value> value, Isolate* iso,
                                std::string& err) {
      // Delete any existing index rows for this primary key first.
      for (auto& [_, imeta] : meta.indexes) {
        std::string del = "DELETE FROM idx_" + meta.name + "_" + imeta.name + " WHERE pk=?1";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db->db, del.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
          err = sqlite3_errmsg(db->db);
          return;
        }
        bind_blob(stmt, 1, pk);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
          err = sqlite3_errmsg(db->db);
          sqlite3_finalize(stmt);
          return;
        }
        sqlite3_finalize(stmt);
      }
      // Insert fresh rows.
      for (auto& [_, imeta] : meta.indexes) {
        Local<Value> ikv = resolve_key_path(iso, value, imeta.key_path);
        if (ikv.IsEmpty() || ikv->IsUndefined())
          continue; // no value at index path → skip
        std::vector<uint8_t> ikey;
        if (!encode_key(iso, ikv, ikey))
          return; // key encoding threw
        std::string sql =
            "INSERT INTO idx_" + meta.name + "_" + imeta.name + " (ik, pk) VALUES (?1, ?2)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
          err = sqlite3_errmsg(db->db);
          return;
        }
        bind_blob(stmt, 1, ikey);
        bind_blob(stmt, 2, pk);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_CONSTRAINT) {
          if (imeta.unique)
            err = "ConstraintError: index '" + imeta.name + "' uniqueness violation";
          else
            err = sqlite3_errmsg(db->db);
          return;
        }
        if (rc != SQLITE_DONE) {
          err = sqlite3_errmsg(db->db);
          return;
        }
      }
    }

    void delete_indexes_for_pk(database_state* db, store_meta& meta, const std::vector<uint8_t>& pk,
                               std::string& err) {
      for (auto& [_, imeta] : meta.indexes) {
        std::string del = "DELETE FROM idx_" + meta.name + "_" + imeta.name + " WHERE pk=?1";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db->db, del.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
          err = sqlite3_errmsg(db->db);
          return;
        }
        bind_blob(stmt, 1, pk);
        if (sqlite3_step(stmt) != SQLITE_DONE)
          err = sqlite3_errmsg(db->db);
        sqlite3_finalize(stmt);
        if (!err.empty())
          return;
      }
    }

    // ============================== Store ops ==============================
    // Each op:
    //   1. validates state (transaction active, mode allows writes if needed)
    //   2. encodes args
    //   3. runs SQL
    //   4. creates an IDBRequest, schedules resolve/reject in next microtask
    //   5. returns the request

    bool ensure_active_tx(Isolate* iso, store_handle* h) {
      auto tx = h->tx.Get(iso);
      auto* tx_st = unwrap_tx(tx);
      if (!tx_st || tx_st->finished || !tx_st->active) {
        throw_msg(iso, "InvalidStateError: transaction is not active", "InvalidStateError");
        return false;
      }
      return true;
    }

    bool require_writable(Isolate* iso, store_handle* h) {
      auto tx = h->tx.Get(iso);
      auto* tx_st = unwrap_tx(tx);
      if (!tx_st || tx_st->mode == "readonly") {
        throw_msg(iso, "ReadOnlyError: transaction is read-only", "ReadOnlyError");
        return false;
      }
      return true;
    }

    void store_put_or_add(const FunctionCallbackInfo<Value>& info, bool overwrite) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h || h->is_index) {
        throw_msg(iso, "store.put: invalid receiver", "TypeError");
        return;
      }
      if (!ensure_active_tx(iso, h) || !require_writable(iso, h))
        return;
      if (info.Length() < 1) {
        throw_msg(iso, "store.put requires a value", "TypeError");
        return;
      }
      Local<Value> value = info[0];
      auto& meta = h->db->stores.at(h->name);

      std::vector<uint8_t> pk;
      Local<Value> derived_key;
      bool key_from_arg = false;

      if (info.Length() >= 2 && !info[1]->IsUndefined() && !info[1]->IsNull()) {
        if (meta.key_path) {
          throw_msg(iso, "DataError: explicit key not allowed when keyPath is set", "DataError");
          return;
        }
        if (!encode_key(iso, info[1], pk))
          return;
        derived_key = info[1];
        key_from_arg = true;
      } else if (meta.key_path) {
        Local<Value> k = resolve_key_path(iso, value, *meta.key_path);
        if (k.IsEmpty() || k->IsUndefined()) {
          if (!meta.auto_increment) {
            throw_msg(iso, "DataError: keyPath did not resolve to a key", "DataError");
            return;
          }
          int64_t auto_k = next_auto_key(h->db.get(), meta);
          derived_key = Number::New(iso, static_cast<double>(auto_k));
          if (!encode_key(iso, derived_key, pk))
            return;
          if (value->IsObject())
            inject_key_path(iso, value.As<Object>(), *meta.key_path, derived_key);
        } else {
          if (!encode_key(iso, k, pk))
            return;
          derived_key = k;
        }
      } else if (meta.auto_increment) {
        int64_t auto_k = next_auto_key(h->db.get(), meta);
        derived_key = Number::New(iso, static_cast<double>(auto_k));
        if (!encode_key(iso, derived_key, pk))
          return;
      } else {
        throw_msg(iso, "DataError: store has no keyPath and no key was provided", "DataError");
        return;
      }
      (void)key_from_arg;

      // Serialize value.
      std::vector<uint8_t> blob;
      std::string sderr;
      if (!serialize_value(iso, ctx, value, blob, sderr)) {
        throw_msg(iso, "DataCloneError: " + sderr, "DataCloneError");
        return;
      }

      // SQL.
      std::string sql;
      if (overwrite) {
        sql = "INSERT OR REPLACE INTO store_" + h->name + " (k, v) VALUES (?1, ?2)";
      } else {
        sql = "INSERT INTO store_" + h->name + " (k, v) VALUES (?1, ?2)";
      }
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      bind_blob(stmt, 1, pk);
      bind_blob(stmt, 2, blob);
      int rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (rc == SQLITE_CONSTRAINT) {
        auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
        schedule_reject(
            iso, req,
            make_dom_error(iso, "ConstraintError", "ConstraintError: key already exists"));
        info.GetReturnValue().Set(req);
        return;
      }
      if (rc != SQLITE_DONE) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      // Update indexes.
      std::string ierr;
      update_indexes_for_put(h->db.get(), meta, pk, value, iso, ierr);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      if (!ierr.empty())
        schedule_reject(iso, req, make_dom_error(iso, "ConstraintError", ierr));
      else
        schedule_resolve(iso, req,
                         derived_key.IsEmpty() ? Local<Value>(Undefined(iso)) : derived_key);
      info.GetReturnValue().Set(req);
    }

    void store_put(const FunctionCallbackInfo<Value>& info) {
      store_put_or_add(info, true);
    }
    void store_add(const FunctionCallbackInfo<Value>& info) {
      store_put_or_add(info, false);
    }

    // Fetch a single row by key. Returns the deserialised value or undefined.
    void store_get(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h)
        return;
      if (!ensure_active_tx(iso, h))
        return;
      if (info.Length() < 1) {
        throw_msg(iso, "store.get requires a key");
        return;
      }
      key_range range;
      if (!decode_query(iso, info[0], range, false))
        return;

      std::string sql;
      if (h->is_index) {
        sql = "SELECT v FROM store_" + h->name +
              " AS s "
              "INNER JOIN idx_" +
              h->name + "_" + h->index_name + " AS i ON s.k = i.pk WHERE";
      } else {
        sql = "SELECT v FROM store_" + h->name + " WHERE";
      }
      std::string col = h->is_index ? "i.ik" : "k";
      bool first = true;
      auto append_clause = [&](const char* cmp, bool side_lower) {
        sql += first ? " " : " AND ";
        sql += col;
        sql += cmp;
        sql += side_lower ? "?1" : "?2";
        first = false;
      };
      if (range.has_lower)
        append_clause(range.lower_open ? ">" : ">=", true);
      if (range.has_upper && (!range.has_lower || range.lower != range.upper))
        append_clause(range.upper_open ? "<" : "<=", false);
      sql += " ORDER BY ";
      sql += col;
      sql += " ASC LIMIT 1";

      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      stmt_guard sg{stmt};
      if (range.has_lower)
        bind_blob(stmt, 1, range.lower);
      if (range.has_upper && (!range.has_lower || range.lower != range.upper))
        bind_blob(stmt, 2, range.upper);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        size_t len = static_cast<size_t>(sqlite3_column_bytes(stmt, 0));
        Local<Value> val;
        if (deserialize_value(iso, ctx, data, len).ToLocal(&val))
          schedule_resolve(iso, req, val);
        else
          schedule_resolve(iso, req, Undefined(iso));
      } else if (rc == SQLITE_DONE) {
        schedule_resolve(iso, req, Undefined(iso));
      } else {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(h->db->db)));
      }
      info.GetReturnValue().Set(req);
    }

    void store_get_key(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h)
        return;
      if (!ensure_active_tx(iso, h))
        return;
      if (info.Length() < 1) {
        throw_msg(iso, "store.getKey requires a key");
        return;
      }
      key_range range;
      if (!decode_query(iso, info[0], range, false))
        return;
      std::string sql;
      if (h->is_index) {
        sql = "SELECT pk FROM idx_" + h->name + "_" + h->index_name + " WHERE";
      } else {
        sql = "SELECT k FROM store_" + h->name + " WHERE";
      }
      std::string col = h->is_index ? "ik" : "k";
      bool first = true;
      auto add = [&](const char* cmp, bool lower) {
        sql += first ? " " : " AND ";
        sql += col;
        sql += cmp;
        sql += lower ? "?1" : "?2";
        first = false;
      };
      if (range.has_lower)
        add(range.lower_open ? ">" : ">=", true);
      if (range.has_upper && (!range.has_lower || range.lower != range.upper))
        add(range.upper_open ? "<" : "<=", false);
      sql += " ORDER BY ";
      sql += col;
      sql += " ASC LIMIT 1";

      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      stmt_guard sg{stmt};
      if (range.has_lower)
        bind_blob(stmt, 1, range.lower);
      if (range.has_upper && (!range.has_lower || range.lower != range.upper))
        bind_blob(stmt, 2, range.upper);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        size_t len = static_cast<size_t>(sqlite3_column_bytes(stmt, 0));
        schedule_resolve(iso, req, decode_key(iso, data, len));
      } else if (rc == SQLITE_DONE) {
        schedule_resolve(iso, req, Undefined(iso));
      } else {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(h->db->db)));
      }
      info.GetReturnValue().Set(req);
    }

    void store_get_all_impl(const FunctionCallbackInfo<Value>& info, bool keys_only) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h)
        return;
      if (!ensure_active_tx(iso, h))
        return;
      key_range range;
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (!decode_query(iso, info[0], range, true))
          return;
      }
      int64_t limit = -1;
      if (info.Length() >= 2 && info[1]->IsNumber())
        limit = std::max<int64_t>(0, info[1]->IntegerValue(ctx).FromMaybe(-1));

      std::string sql;
      std::string col = h->is_index ? "ik" : "k";
      if (h->is_index && !keys_only) {
        sql = "SELECT s.v FROM store_" + h->name + " AS s INNER JOIN idx_" + h->name + "_" +
              h->index_name + " AS i ON s.k = i.pk WHERE 1=1";
      } else if (h->is_index && keys_only) {
        sql = "SELECT pk FROM idx_" + h->name + "_" + h->index_name + " WHERE 1=1";
      } else if (!h->is_index && !keys_only) {
        sql = "SELECT v FROM store_" + h->name + " WHERE 1=1";
      } else {
        sql = "SELECT k FROM store_" + h->name + " WHERE 1=1";
      }
      std::string fcol = h->is_index ? "i.ik" : col;
      if (range.has_lower) {
        sql += " AND ";
        sql += fcol;
        sql += range.lower_open ? " > ?1" : " >= ?1";
      }
      if (range.has_upper) {
        sql += " AND ";
        sql += fcol;
        sql += range.upper_open ? " < ?2" : " <= ?2";
      }
      sql += " ORDER BY ";
      sql += fcol;
      sql += " ASC";
      if (limit >= 0)
        sql += " LIMIT " + std::to_string(limit);

      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      stmt_guard sg{stmt};
      if (range.has_lower)
        bind_blob(stmt, 1, range.lower);
      if (range.has_upper)
        bind_blob(stmt, 2, range.upper);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      auto arr = Array::New(iso);
      uint32_t i = 0;
      int rc;
      while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        size_t len = static_cast<size_t>(sqlite3_column_bytes(stmt, 0));
        Local<Value> v;
        if (keys_only)
          v = decode_key(iso, data, len);
        else if (!deserialize_value(iso, ctx, data, len).ToLocal(&v))
          v = Undefined(iso);
        (void)arr->Set(ctx, i++, v);
      }
      if (rc != SQLITE_DONE) {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(h->db->db)));
      } else {
        schedule_resolve(iso, req, arr);
      }
      info.GetReturnValue().Set(req);
    }
    void store_get_all(const FunctionCallbackInfo<Value>& info) {
      store_get_all_impl(info, false);
    }
    void store_get_all_keys(const FunctionCallbackInfo<Value>& info) {
      store_get_all_impl(info, true);
    }

    void store_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h)
        return;
      if (!ensure_active_tx(iso, h))
        return;
      key_range range;
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (!decode_query(iso, info[0], range, true))
          return;
      }
      std::string sql;
      std::string col = h->is_index ? "ik" : "k";
      if (h->is_index) {
        sql = "SELECT COUNT(*) FROM idx_" + h->name + "_" + h->index_name + " WHERE 1=1";
      } else {
        sql = "SELECT COUNT(*) FROM store_" + h->name + " WHERE 1=1";
      }
      if (range.has_lower) {
        sql += " AND ";
        sql += col;
        sql += range.lower_open ? " > ?1" : " >= ?1";
      }
      if (range.has_upper) {
        sql += " AND ";
        sql += col;
        sql += range.upper_open ? " < ?2" : " <= ?2";
      }
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      stmt_guard sg{stmt};
      if (range.has_lower)
        bind_blob(stmt, 1, range.lower);
      if (range.has_upper)
        bind_blob(stmt, 2, range.upper);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        int64_t n = sqlite3_column_int64(stmt, 0);
        schedule_resolve(iso, req, Number::New(iso, static_cast<double>(n)));
      } else {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(h->db->db)));
      }
      info.GetReturnValue().Set(req);
    }

    void store_delete(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h || h->is_index) {
        throw_msg(iso, "store.delete: invalid receiver", "TypeError");
        return;
      }
      if (!ensure_active_tx(iso, h) || !require_writable(iso, h))
        return;
      if (info.Length() < 1) {
        throw_msg(iso, "store.delete requires a key", "TypeError");
        return;
      }
      key_range range;
      if (!decode_query(iso, info[0], range, false))
        return;
      // For each matching pk: delete index rows then delete value row.
      auto& meta = h->db->stores.at(h->name);
      std::string sel = "SELECT k FROM store_" + h->name + " WHERE 1=1";
      if (range.has_lower)
        sel += range.lower_open ? " AND k > ?1" : " AND k >= ?1";
      if (range.has_upper)
        sel += range.upper_open ? " AND k < ?2" : " AND k <= ?2";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sel.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      stmt_guard sg{stmt};
      if (range.has_lower)
        bind_blob(stmt, 1, range.lower);
      if (range.has_upper)
        bind_blob(stmt, 2, range.upper);
      std::vector<std::vector<uint8_t>> pks;
      int rc;
      while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        size_t len = static_cast<size_t>(sqlite3_column_bytes(stmt, 0));
        pks.emplace_back(data, data + len);
      }
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      if (rc != SQLITE_DONE) {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(h->db->db)));
        info.GetReturnValue().Set(req);
        return;
      }
      for (auto& pk : pks) {
        std::string ierr;
        delete_indexes_for_pk(h->db.get(), meta, pk, ierr);
        if (!ierr.empty()) {
          schedule_reject(iso, req, make_dom_error(iso, "UnknownError", ierr));
          info.GetReturnValue().Set(req);
          return;
        }
        std::string del = "DELETE FROM store_" + h->name + " WHERE k=?1";
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(h->db->db, del.c_str(), -1, &d, nullptr) != SQLITE_OK) {
          schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(h->db->db)));
          info.GetReturnValue().Set(req);
          return;
        }
        bind_blob(d, 1, pk);
        sqlite3_step(d);
        sqlite3_finalize(d);
      }
      schedule_resolve(iso, req, Undefined(iso));
      info.GetReturnValue().Set(req);
    }

    void store_clear(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h || h->is_index) {
        throw_msg(iso, "store.clear: invalid receiver", "TypeError");
        return;
      }
      if (!ensure_active_tx(iso, h) || !require_writable(iso, h))
        return;
      auto& meta = h->db->stores.at(h->name);
      std::string err;
      for (auto& [_, imeta] : meta.indexes) {
        std::string del = "DELETE FROM idx_" + meta.name + "_" + imeta.name;
        if (!exec_sql(h->db.get(), del.c_str(), err))
          break;
      }
      if (err.empty()) {
        std::string del = "DELETE FROM store_" + h->name;
        exec_sql(h->db.get(), del.c_str(), err);
      }
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      if (err.empty())
        schedule_resolve(iso, req, Undefined(iso));
      else
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", err));
      info.GetReturnValue().Set(req);
    }

    // ============================== createIndex / deleteIndex ==============================

    void store_create_index(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h || h->is_index) {
        throw_msg(iso, "store.createIndex: invalid receiver", "TypeError");
        return;
      }
      auto tx = h->tx.Get(iso);
      auto* tx_st = unwrap_tx(tx);
      if (!tx_st || tx_st->mode != "versionchange") {
        throw_msg(iso, "InvalidStateError: createIndex requires versionchange transaction",
                  "InvalidStateError");
        return;
      }
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        throw_msg(iso, "store.createIndex(name, keyPath, opts?)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      std::string key_path = utf8(iso, info[1]);
      bool unique = false;
      bool multi = false;
      if (info.Length() >= 3 && info[2]->IsObject()) {
        auto opts = info[2].As<Object>();
        Local<Value> v;
        if (opts->Get(ctx, "unique"_v8(iso)).ToLocal(&v))
          unique = v->BooleanValue(iso);
        if (opts->Get(ctx, "multiEntry"_v8(iso)).ToLocal(&v))
          multi = v->BooleanValue(iso);
        (void)multi; // multiEntry deferred to v2
      }

      auto& meta = h->db->stores.at(h->name);
      if (meta.indexes.count(name)) {
        throw_msg(iso, "ConstraintError: index already exists", "ConstraintError");
        return;
      }

      // Create index table + record metadata.
      std::string ddl = "CREATE TABLE IF NOT EXISTS idx_" + h->name + "_" + name +
                        " (ik BLOB, pk BLOB, PRIMARY KEY(ik, pk))";
      std::string err;
      if (!exec_sql(h->db.get(), ddl.c_str(), err)) {
        throw_msg(iso, err);
        return;
      }
      sqlite3_stmt* ins = nullptr;
      sqlite3_prepare_v2(h->db->db,
                         "INSERT INTO __indexes(store, name, key_path, is_unique, is_multi) "
                         "VALUES (?1, ?2, ?3, ?4, ?5)",
                         -1, &ins, nullptr);
      sqlite3_bind_text(ins, 1, h->name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 2, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 3, key_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(ins, 4, unique ? 1 : 0);
      sqlite3_bind_int(ins, 5, multi ? 1 : 0);
      if (sqlite3_step(ins) != SQLITE_DONE) {
        sqlite3_finalize(ins);
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      sqlite3_finalize(ins);

      // Backfill from existing rows.
      index_meta im;
      im.name = name;
      im.key_path = key_path;
      im.unique = unique;
      im.multi = multi;
      meta.indexes[name] = im;

      sqlite3_stmt* sel = nullptr;
      std::string sel_sql = "SELECT k, v FROM store_" + h->name;
      if (sqlite3_prepare_v2(h->db->db, sel_sql.c_str(), -1, &sel, nullptr) == SQLITE_OK) {
        stmt_guard sg{sel};
        std::string ins_sql =
            "INSERT INTO idx_" + h->name + "_" + name + " (ik, pk) VALUES (?1, ?2)";
        int rc;
        while ((rc = sqlite3_step(sel)) == SQLITE_ROW) {
          const auto* pkdata = static_cast<const uint8_t*>(sqlite3_column_blob(sel, 0));
          size_t pklen = static_cast<size_t>(sqlite3_column_bytes(sel, 0));
          const auto* vdata = static_cast<const uint8_t*>(sqlite3_column_blob(sel, 1));
          size_t vlen = static_cast<size_t>(sqlite3_column_bytes(sel, 1));
          Local<Value> val;
          if (!deserialize_value(iso, ctx, vdata, vlen).ToLocal(&val))
            continue;
          Local<Value> ikv = resolve_key_path(iso, val, key_path);
          if (ikv.IsEmpty() || ikv->IsUndefined())
            continue;
          std::vector<uint8_t> ikey;
          if (!encode_key(iso, ikv, ikey))
            return;
          sqlite3_stmt* idx_ins = nullptr;
          if (sqlite3_prepare_v2(h->db->db, ins_sql.c_str(), -1, &idx_ins, nullptr) != SQLITE_OK)
            continue;
          bind_blob(idx_ins, 1, ikey);
          std::vector<uint8_t> pk_vec(pkdata, pkdata + pklen);
          bind_blob(idx_ins, 2, pk_vec);
          int irc = sqlite3_step(idx_ins);
          sqlite3_finalize(idx_ins);
          if (irc == SQLITE_CONSTRAINT && unique) {
            // Roll back the in-progress index addition.
            std::string drop = "DROP TABLE idx_" + h->name + "_" + name;
            std::string drop_err;
            exec_sql(h->db.get(), drop.c_str(), drop_err);
            sqlite3_stmt* del = nullptr;
            sqlite3_prepare_v2(h->db->db, "DELETE FROM __indexes WHERE store=?1 AND name=?2", -1,
                               &del, nullptr);
            sqlite3_bind_text(del, 1, h->name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(del, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
            sqlite3_finalize(del);
            meta.indexes.erase(name);
            throw_msg(iso, "ConstraintError: unique index violation during backfill",
                      "ConstraintError");
            return;
          }
        }
      }

      auto idx_obj = create_index_handle(iso, ctx, h->db, h->tx.Get(iso), h->name, name);
      info.GetReturnValue().Set(idx_obj);
    }

    void store_delete_index(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_store(info.This());
      if (!h || h->is_index) {
        throw_msg(iso, "store.deleteIndex: invalid receiver", "TypeError");
        return;
      }
      auto tx = h->tx.Get(iso);
      auto* tx_st = unwrap_tx(tx);
      if (!tx_st || tx_st->mode != "versionchange") {
        throw_msg(iso, "InvalidStateError: deleteIndex requires versionchange transaction",
                  "InvalidStateError");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "store.deleteIndex(name)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      auto& meta = h->db->stores.at(h->name);
      if (!meta.indexes.count(name)) {
        throw_msg(iso, "NotFoundError: index does not exist", "NotFoundError");
        return;
      }
      std::string drop = "DROP TABLE idx_" + h->name + "_" + name;
      std::string err;
      exec_sql(h->db.get(), drop.c_str(), err);
      sqlite3_stmt* del = nullptr;
      sqlite3_prepare_v2(h->db->db, "DELETE FROM __indexes WHERE store=?1 AND name=?2", -1, &del,
                         nullptr);
      sqlite3_bind_text(del, 1, h->name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(del, 2, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(del);
      sqlite3_finalize(del);
      meta.indexes.erase(name);
    }

    void store_index_method(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h || h->is_index) {
        throw_msg(iso, "store.index: invalid receiver", "TypeError");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "store.index(name)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      auto& meta = h->db->stores.at(h->name);
      if (!meta.indexes.count(name)) {
        throw_msg(iso, "NotFoundError: index does not exist", "NotFoundError");
        return;
      }
      info.GetReturnValue().Set(
          create_index_handle(iso, ctx, h->db, h->tx.Get(iso), h->name, name));
    }

    // ============================== Cursors ==============================

    struct cursor_state : weak_holder<cursor_state> {
      std::shared_ptr<database_state> db;
      Global<Object> source; // store or index
      Global<Object> tx;
      Global<Object> request; // the IDBRequest that opened the cursor
      sqlite3_stmt* stmt = nullptr;
      bool has_value = false; // cursor is IDBCursorWithValue
      bool exhausted = false;
      bool is_index = false;
      std::string store_name;
      std::string index_name;
      std::vector<uint8_t> last_pk; // for update/delete
      std::vector<uint8_t> last_ik; // for index cursors

      void on_finalize(Isolate*) {
        if (stmt)
          sqlite3_finalize(stmt);
      }
    };

    cursor_state* unwrap_cursor(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 1)
        return nullptr;
      return static_cast<cursor_state*>(unwrap(obj, TAG_IDB_CURSOR));
    }

    // Step the cursor; on row, sets the JS-visible key/primaryKey/value props
    // on the cursor object and resolves the request with the cursor; on done,
    // resolves with null.
    void cursor_step(Isolate* iso, Local<Context> ctx, Local<Object> cursor_obj, cursor_state* st) {
      if (st->exhausted || !st->stmt) {
        auto req = st->request.Get(iso);
        schedule_resolve(iso, req, Null(iso));
        return;
      }
      int rc = sqlite3_step(st->stmt);
      auto req = st->request.Get(iso);
      if (rc == SQLITE_ROW) {
        const auto* col0 = static_cast<const uint8_t*>(sqlite3_column_blob(st->stmt, 0));
        size_t col0_len = static_cast<size_t>(sqlite3_column_bytes(st->stmt, 0));
        if (st->is_index) {
          // columns: ik, pk, v
          const auto* col1 = static_cast<const uint8_t*>(sqlite3_column_blob(st->stmt, 1));
          size_t col1_len = static_cast<size_t>(sqlite3_column_bytes(st->stmt, 1));
          st->last_ik.assign(col0, col0 + col0_len);
          st->last_pk.assign(col1, col1 + col1_len);
          (void)cursor_obj->Set(ctx, "key"_v8(iso), decode_key(iso, col0, col0_len));
          (void)cursor_obj->Set(ctx, "primaryKey"_v8(iso), decode_key(iso, col1, col1_len));
          if (st->has_value) {
            const auto* vdata = static_cast<const uint8_t*>(sqlite3_column_blob(st->stmt, 2));
            size_t vlen = static_cast<size_t>(sqlite3_column_bytes(st->stmt, 2));
            Local<Value> v;
            if (!deserialize_value(iso, ctx, vdata, vlen).ToLocal(&v))
              v = Undefined(iso);
            (void)cursor_obj->Set(ctx, "value"_v8(iso), v);
          }
        } else {
          // columns: k (and v for with-value)
          st->last_pk.assign(col0, col0 + col0_len);
          (void)cursor_obj->Set(ctx, "key"_v8(iso), decode_key(iso, col0, col0_len));
          (void)cursor_obj->Set(ctx, "primaryKey"_v8(iso), decode_key(iso, col0, col0_len));
          if (st->has_value) {
            const auto* vdata = static_cast<const uint8_t*>(sqlite3_column_blob(st->stmt, 1));
            size_t vlen = static_cast<size_t>(sqlite3_column_bytes(st->stmt, 1));
            Local<Value> v;
            if (!deserialize_value(iso, ctx, vdata, vlen).ToLocal(&v))
              v = Undefined(iso);
            (void)cursor_obj->Set(ctx, "value"_v8(iso), v);
          }
        }
        schedule_resolve(iso, req, cursor_obj);
      } else {
        st->exhausted = true;
        sqlite3_finalize(st->stmt);
        st->stmt = nullptr;
        schedule_resolve(iso, req, Null(iso));
      }
    }

    void cursor_continue(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* st = unwrap_cursor(info.This());
      if (!st) {
        throw_msg(iso, "cursor.continue: invalid receiver", "TypeError");
        return;
      }
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        throw_msg(
            iso,
            "cursor.continue(key) is not supported in v1; use store.openCursor with a IDBKeyRange",
            "NotSupportedError");
        return;
      }
      cursor_step(iso, ctx, info.This(), st);
    }

    void cursor_advance(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* st = unwrap_cursor(info.This());
      if (!st) {
        throw_msg(iso, "cursor.advance: invalid receiver", "TypeError");
        return;
      }
      int n = info.Length() >= 1 ? info[0]->Int32Value(ctx).FromMaybe(0) : 0;
      if (n <= 0) {
        throw_msg(iso, "cursor.advance(count) requires count > 0", "TypeError");
        return;
      }
      for (int i = 0; i < n - 1 && !st->exhausted; ++i) {
        if (sqlite3_step(st->stmt) != SQLITE_ROW) {
          st->exhausted = true;
          break;
        }
      }
      cursor_step(iso, ctx, info.This(), st);
    }

    void cursor_update(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* st = unwrap_cursor(info.This());
      if (!st || st->exhausted) {
        throw_msg(iso, "cursor.update: invalid receiver or no current row", "InvalidStateError");
        return;
      }
      auto tx_st = unwrap_tx(st->tx.Get(iso));
      if (!tx_st || tx_st->mode == "readonly") {
        throw_msg(iso, "ReadOnlyError: cursor.update requires writable transaction",
                  "ReadOnlyError");
        return;
      }
      if (info.Length() < 1) {
        throw_msg(iso, "cursor.update requires a value", "TypeError");
        return;
      }
      Local<Value> value = info[0];
      auto& meta = st->db->stores.at(st->store_name);
      // Re-derive key from keyPath if set; must match cursor's primaryKey.
      if (meta.key_path) {
        Local<Value> derived = resolve_key_path(iso, value, *meta.key_path);
        std::vector<uint8_t> dk;
        if (derived.IsEmpty() || derived->IsUndefined() || !encode_key(iso, derived, dk) ||
            dk != st->last_pk) {
          throw_msg(iso, "DataError: cursor.update value's key must match current primaryKey",
                    "DataError");
          return;
        }
      }
      std::vector<uint8_t> blob;
      std::string sderr;
      if (!serialize_value(iso, ctx, value, blob, sderr)) {
        throw_msg(iso, "DataCloneError: " + sderr, "DataCloneError");
        return;
      }
      std::string sql = "UPDATE store_" + st->store_name + " SET v=?1 WHERE k=?2";
      sqlite3_stmt* upd = nullptr;
      if (sqlite3_prepare_v2(st->db->db, sql.c_str(), -1, &upd, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(st->db->db));
        return;
      }
      bind_blob(upd, 1, blob);
      bind_blob(upd, 2, st->last_pk);
      int rc = sqlite3_step(upd);
      sqlite3_finalize(upd);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), st->tx.Get(iso));
      if (rc != SQLITE_DONE) {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(st->db->db)));
        info.GetReturnValue().Set(req);
        return;
      }
      std::string ierr;
      update_indexes_for_put(st->db.get(), meta, st->last_pk, value, iso, ierr);
      if (!ierr.empty())
        schedule_reject(iso, req, make_dom_error(iso, "ConstraintError", ierr));
      else
        schedule_resolve(iso, req, decode_key(iso, st->last_pk.data(), st->last_pk.size()));
      info.GetReturnValue().Set(req);
    }

    void cursor_delete(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* st = unwrap_cursor(info.This());
      if (!st || st->exhausted) {
        throw_msg(iso, "cursor.delete: invalid receiver or no current row", "InvalidStateError");
        return;
      }
      auto tx_st = unwrap_tx(st->tx.Get(iso));
      if (!tx_st || tx_st->mode == "readonly") {
        throw_msg(iso, "ReadOnlyError: cursor.delete requires writable transaction",
                  "ReadOnlyError");
        return;
      }
      auto& meta = st->db->stores.at(st->store_name);
      std::string ierr;
      delete_indexes_for_pk(st->db.get(), meta, st->last_pk, ierr);
      auto req = create_request(iso, ctx, request_kind::generic, info.This(), st->tx.Get(iso));
      if (!ierr.empty()) {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", ierr));
        info.GetReturnValue().Set(req);
        return;
      }
      std::string sql = "DELETE FROM store_" + st->store_name + " WHERE k=?1";
      sqlite3_stmt* del = nullptr;
      if (sqlite3_prepare_v2(st->db->db, sql.c_str(), -1, &del, nullptr) != SQLITE_OK) {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(st->db->db)));
        info.GetReturnValue().Set(req);
        return;
      }
      bind_blob(del, 1, st->last_pk);
      int rc = sqlite3_step(del);
      sqlite3_finalize(del);
      if (rc != SQLITE_DONE)
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", sqlite3_errmsg(st->db->db)));
      else
        schedule_resolve(iso, req, Undefined(iso));
      info.GetReturnValue().Set(req);
    }

    void store_open_cursor_impl(const FunctionCallbackInfo<Value>& info, bool keys_only) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_store(info.This());
      if (!h)
        return;
      if (!ensure_active_tx(iso, h))
        return;
      key_range range;
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (!decode_query(iso, info[0], range, true))
          return;
      }
      // direction arg: 'next'|'prev' (we honor 'next' / 'nextunique' as forward asc;
      // 'prev' / 'prevunique' as descending).
      bool descending = false;
      if (info.Length() >= 2 && info[1]->IsString()) {
        std::string dir = utf8(iso, info[1]);
        if (dir == "prev" || dir == "prevunique")
          descending = true;
      }

      std::string sql;
      std::string col = h->is_index ? "ik" : "k";
      if (h->is_index) {
        if (keys_only) {
          sql = "SELECT ik, pk FROM idx_" + h->name + "_" + h->index_name + " WHERE 1=1";
        } else {
          sql = "SELECT i.ik, i.pk, s.v FROM idx_" + h->name + "_" + h->index_name +
                " AS i INNER JOIN store_" + h->name + " AS s ON s.k = i.pk WHERE 1=1";
        }
      } else {
        if (keys_only) {
          sql = "SELECT k FROM store_" + h->name + " WHERE 1=1";
        } else {
          sql = "SELECT k, v FROM store_" + h->name + " WHERE 1=1";
        }
      }
      std::string fcol = h->is_index ? "i.ik" : col;
      if (range.has_lower) {
        sql += " AND ";
        sql += fcol;
        sql += range.lower_open ? " > ?1" : " >= ?1";
      }
      if (range.has_upper) {
        sql += " AND ";
        sql += fcol;
        sql += range.upper_open ? " < ?2" : " <= ?2";
      }
      sql += " ORDER BY ";
      sql += fcol;
      sql += descending ? " DESC" : " ASC";

      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(h->db->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw_msg(iso, sqlite3_errmsg(h->db->db));
        return;
      }
      if (range.has_lower)
        bind_blob(stmt, 1, range.lower);
      if (range.has_upper)
        bind_blob(stmt, 2, range.upper);

      auto& tpl_ref = keys_only ? tpl_for(iso).cursor_tpl : tpl_for(iso).cursor_with_value_tpl;
      auto cursor = new_instance_from(iso, ctx, tpl_ref);
      auto* st = new cursor_state();
      st->db = h->db;
      st->stmt = stmt;
      st->has_value = !keys_only;
      st->is_index = h->is_index;
      st->store_name = h->name;
      st->index_name = h->index_name;
      st->source.Reset(iso, info.This());
      st->tx.Reset(iso, h->tx.Get(iso));
      set_native(iso, cursor, st, TAG_IDB_CURSOR);
      st->bind(iso, cursor);
      (void)cursor->Set(ctx, "source"_v8(iso), info.This());
      (void)cursor->Set(ctx, "direction"_v8(iso), s(iso, descending ? "prev" : "next"));
      (void)cursor->Set(ctx, "key"_v8(iso), Null(iso));
      (void)cursor->Set(ctx, "primaryKey"_v8(iso), Null(iso));
      if (!keys_only)
        (void)cursor->Set(ctx, "value"_v8(iso), Undefined(iso));

      auto req = create_request(iso, ctx, request_kind::generic, info.This(), h->tx.Get(iso));
      st->request.Reset(iso, req);
      cursor_step(iso, ctx, cursor, st);
      info.GetReturnValue().Set(req);
    }
    void store_open_cursor(const FunctionCallbackInfo<Value>& info) {
      store_open_cursor_impl(info, false);
    }
    void store_open_key_cursor(const FunctionCallbackInfo<Value>& info) {
      store_open_cursor_impl(info, true);
    }

    // ============================== IDBTransaction methods ==============================

    void tx_object_store(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* tx = unwrap_tx(info.This());
      if (!tx) {
        throw_msg(iso, "tx.objectStore: invalid receiver", "TypeError");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "tx.objectStore(name)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      bool found = false;
      for (auto& sn : tx->store_names)
        if (sn == name) {
          found = true;
          break;
        }
      if (!found) {
        throw_msg(iso, "NotFoundError: store not in transaction's scope", "NotFoundError");
        return;
      }
      if (!tx->db->stores.count(name)) {
        throw_msg(iso, "NotFoundError: store does not exist", "NotFoundError");
        return;
      }
      info.GetReturnValue().Set(create_object_store_handle(iso, ctx, tx->db, info.This(), name));
    }

    void tx_commit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* tx = unwrap_tx(info.This());
      if (!tx)
        return;
      tx_commit_internal(iso, ctx, info.This(), tx);
    }

    void tx_abort(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* tx = unwrap_tx(info.This());
      if (!tx)
        return;
      tx_abort_internal(iso, ctx, info.This(), tx);
    }

    // ============================== IDBDatabase methods ==============================

    struct database_handle : weak_holder<database_handle> {
      std::shared_ptr<database_state> db;

      void on_finalize(Isolate*) {
        if (!db)
          return;
        db->open_connections -= 1;
        if (db->open_connections <= 0 && db->closed && db->db) {
          sqlite3_close(db->db);
          db->db = nullptr;
        }
      }
    };

    database_handle* unwrap_database(Local<Object> obj) {
      if (obj.IsEmpty() || obj->InternalFieldCount() < 1)
        return nullptr;
      return static_cast<database_handle*>(unwrap(obj, TAG_IDB_DATABASE));
    }

    Local<Object> create_database_handle(Isolate* iso, Local<Context> ctx,
                                         std::shared_ptr<database_state> db) {
      auto inst = new_instance_from(iso, ctx, tpl_for(iso).database_tpl);
      auto* h = new database_handle();
      h->db = db;
      h->db->open_connections += 1;
      set_native(iso, inst, h, TAG_IDB_DATABASE);
      h->bind(iso, inst);
      (void)inst->Set(ctx, "name"_v8(iso), s(iso, db->name));
      (void)inst->Set(ctx, "version"_v8(iso), Number::New(iso, static_cast<double>(db->version)));
      auto names = Array::New(iso);
      uint32_t i = 0;
      for (auto& [k, _] : db->stores)
        (void)names->Set(ctx, i++, s(iso, k));
      (void)inst->Set(ctx, "objectStoreNames"_v8(iso), names);
      (void)inst->Set(ctx, "onversionchange"_v8(iso), Null(iso));
      (void)inst->Set(ctx, "onclose"_v8(iso), Null(iso));
      return inst;
    }

    // The active transaction is established here. Within versionchange we run
    // user's onupgradeneeded synchronously so they can createObjectStore etc.
    void database_create_object_store(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_database(info.This());
      if (!h) {
        throw_msg(iso, "db.createObjectStore: invalid receiver", "TypeError");
        return;
      }
      // Look up the active versionchange tx via Symbol-keyed property.
      auto active_tx = info.This()->Get(ctx, "__fxe_active_tx"_v8(iso));
      if (active_tx.IsEmpty() || !active_tx.ToLocalChecked()->IsObject()) {
        throw_msg(iso, "InvalidStateError: createObjectStore requires versionchange transaction",
                  "InvalidStateError");
        return;
      }
      auto tx_obj = active_tx.ToLocalChecked().As<Object>();
      auto* tx_st = unwrap_tx(tx_obj);
      if (!tx_st || tx_st->mode != "versionchange") {
        throw_msg(iso, "InvalidStateError: createObjectStore requires versionchange transaction",
                  "InvalidStateError");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "db.createObjectStore(name, opts?)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      if (h->db->stores.count(name)) {
        throw_msg(iso, "ConstraintError: object store already exists", "ConstraintError");
        return;
      }
      std::optional<std::string> key_path;
      bool auto_inc = false;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto opts = info[1].As<Object>();
        Local<Value> v;
        if (opts->Get(ctx, "keyPath"_v8(iso)).ToLocal(&v)) {
          if (v->IsString())
            key_path = utf8(iso, v);
          else if (v->IsArray())
            // Compound key paths not supported in v1.
            ;
        }
        if (opts->Get(ctx, "autoIncrement"_v8(iso)).ToLocal(&v))
          auto_inc = v->BooleanValue(iso);
      }
      // SQL: create store table + meta row.
      std::string ddl = "CREATE TABLE store_" + name + " (k BLOB PRIMARY KEY, v BLOB NOT NULL)";
      std::string err;
      if (!exec_sql(h->db.get(), ddl.c_str(), err)) {
        throw_msg(iso, err);
        return;
      }
      sqlite3_stmt* ins = nullptr;
      sqlite3_prepare_v2(
          h->db->db,
          "INSERT INTO __stores(name, key_path, auto_increment, auto_seq) VALUES (?1, ?2, ?3, 0)",
          -1, &ins, nullptr);
      sqlite3_bind_text(ins, 1, name.c_str(), -1, SQLITE_TRANSIENT);
      if (key_path)
        sqlite3_bind_text(ins, 2, key_path->c_str(), -1, SQLITE_TRANSIENT);
      else
        sqlite3_bind_null(ins, 2);
      sqlite3_bind_int(ins, 3, auto_inc ? 1 : 0);
      sqlite3_step(ins);
      sqlite3_finalize(ins);

      store_meta meta;
      meta.name = name;
      meta.key_path = key_path;
      meta.auto_increment = auto_inc;
      h->db->stores[name] = std::move(meta);

      // Update objectStoreNames on the database instance.
      auto names = Array::New(iso);
      uint32_t i = 0;
      for (auto& [k, _] : h->db->stores)
        (void)names->Set(ctx, i++, s(iso, k));
      (void)info.This()->Set(ctx, "objectStoreNames"_v8(iso), names);

      info.GetReturnValue().Set(create_object_store_handle(iso, ctx, h->db, tx_obj, name));
    }

    void database_delete_object_store(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_database(info.This());
      if (!h)
        return;
      auto active_tx = info.This()->Get(ctx, "__fxe_active_tx"_v8(iso));
      if (active_tx.IsEmpty() || !active_tx.ToLocalChecked()->IsObject()) {
        throw_msg(iso, "InvalidStateError: deleteObjectStore requires versionchange transaction",
                  "InvalidStateError");
        return;
      }
      auto tx_obj = active_tx.ToLocalChecked().As<Object>();
      auto* tx_st = unwrap_tx(tx_obj);
      if (!tx_st || tx_st->mode != "versionchange") {
        throw_msg(iso, "InvalidStateError: deleteObjectStore requires versionchange transaction",
                  "InvalidStateError");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "db.deleteObjectStore(name)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      if (!h->db->stores.count(name)) {
        throw_msg(iso, "NotFoundError: store does not exist", "NotFoundError");
        return;
      }
      auto& meta = h->db->stores[name];
      std::string err;
      for (auto& [_, imeta] : meta.indexes) {
        std::string drop = "DROP TABLE idx_" + name + "_" + imeta.name;
        exec_sql(h->db.get(), drop.c_str(), err);
      }
      std::string drop = "DROP TABLE store_" + name;
      exec_sql(h->db.get(), drop.c_str(), err);
      sqlite3_stmt* del = nullptr;
      sqlite3_prepare_v2(h->db->db, "DELETE FROM __stores WHERE name=?1", -1, &del, nullptr);
      sqlite3_bind_text(del, 1, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(del);
      sqlite3_finalize(del);
      sqlite3_stmt* del2 = nullptr;
      sqlite3_prepare_v2(h->db->db, "DELETE FROM __indexes WHERE store=?1", -1, &del2, nullptr);
      sqlite3_bind_text(del2, 1, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(del2);
      sqlite3_finalize(del2);
      h->db->stores.erase(name);
      auto names = Array::New(iso);
      uint32_t i = 0;
      for (auto& [k, _] : h->db->stores)
        (void)names->Set(ctx, i++, s(iso, k));
      (void)info.This()->Set(ctx, "objectStoreNames"_v8(iso), names);
    }

    void database_transaction(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_database(info.This());
      if (!h) {
        throw_msg(iso, "db.transaction: invalid receiver", "TypeError");
        return;
      }
      if (h->db->closed) {
        throw_msg(iso, "InvalidStateError: database is closed", "InvalidStateError");
        return;
      }
      if (info.Length() < 1) {
        throw_msg(iso, "db.transaction(stores, mode?)", "TypeError");
        return;
      }
      std::vector<std::string> store_names;
      if (info[0]->IsString()) {
        store_names.push_back(utf8(iso, info[0]));
      } else if (info[0]->IsArray()) {
        auto arr = info[0].As<Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
          Local<Value> v;
          if (arr->Get(ctx, i).ToLocal(&v) && v->IsString())
            store_names.push_back(utf8(iso, v));
        }
      } else {
        throw_msg(iso, "db.transaction: stores must be a string or array of strings", "TypeError");
        return;
      }
      for (auto& sn : store_names) {
        if (!h->db->stores.count(sn)) {
          throw_msg(iso, "NotFoundError: store '" + sn + "' does not exist", "NotFoundError");
          return;
        }
      }
      std::string mode = "readonly";
      if (info.Length() >= 2 && info[1]->IsString())
        mode = utf8(iso, info[1]);
      if (mode != "readonly" && mode != "readwrite") {
        throw_msg(iso, "TypeError: mode must be 'readonly' or 'readwrite'", "TypeError");
        return;
      }
      auto tx = create_transaction(iso, ctx, h->db, store_names, mode);
      info.GetReturnValue().Set(tx);
    }

    void database_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      (void)iso;
      auto* h = unwrap_database(info.This());
      if (!h)
        return;
      h->db->closed = true;
    }

    // ============================== IDBFactory ==============================

    std::shared_ptr<database_state> open_database(Isolate* iso, std::string name,
                                                  std::string& err) {
      auto& reg = registry_table()[iso];
      auto it = reg.by_name.find(name);
      if (it != reg.by_name.end())
        return it->second;
      auto fname = resolve_db_filename(name, err);
      if (!fname)
        return nullptr;
      auto st = std::make_shared<database_state>();
      st->name = name;
      st->filename = *fname;
      const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
      if (sqlite3_open_v2(st->filename.c_str(), &st->db, flags, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(st->db);
        if (st->db)
          sqlite3_close(st->db);
        return nullptr;
      }
      sqlite3_exec(st->db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
      if (!ensure_schema(st.get(), err)) {
        sqlite3_close(st->db);
        return nullptr;
      }
      reg.by_name[name] = st;
      return st;
    }

    // Deferred open() completion: ensures req.onupgradeneeded / onsuccess /
    // onerror listeners installed by user code after open() returns are seen
    // before the upgrade flow fires.
    struct open_completion {
      Global<Object> req;
      std::shared_ptr<database_state> db;
      int64_t requested_version = 0;
      bool needs_upgrade = false;
    };

    void open_completion_callback(void* data) {
      std::unique_ptr<open_completion> p(static_cast<open_completion*>(data));
      auto* iso = Isolate::GetCurrent();
      Isolate::Scope is(iso);
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      Context::Scope cs(ctx);
      auto req = p->req.Get(iso);
      if (p->needs_upgrade) {
        TryCatch tc(iso);
        std::vector<std::string> empty;
        auto tx = create_transaction(iso, ctx, p->db, empty, "versionchange");
        if (tc.HasCaught()) {
          reject_request(iso, ctx, req, tc.Exception());
          return;
        }
        auto db_obj = create_database_handle(iso, ctx, p->db);
        (void)db_obj->Set(ctx, "__fxe_active_tx"_v8(iso), tx);
        sqlite3_stmt* upd = nullptr;
        sqlite3_prepare_v2(p->db->db, "UPDATE __meta SET user_version=?1", -1, &upd, nullptr);
        sqlite3_bind_int64(upd, 1, p->requested_version);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
        int64_t old_version = p->db->version;
        p->db->version = p->requested_version;
        (void)db_obj->Set(ctx, "version"_v8(iso),
                          Number::New(iso, static_cast<double>(p->requested_version)));
        auto evt = Object::New(iso);
        set_str(ctx, evt, "type", "upgradeneeded");
        (void)evt->Set(ctx, "target"_v8(iso), req);
        (void)evt->Set(ctx, "oldVersion"_v8(iso),
                       Number::New(iso, static_cast<double>(old_version)));
        (void)evt->Set(ctx, "newVersion"_v8(iso),
                       Number::New(iso, static_cast<double>(p->requested_version)));
        (void)req->Set(ctx, "result"_v8(iso), db_obj);
        (void)req->Set(ctx, "transaction"_v8(iso), tx);
        invoke_listener(iso, ctx, req, "onupgradeneeded", evt);
        if (tc.HasCaught()) {
          // User handler threw; abort the tx and surface the error.
          auto* tx_st = unwrap_tx(tx);
          tx_abort_internal(iso, ctx, tx, tx_st);
          (void)db_obj->Set(ctx, "__fxe_active_tx"_v8(iso), Undefined(iso));
          reject_request(iso, ctx, req, tc.Exception());
          return;
        }
        auto* tx_st = unwrap_tx(tx);
        tx_commit_internal(iso, ctx, tx, tx_st);
        (void)db_obj->Set(ctx, "__fxe_active_tx"_v8(iso), Undefined(iso));
        if (tx_st && tx_st->errored) {
          reject_request(iso, ctx, req,
                         make_dom_error(iso, "AbortError", "versionchange transaction aborted"));
          return;
        }
        resolve_request(iso, ctx, req, db_obj);
      } else {
        auto db_obj = create_database_handle(iso, ctx, p->db);
        resolve_request(iso, ctx, req, db_obj);
      }
    }

    void schedule_open(Isolate* iso, Local<Object> req, std::shared_ptr<database_state> db,
                       int64_t requested_version, bool needs_upgrade) {
      auto* p = new open_completion();
      p->req.Reset(iso, req);
      p->db = std::move(db);
      p->requested_version = requested_version;
      p->needs_upgrade = needs_upgrade;
      iso->EnqueueMicrotask(&open_completion_callback, p);
    }

    void factory_open(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "indexedDB.open(name, version?)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      int64_t requested_version = -1;
      if (info.Length() >= 2 && info[1]->IsNumber()) {
        requested_version = info[1]->IntegerValue(ctx).FromMaybe(0);
        if (requested_version <= 0) {
          throw_msg(iso, "TypeError: version must be a positive integer", "TypeError");
          return;
        }
      }
      auto req = create_request(iso, ctx, request_kind::open, Local<Object>(), Local<Object>());
      std::string err;
      auto db = open_database(iso, name, err);
      if (!db) {
        schedule_reject(iso, req, make_dom_error(iso, "UnknownError", err));
        info.GetReturnValue().Set(req);
        return;
      }
      if (requested_version < 0)
        requested_version = std::max<int64_t>(db->version, 1);
      if (requested_version < db->version) {
        schedule_reject(
            iso, req,
            make_dom_error(iso, "VersionError", "VersionError: requested version < existing"));
        info.GetReturnValue().Set(req);
        return;
      }
      bool needs_upgrade = (requested_version > db->version);
      schedule_open(iso, req, db, requested_version, needs_upgrade);
      info.GetReturnValue().Set(req);
    }

    void factory_delete_database(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_msg(iso, "indexedDB.deleteDatabase(name)", "TypeError");
        return;
      }
      std::string name = utf8(iso, info[0]);
      auto req = create_request(iso, ctx, request_kind::open, Local<Object>(), Local<Object>());
      std::string err;
      auto fname = resolve_db_filename(name, err);
      auto& reg = registry_table()[iso];
      auto it = reg.by_name.find(name);
      if (it != reg.by_name.end()) {
        if (it->second->db) {
          sqlite3_close(it->second->db);
          it->second->db = nullptr;
        }
        reg.by_name.erase(it);
      }
      if (fname) {
        std::error_code ec;
        std::filesystem::remove(*fname, ec);
        std::filesystem::remove(*fname + "-wal", ec);
        std::filesystem::remove(*fname + "-shm", ec);
      }
      schedule_resolve(iso, req, Undefined(iso));
      info.GetReturnValue().Set(req);
    }

    void factory_databases(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      auto user_data = fxe::os::get_path("userData");
      auto arr = Array::New(iso);
      uint32_t i = 0;
      if (!user_data.empty()) {
        auto idb_dir = std::filesystem::path(user_data) / "idb";
        std::error_code ec;
        if (std::filesystem::exists(idb_dir, ec)) {
          for (auto& entry : std::filesystem::directory_iterator(idb_dir, ec)) {
            if (entry.path().extension() == ".sqlite3") {
              auto stem = entry.path().stem().string();
              auto rec = Object::New(iso);
              set_str(ctx, rec, "name", stem);
              // version unknown without opening — report 0 unless cached
              auto& reg = registry_table()[iso];
              auto it = reg.by_name.find(stem);
              int64_t v = it != reg.by_name.end() ? it->second->version : 0;
              set_int(ctx, rec, "version", v);
              (void)arr->Set(ctx, i++, rec);
            }
          }
        }
      }
      (void)resolver->Resolve(ctx, arr);
    }

    void factory_cmp(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 2) {
        throw_msg(iso, "indexedDB.cmp(a, b)", "TypeError");
        return;
      }
      std::vector<uint8_t> a, b;
      if (!encode_key(iso, info[0], a) || !encode_key(iso, info[1], b))
        return;
      info.GetReturnValue().Set(compare_blobs(a.data(), a.size(), b.data(), b.size()));
    }

    void indexed_db_reset_for_isolate(Isolate* iso) {
      // The per-isolate template entry holds Global<FunctionTemplate> handles
      // anchored to this isolate. Reset them and drop the map entry before the
      // isolate is disposed so V8 doesn't trip on dangling GlobalHandles.
      auto& table = templates_table();
      auto it = table.find(iso);
      if (it != table.end()) {
        it->second.request_tpl.Reset();
        it->second.open_request_tpl.Reset();
        it->second.database_tpl.Reset();
        it->second.transaction_tpl.Reset();
        it->second.object_store_tpl.Reset();
        it->second.index_tpl.Reset();
        it->second.cursor_tpl.Reset();
        it->second.cursor_with_value_tpl.Reset();
        it->second.key_range_tpl.Reset();
        table.erase(it);
      }
      // Also drop any open sqlite handles for this isolate.
      auto& reg = registry_table();
      auto rit = reg.find(iso);
      if (rit != reg.end()) {
        for (auto& [_, db] : rit->second.by_name) {
          if (db && db->db) {
            sqlite3_close(db->db);
            db->db = nullptr;
          }
        }
        reg.erase(rit);
      }
    }

    struct indexed_db_resetter_register {
      indexed_db_resetter_register() {
        register_template_resetter(&indexed_db_reset_for_isolate);
      }
    };
    static indexed_db_resetter_register s_indexed_db_resetter_register;

  } // namespace

  // ============================== Template installers ==============================

  void install_indexed_db_bindings(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);

    // IDBKeyRange constructor (no instances directly; static factories return wrapped).
    auto kr_ctor = FunctionTemplate::New(iso);
    kr_ctor->SetClassName("IDBKeyRange"_v8(iso));
    kr_ctor->InstanceTemplate()->SetInternalFieldCount(2);
    kr_ctor->PrototypeTemplate()->Set(iso, "includes",
                                      FunctionTemplate::New(iso, key_range_includes));
    kr_ctor->Set(iso, "only", FunctionTemplate::New(iso, key_range_only));
    kr_ctor->Set(iso, "lowerBound", FunctionTemplate::New(iso, key_range_lower_bound));
    kr_ctor->Set(iso, "upperBound", FunctionTemplate::New(iso, key_range_upper_bound));
    kr_ctor->Set(iso, "bound", FunctionTemplate::New(iso, key_range_bound));
    tpl_for(iso).key_range_tpl.Reset(iso, kr_ctor);
    global->Set(iso, "IDBKeyRange", kr_ctor);

    // IDBRequest / IDBOpenDBRequest
    auto req_tpl = FunctionTemplate::New(iso);
    req_tpl->SetClassName("IDBRequest"_v8(iso));
    req_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    tpl_for(iso).request_tpl.Reset(iso, req_tpl);
    auto open_tpl = FunctionTemplate::New(iso);
    open_tpl->SetClassName("IDBOpenDBRequest"_v8(iso));
    open_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    open_tpl->Inherit(req_tpl);
    tpl_for(iso).open_request_tpl.Reset(iso, open_tpl);

    // IDBDatabase
    auto db_tpl = FunctionTemplate::New(iso);
    db_tpl->SetClassName("IDBDatabase"_v8(iso));
    db_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    db_tpl->PrototypeTemplate()->Set(iso, "createObjectStore",
                                     FunctionTemplate::New(iso, database_create_object_store));
    db_tpl->PrototypeTemplate()->Set(iso, "deleteObjectStore",
                                     FunctionTemplate::New(iso, database_delete_object_store));
    db_tpl->PrototypeTemplate()->Set(iso, "transaction",
                                     FunctionTemplate::New(iso, database_transaction));
    db_tpl->PrototypeTemplate()->Set(iso, "close", FunctionTemplate::New(iso, database_close));
    tpl_for(iso).database_tpl.Reset(iso, db_tpl);

    // IDBTransaction
    auto tx_tpl = FunctionTemplate::New(iso);
    tx_tpl->SetClassName("IDBTransaction"_v8(iso));
    tx_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    tx_tpl->PrototypeTemplate()->Set(iso, "objectStore",
                                     FunctionTemplate::New(iso, tx_object_store));
    tx_tpl->PrototypeTemplate()->Set(iso, "commit", FunctionTemplate::New(iso, tx_commit));
    tx_tpl->PrototypeTemplate()->Set(iso, "abort", FunctionTemplate::New(iso, tx_abort));
    tpl_for(iso).transaction_tpl.Reset(iso, tx_tpl);

    // IDBObjectStore
    auto store_tpl = FunctionTemplate::New(iso);
    store_tpl->SetClassName("IDBObjectStore"_v8(iso));
    store_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    store_tpl->PrototypeTemplate()->Set(iso, "put", FunctionTemplate::New(iso, store_put));
    store_tpl->PrototypeTemplate()->Set(iso, "add", FunctionTemplate::New(iso, store_add));
    store_tpl->PrototypeTemplate()->Set(iso, "get", FunctionTemplate::New(iso, store_get));
    store_tpl->PrototypeTemplate()->Set(iso, "getKey", FunctionTemplate::New(iso, store_get_key));
    store_tpl->PrototypeTemplate()->Set(iso, "getAll", FunctionTemplate::New(iso, store_get_all));
    store_tpl->PrototypeTemplate()->Set(iso, "getAllKeys",
                                        FunctionTemplate::New(iso, store_get_all_keys));
    store_tpl->PrototypeTemplate()->Set(iso, "delete", FunctionTemplate::New(iso, store_delete));
    store_tpl->PrototypeTemplate()->Set(iso, "clear", FunctionTemplate::New(iso, store_clear));
    store_tpl->PrototypeTemplate()->Set(iso, "count", FunctionTemplate::New(iso, store_count));
    store_tpl->PrototypeTemplate()->Set(iso, "createIndex",
                                        FunctionTemplate::New(iso, store_create_index));
    store_tpl->PrototypeTemplate()->Set(iso, "deleteIndex",
                                        FunctionTemplate::New(iso, store_delete_index));
    store_tpl->PrototypeTemplate()->Set(iso, "index",
                                        FunctionTemplate::New(iso, store_index_method));
    store_tpl->PrototypeTemplate()->Set(iso, "openCursor",
                                        FunctionTemplate::New(iso, store_open_cursor));
    store_tpl->PrototypeTemplate()->Set(iso, "openKeyCursor",
                                        FunctionTemplate::New(iso, store_open_key_cursor));
    tpl_for(iso).object_store_tpl.Reset(iso, store_tpl);

    // IDBIndex (subset of object store ops)
    auto idx_tpl = FunctionTemplate::New(iso);
    idx_tpl->SetClassName("IDBIndex"_v8(iso));
    idx_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    idx_tpl->PrototypeTemplate()->Set(iso, "get", FunctionTemplate::New(iso, store_get));
    idx_tpl->PrototypeTemplate()->Set(iso, "getKey", FunctionTemplate::New(iso, store_get_key));
    idx_tpl->PrototypeTemplate()->Set(iso, "getAll", FunctionTemplate::New(iso, store_get_all));
    idx_tpl->PrototypeTemplate()->Set(iso, "getAllKeys",
                                      FunctionTemplate::New(iso, store_get_all_keys));
    idx_tpl->PrototypeTemplate()->Set(iso, "count", FunctionTemplate::New(iso, store_count));
    idx_tpl->PrototypeTemplate()->Set(iso, "openCursor",
                                      FunctionTemplate::New(iso, store_open_cursor));
    idx_tpl->PrototypeTemplate()->Set(iso, "openKeyCursor",
                                      FunctionTemplate::New(iso, store_open_key_cursor));
    tpl_for(iso).index_tpl.Reset(iso, idx_tpl);

    // IDBCursor / IDBCursorWithValue
    auto cur_tpl = FunctionTemplate::New(iso);
    cur_tpl->SetClassName("IDBCursor"_v8(iso));
    cur_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    cur_tpl->PrototypeTemplate()->Set(iso, "continue", FunctionTemplate::New(iso, cursor_continue));
    cur_tpl->PrototypeTemplate()->Set(iso, "advance", FunctionTemplate::New(iso, cursor_advance));
    cur_tpl->PrototypeTemplate()->Set(iso, "update", FunctionTemplate::New(iso, cursor_update));
    cur_tpl->PrototypeTemplate()->Set(iso, "delete", FunctionTemplate::New(iso, cursor_delete));
    tpl_for(iso).cursor_tpl.Reset(iso, cur_tpl);

    auto cwv_tpl = FunctionTemplate::New(iso);
    cwv_tpl->SetClassName("IDBCursorWithValue"_v8(iso));
    cwv_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    cwv_tpl->Inherit(cur_tpl);
    tpl_for(iso).cursor_with_value_tpl.Reset(iso, cwv_tpl);

    // IDBFactory (the `indexedDB` global itself).
    auto factory_tpl = ObjectTemplate::New(iso);
    factory_tpl->Set(iso, "open", FunctionTemplate::New(iso, factory_open));
    factory_tpl->Set(iso, "deleteDatabase", FunctionTemplate::New(iso, factory_delete_database));
    factory_tpl->Set(iso, "databases", FunctionTemplate::New(iso, factory_databases));
    factory_tpl->Set(iso, "cmp", FunctionTemplate::New(iso, factory_cmp));
    global->Set(iso, "indexedDB", factory_tpl);
  }

} // namespace fxe::js