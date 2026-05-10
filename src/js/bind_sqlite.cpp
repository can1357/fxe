// JS bindings implementing the `fxe:sqlite` synthetic ES module.
//
// Surface (mirrors Bun's `bun:sqlite`):
//   import { Database, constants } from "fxe:sqlite";
//   const db = new Database(":memory:", { readonly?, create?, readwrite?,
//                                          safeIntegers?, strict? });
//   const stmt = db.query("SELECT $name") | db.prepare(...);
//   stmt.all(params?), stmt.get(params?), stmt.run(params?),
//   stmt.values(params?), stmt.iterate(params?) | for..of, stmt.as(Class),
//   stmt.finalize(), stmt.toString(); columnNames / paramsCount accessors.
//   db.run(sql, params?), db.exec === db.run, db.transaction(fn) ->
//   wrapped fn with .deferred/.immediate/.exclusive, db.serialize(),
//   Database.deserialize(buf, opts?), db.loadExtension(name, entry?),
//   db.fileControl(cmd, value), db.close(throwOnError?), db[Symbol.dispose]().
//
// Type tags: 'SDB ' for Database, 'SST ' for Statement (see bind_sqlite.hpp).
//
// Threading: V8 is single-threaded; sqlite handles are created with the
// default serialised threading mode (whatever the build picked) and are only
// touched from the V8 thread.

#include "bind_sqlite.hpp"
#include "weak_holder.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fxe/js_bindings.hpp>
#include <fxe/log.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
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

    using TplGlobal = Global<FunctionTemplate>;

    // --- per-isolate template tables -----------------------------------------

    std::unordered_map<Isolate*, TplGlobal>& db_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& stmt_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void sqlite_reset_for_isolate(Isolate* iso) {
      if (auto it = db_tpl_table().find(iso); it != db_tpl_table().end()) {
        it->second.Reset();
        db_tpl_table().erase(it);
      }
      if (auto it = stmt_tpl_table().find(iso); it != stmt_tpl_table().end()) {
        it->second.Reset();
        stmt_tpl_table().erase(it);
      }
    }
    struct sqlite_resetter_register {
      sqlite_resetter_register() {
        register_template_resetter(&sqlite_reset_for_isolate);
      }
    };
    static sqlite_resetter_register s_sqlite_resetter_register;

    // --- holder structs ------------------------------------------------------

    struct stmt_holder; // fwd
    struct db_holder;
    void close_db_holder(db_holder*);
    void mark_stmt_finalized(stmt_holder*);

    struct db_holder : weak_holder<db_holder> {
      sqlite3* db = nullptr;
      bool safe_integers = false;
      bool strict = false;
      bool closed = false;
      // Open Statement holders for this database; used by .close(true) to
      // detect pending statements.
      std::vector<stmt_holder*> open_stmts;
      // Transaction nesting depth; depth 0 uses BEGIN/COMMIT/ROLLBACK, depth>0
      // uses SAVEPOINT/RELEASE/ROLLBACK TO.
      int txn_depth = 0;

      void on_finalize(v8::Isolate*) {
        close_db_holder(this);
      }
    };

    struct stmt_holder : weak_holder<stmt_holder> {
      sqlite3_stmt* stmt = nullptr;
      db_holder* owner = nullptr;
      std::string sql;
      bool finalized = false;
      // Cached column names (refreshed lazily after sqlite3_step).
      std::vector<std::string> column_names;
      // `as` class prototype, if any; column name keys are reused across rows.
      Global<Function> as_class;

      void on_finalize(v8::Isolate*) {
        mark_stmt_finalized(this);
      }
    };

    // --- utilities -----------------------------------------------------------

    Local<String> s(Isolate* iso, std::string_view sv) {
      return String::NewFromUtf8(iso, sv.data(), NewStringType::kNormal,
                                 static_cast<int>(sv.size()))
          .ToLocalChecked();
    }

    Local<String> s(Isolate* iso, const char* z) {
      return String::NewFromUtf8(iso, z ? z : "", NewStringType::kNormal).ToLocalChecked();
    }

    [[nodiscard]] bool throw_sqlite(Isolate* iso, sqlite3* db, const char* fallback = nullptr) {
      const char* msg = db ? sqlite3_errmsg(db) : fallback;
      if (!msg || !*msg)
        msg = fallback ? fallback : "sqlite error";
      auto err = Exception::Error(s(iso, msg));
      if (db && err->IsObject()) {
        auto ctx = iso->GetCurrentContext();
        auto o = err.As<Object>();
        (void)o->Set(ctx, "code"_v8(iso), Integer::New(iso, sqlite3_extended_errcode(db)));
        (void)o->Set(ctx, "errno"_v8(iso), Integer::New(iso, sqlite3_errcode(db)));
      }
      iso->ThrowException(err);
      return false;
    }

    bool sqlite_unknown_key_warning_enabled(Isolate* iso, Local<Context> ctx) {
      Local<Value> flag;
      if (!ctx->Global()->Get(ctx, "__FXE_SQLITE_WARN_UNKNOWN_KEYS"_v8(iso)).ToLocal(&flag))
        return false;
      return flag->IsTrue();
    }

    bool& sqlite_unknown_keys_warned() {
      static bool warned = false;
      return warned;
    }

    void warn_ignored_sqlite_keys_once(const std::vector<std::string>& keys) {
      if (keys.empty() || sqlite_unknown_keys_warned())
        return;
      sqlite_unknown_keys_warned() = true;
      std::string joined;
      for (const auto& key : keys) {
        if (!joined.empty())
          joined += ", ";
        joined += key;
      }
      FXE_WARN("js.sqlite",
               "ignored unknown non-strict bind key(s): {} (Bun-compatible; opt in with "
               "globalThis.__FXE_SQLITE_WARN_UNKNOWN_KEYS = true)",
               joined);
    }

    db_holder* unwrap_db(Local<Object> obj) {
      return static_cast<db_holder*>(unwrap(obj, TAG_SQLITE_DATABASE));
    }
    stmt_holder* unwrap_stmt(Local<Object> obj) {
      return static_cast<stmt_holder*>(unwrap(obj, TAG_SQLITE_STATEMENT));
    }

    void mark_stmt_finalized(stmt_holder* sh) {
      if (!sh || sh->finalized)
        return;
      if (sh->stmt) {
        sqlite3_finalize(sh->stmt);
        sh->stmt = nullptr;
      }
      sh->finalized = true;
      if (sh->owner) {
        auto& v = sh->owner->open_stmts;
        v.erase(std::remove(v.begin(), v.end(), sh), v.end());
        sh->owner = nullptr;
      }
    }

    void close_db_holder(db_holder* dh) {
      if (!dh || dh->closed)
        return;
      // Finalise any still-live statements; sqlite3_close requires it.
      for (auto* sh : dh->open_stmts) {
        if (sh && !sh->finalized && sh->stmt) {
          sqlite3_finalize(sh->stmt);
          sh->stmt = nullptr;
          sh->finalized = true;
          sh->owner = nullptr;
        }
      }
      dh->open_stmts.clear();
      if (dh->db) {
        sqlite3_close_v2(dh->db);
        dh->db = nullptr;
      }
      dh->closed = true;
    }

    // --- finalisers ---------------------------------------------------------

    // db_holder cleanup runs through weak_holder::on_finalize.

    // stmt_holder cleanup runs through weak_holder::on_finalize.

    // --- value coercion: JS -> sqlite parameter binding ---------------------

    // Bind a JS value to parameter index `idx` (1-based). On failure throws
    // and returns false.
    bool bind_one(Isolate* iso, Local<Context> ctx, sqlite3_stmt* stmt, int idx, Local<Value> v,
                  bool safe_integers) {
      if (v->IsNullOrUndefined()) {
        if (sqlite3_bind_null(stmt, idx) != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      if (v->IsBoolean()) {
        int rc = sqlite3_bind_int(stmt, idx, v->BooleanValue(iso) ? 1 : 0);
        if (rc != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      if (v->IsBigInt()) {
        bool lossless = false;
        i64 i = v.As<BigInt>()->Int64Value(&lossless);
        if (!lossless) {
          // Build a meaningful range message.
          String::Utf8Value u(iso, v);
          std::string msg = "BigInt value '";
          msg += (*u ? std::string(*u, u.length()) : std::string{});
          msg += "' is out of range";
          return throw_error(iso, msg);
        }
        if (sqlite3_bind_int64(stmt, idx, i) != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      if (v->IsNumber()) {
        double d = v->NumberValue(ctx).FromMaybe(0);
        // Match Bun: if the number is integral and fits in int64, bind as int.
        if (std::isfinite(d) && d == static_cast<double>(static_cast<i64>(d)) &&
            d >= -9.2233720368547758e18 && d <= 9.2233720368547758e18) {
          if (sqlite3_bind_int64(stmt, idx, static_cast<i64>(d)) != SQLITE_OK)
            return throw_sqlite(iso, sqlite3_db_handle(stmt));
          return true;
        }
        if (sqlite3_bind_double(stmt, idx, d) != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      if (v->IsUint8Array() || v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        auto buf = view->Buffer();
        auto bs = buf->GetBackingStore();
        const u8* data = static_cast<const u8*>(bs->Data()) + view->ByteOffset();
        int n = static_cast<int>(view->ByteLength());
        if (sqlite3_bind_blob(stmt, idx, data, n, SQLITE_TRANSIENT) != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      if (v->IsArrayBuffer()) {
        auto buf = v.As<ArrayBuffer>();
        auto bs = buf->GetBackingStore();
        if (sqlite3_bind_blob(stmt, idx, bs->Data(), static_cast<int>(buf->ByteLength()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      if (v->IsString()) {
        String::Utf8Value u(iso, v);
        if (sqlite3_bind_text(stmt, idx, *u ? *u : "", static_cast<int>(u.length()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
          return throw_sqlite(iso, sqlite3_db_handle(stmt));
        return true;
      }
      // Fallback: stringify objects.
      auto str = v->ToString(ctx);
      Local<String> sv;
      if (!str.ToLocal(&sv))
        return throw_type_error(iso, "unsupported sqlite parameter type");
      String::Utf8Value u(iso, sv);
      if (sqlite3_bind_text(stmt, idx, *u ? *u : "", static_cast<int>(u.length()),
                            SQLITE_TRANSIENT) != SQLITE_OK)
        return throw_sqlite(iso, sqlite3_db_handle(stmt));
      (void)safe_integers;
      return true;
    }

    // Apply parameters from the FunctionCallbackInfo starting at argument
    // `arg_offset`. Mirrors Bun's behaviour: an object argument binds named
    // parameters (with strict mode controlling prefix handling), positional
    // arguments bind sequentially.
    bool apply_params(const FunctionCallbackInfo<Value>& info, int arg_offset, sqlite3_stmt* stmt,
                      bool strict, bool safe_integers) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      sqlite3_clear_bindings(stmt);

      // Bun-compatible non-strict mode ignores extra object keys. Set
      // globalThis.__FXE_SQLITE_WARN_UNKNOWN_KEYS = true to opt into a
      // once-per-session diagnostic that lists the ignored keys.
      const bool warn_unknown_keys = sqlite_unknown_key_warning_enabled(iso, ctx);
      const int param_count = sqlite3_bind_parameter_count(stmt);
      const int n_args = info.Length() - arg_offset;
      if (n_args <= 0)
        return true;

      auto first = info[arg_offset];
      const bool single_object = (n_args == 1) && first->IsObject() && !first->IsTypedArray() &&
                                 !first->IsArrayBufferView() && !first->IsArrayBuffer() &&
                                 !first->IsArray() && !first->IsBigInt();

      if (single_object) {
        auto obj = first.As<Object>();
        // Track which params were bound when strict mode is on.
        std::vector<bool> bound(static_cast<usize>(param_count) + 1, false);
        Local<Array> keys;
        std::vector<std::string> ignored_unknown_keys;
        if (!obj->GetOwnPropertyNames(ctx).ToLocal(&keys))
          return throw_error(iso, "failed to read parameter object keys");
        for (u32 i = 0; i < keys->Length(); ++i) {
          Local<Value> k;
          if (!keys->Get(ctx, i).ToLocal(&k))
            continue;
          String::Utf8Value ku(iso, k);
          if (!*ku)
            continue;
          std::string name(*ku, ku.length());
          int idx = sqlite3_bind_parameter_index(stmt, name.c_str());
          if (idx == 0) {
            // Strict mode: try with each prefix.
            if (strict) {
              std::string p = ":" + name;
              idx = sqlite3_bind_parameter_index(stmt, p.c_str());
              if (idx == 0) {
                p = "@" + name;
                idx = sqlite3_bind_parameter_index(stmt, p.c_str());
              }
              if (idx == 0) {
                p = "$" + name;
                idx = sqlite3_bind_parameter_index(stmt, p.c_str());
              }
              if (idx == 0)
                return throw_error(iso, "Missing parameter \"" + name + "\"");
            } else {
              // Non-strict: ignore unknown keys for Bun compatibility.
              if (warn_unknown_keys && !sqlite_unknown_keys_warned())
                ignored_unknown_keys.push_back(name);
              continue;
            }
          }
          Local<Value> val;
          if (!obj->Get(ctx, k).ToLocal(&val))
            continue;
          if (!bind_one(iso, ctx, stmt, idx, val, safe_integers))
            return false;
          if (idx > 0 && static_cast<usize>(idx) < bound.size())
            bound[static_cast<usize>(idx)] = true;
        }
        if (strict) {
          for (int i = 1; i <= param_count; ++i) {
            if (!bound[static_cast<usize>(i)]) {
              const char* nm = sqlite3_bind_parameter_name(stmt, i);
              std::string nstr = nm ? nm : ("?" + std::to_string(i));
              return throw_error(iso, "Missing parameter \"" + nstr + "\"");
            }
          }
        }
        warn_ignored_sqlite_keys_once(ignored_unknown_keys);
        return true;
      }

      // Positional binding: each arg goes to slots 1..N in order.
      for (int i = 0; i < n_args; ++i) {
        if (!bind_one(iso, ctx, stmt, i + 1, info[arg_offset + i], safe_integers))
          return false;
      }
      return true;
    }

    // --- value coercion: sqlite column -> JS value --------------------------

    Local<Value> column_value(Isolate* iso, Local<Context> /*ctx*/, sqlite3_stmt* stmt, int col,
                              bool safe_integers) {
      switch (sqlite3_column_type(stmt, col)) {
      case SQLITE_INTEGER: {
        i64 v = sqlite3_column_int64(stmt, col);
        if (safe_integers)
          return BigInt::New(iso, v);
        // Match Bun: return as number even when it loses precision past 2^53.
        return Number::New(iso, static_cast<double>(v));
      }
      case SQLITE_FLOAT:
        return Number::New(iso, sqlite3_column_double(stmt, col));
      case SQLITE_TEXT: {
        const auto* txt = sqlite3_column_text(stmt, col);
        int n = sqlite3_column_bytes(stmt, col);
        return String::NewFromUtf8(iso, reinterpret_cast<const char*>(txt), NewStringType::kNormal,
                                   n)
            .ToLocalChecked();
      }
      case SQLITE_BLOB: {
        const void* data = sqlite3_column_blob(stmt, col);
        int n = sqlite3_column_bytes(stmt, col);
        auto buf = ArrayBuffer::New(iso, static_cast<usize>(n));
        if (n > 0)
          std::memcpy(buf->GetBackingStore()->Data(), data, static_cast<usize>(n));
        return Uint8Array::New(buf, 0, static_cast<usize>(n));
      }
      case SQLITE_NULL:
      default:
        return Null(iso);
      }
    }

    void refresh_column_names(stmt_holder* sh) {
      sh->column_names.clear();
      int n = sqlite3_column_count(sh->stmt);
      sh->column_names.reserve(static_cast<usize>(n));
      for (int i = 0; i < n; ++i) {
        const char* nm = sqlite3_column_name(sh->stmt, i);
        sh->column_names.emplace_back(nm ? nm : "");
      }
    }

    Local<Object> make_row_object(Isolate* iso, Local<Context> ctx, stmt_holder* sh,
                                  bool safe_integers) {
      Local<Object> row;
      if (!sh->as_class.IsEmpty()) {
        // Match Bun: bypass the constructor and assign row props onto an
        // object whose prototype is the class prototype.
        auto cls = sh->as_class.Get(iso);
        Local<Value> proto;
        if (cls->Get(ctx, "prototype"_v8(iso)).ToLocal(&proto) && proto->IsObject()) {
          row = Object::New(iso, proto, nullptr, nullptr, 0);
        } else {
          row = Object::New(iso);
        }
      } else {
        row = Object::New(iso);
      }
      int n = sqlite3_column_count(sh->stmt);
      for (int i = 0; i < n; ++i) {
        const auto& name = sh->column_names[static_cast<usize>(i)];
        (void)row->Set(ctx, s(iso, name), column_value(iso, ctx, sh->stmt, i, safe_integers));
      }
      return row;
    }

    Local<Array> make_row_values(Isolate* iso, Local<Context> ctx, stmt_holder* sh,
                                 bool safe_integers) {
      int n = sqlite3_column_count(sh->stmt);
      auto arr = Array::New(iso, n);
      for (int i = 0; i < n; ++i) {
        (void)arr->Set(ctx, static_cast<u32>(i),
                       column_value(iso, ctx, sh->stmt, i, safe_integers));
      }
      return arr;
    }

    // --- Statement methods --------------------------------------------------

    bool ensure_stmt_live(Isolate* iso, stmt_holder* sh) {
      if (!sh)
        return throw_error(iso, "invalid statement");
      if (sh->finalized || !sh->stmt)
        return throw_error(iso, "statement is finalized");
      if (!sh->owner || sh->owner->closed)
        return throw_error(iso, "database is closed");
      return true;
    }

    void stmt_all(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = unwrap_stmt(info.This());
      if (!ensure_stmt_live(iso, sh))
        return;
      sqlite3_reset(sh->stmt);
      if (!apply_params(info, 0, sh->stmt, sh->owner->strict, sh->owner->safe_integers))
        return;
      refresh_column_names(sh);
      auto out = Array::New(iso);
      u32 row_idx = 0;
      for (;;) {
        int rc = sqlite3_step(sh->stmt);
        if (rc == SQLITE_ROW) {
          auto row = make_row_object(iso, ctx, sh, sh->owner->safe_integers);
          (void)out->Set(ctx, row_idx++, row);
        } else if (rc == SQLITE_DONE) {
          break;
        } else {
          (void)throw_sqlite(iso, sh->owner->db);
          return;
        }
      }
      info.GetReturnValue().Set(out);
    }

    void stmt_get(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = unwrap_stmt(info.This());
      if (!ensure_stmt_live(iso, sh))
        return;
      sqlite3_reset(sh->stmt);
      if (!apply_params(info, 0, sh->stmt, sh->owner->strict, sh->owner->safe_integers))
        return;
      refresh_column_names(sh);
      int rc = sqlite3_step(sh->stmt);
      if (rc == SQLITE_ROW) {
        info.GetReturnValue().Set(make_row_object(iso, ctx, sh, sh->owner->safe_integers));
      } else if (rc == SQLITE_DONE) {
        info.GetReturnValue().Set(Null(iso));
      } else {
        (void)throw_sqlite(iso, sh->owner->db);
      }
    }

    void stmt_values(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = unwrap_stmt(info.This());
      if (!ensure_stmt_live(iso, sh))
        return;
      sqlite3_reset(sh->stmt);
      if (!apply_params(info, 0, sh->stmt, sh->owner->strict, sh->owner->safe_integers))
        return;
      refresh_column_names(sh);
      auto out = Array::New(iso);
      u32 i = 0;
      for (;;) {
        int rc = sqlite3_step(sh->stmt);
        if (rc == SQLITE_ROW) {
          (void)out->Set(ctx, i++, make_row_values(iso, ctx, sh, sh->owner->safe_integers));
        } else if (rc == SQLITE_DONE) {
          break;
        } else {
          (void)throw_sqlite(iso, sh->owner->db);
          return;
        }
      }
      info.GetReturnValue().Set(out);
    }

    Local<Object> make_run_result(Isolate* iso, Local<Context> ctx, db_holder* dh) {
      auto out = Object::New(iso);
      i64 rowid = sqlite3_last_insert_rowid(dh->db);
      int changes = sqlite3_changes(dh->db);
      if (dh->safe_integers) {
        (void)out->Set(ctx, "lastInsertRowid"_v8(iso), BigInt::New(iso, rowid));
        (void)out->Set(ctx, "changes"_v8(iso), Integer::New(iso, changes));
      } else {
        (void)out->Set(ctx, "lastInsertRowid"_v8(iso),
                       Number::New(iso, static_cast<double>(rowid)));
        (void)out->Set(ctx, "changes"_v8(iso), Integer::New(iso, changes));
      }
      return out;
    }

    void stmt_run(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = unwrap_stmt(info.This());
      if (!ensure_stmt_live(iso, sh))
        return;
      sqlite3_reset(sh->stmt);
      if (!apply_params(info, 0, sh->stmt, sh->owner->strict, sh->owner->safe_integers))
        return;
      int rc = sqlite3_step(sh->stmt);
      if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        (void)throw_sqlite(iso, sh->owner->db);
        return;
      }
      info.GetReturnValue().Set(make_run_result(iso, ctx, sh->owner));
    }

    void stmt_finalize(const FunctionCallbackInfo<Value>& info) {
      auto* sh = unwrap_stmt(info.This());
      mark_stmt_finalized(sh);
    }

    void stmt_to_string(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* sh = unwrap_stmt(info.This());
      if (!sh)
        return;
      if (sh->finalized || !sh->stmt) {
        info.GetReturnValue().Set(s(iso, sh->sql));
        return;
      }
      char* expanded = sqlite3_expanded_sql(sh->stmt);
      if (expanded) {
        info.GetReturnValue().Set(s(iso, expanded));
        sqlite3_free(expanded);
      } else {
        info.GetReturnValue().Set(s(iso, sh->sql));
      }
    }

    void stmt_as(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* sh = unwrap_stmt(info.This());
      if (!sh)
        return;
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "Statement.as expects a class");
        return;
      }
      sh->as_class.Reset(iso, info[0].As<Function>());
      info.GetReturnValue().Set(info.This());
    }

    // Iterator protocol: `iterate()` and `[Symbol.iterator]()` both return an
    // iterator whose `.next()` walks sqlite3_step until SQLITE_DONE. The
    // iterator carries (statement-object, safe_integers) via internal fields.
    Global<FunctionTemplate>& iterator_tpl_table_for(Isolate* iso);

    void iterator_next(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto self = info.This();
      Local<Value> stmt_v = self->GetInternalField(0).As<Value>();
      if (!stmt_v->IsObject()) {
        (void)throw_error(iso, "invalid iterator");
        return;
      }
      auto* sh = unwrap_stmt(stmt_v.As<Object>());
      auto out = Object::New(iso);
      if (!sh || sh->finalized || !sh->stmt || !sh->owner || sh->owner->closed) {
        (void)out->Set(ctx, "value"_v8(iso), Undefined(iso));
        (void)out->Set(ctx, "done"_v8(iso), True(iso));
        info.GetReturnValue().Set(out);
        return;
      }
      int rc = sqlite3_step(sh->stmt);
      if (rc == SQLITE_ROW) {
        (void)out->Set(ctx, "value"_v8(iso),
                       make_row_object(iso, ctx, sh, sh->owner->safe_integers));
        (void)out->Set(ctx, "done"_v8(iso), False(iso));
      } else if (rc == SQLITE_DONE) {
        (void)out->Set(ctx, "value"_v8(iso), Undefined(iso));
        (void)out->Set(ctx, "done"_v8(iso), True(iso));
      } else {
        (void)throw_sqlite(iso, sh->owner->db);
        return;
      }
      info.GetReturnValue().Set(out);
    }

    void iterator_self(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(info.This());
    }

    Global<FunctionTemplate>& iterator_tpl_table_for(Isolate* iso) {
      static std::unordered_map<Isolate*, Global<FunctionTemplate>> t;
      auto& slot = t[iso];
      if (slot.IsEmpty()) {
        HandleScope hs(iso);
        auto tpl = FunctionTemplate::New(iso);
        tpl->SetClassName("SqliteStatementIterator"_v8(iso));
        tpl->InstanceTemplate()->SetInternalFieldCount(1);
        auto proto = tpl->PrototypeTemplate();
        proto->Set(iso, "next", FunctionTemplate::New(iso, iterator_next));
        proto->Set(Symbol::GetIterator(iso), FunctionTemplate::New(iso, iterator_self));
        slot.Reset(iso, tpl);
      }
      return slot;
    }

    Local<Object> make_iterator(Isolate* iso, Local<Context> ctx, Local<Object> stmt_obj) {
      auto tpl = iterator_tpl_table_for(iso).Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      auto inst = fn->NewInstance(ctx).ToLocalChecked();
      inst->SetInternalField(0, stmt_obj);
      return inst;
    }

    void stmt_iterate(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = unwrap_stmt(info.This());
      if (!ensure_stmt_live(iso, sh))
        return;
      sqlite3_reset(sh->stmt);
      if (!apply_params(info, 0, sh->stmt, sh->owner->strict, sh->owner->safe_integers))
        return;
      refresh_column_names(sh);
      info.GetReturnValue().Set(make_iterator(iso, ctx, info.This()));
    }

    void stmt_get_column_names(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* sh = unwrap_stmt(info.This());
      if (!sh || !sh->stmt) {
        info.GetReturnValue().Set(Array::New(iso, 0));
        return;
      }
      int n = sqlite3_column_count(sh->stmt);
      auto arr = Array::New(iso, n);
      for (int i = 0; i < n; ++i) {
        const char* nm = sqlite3_column_name(sh->stmt, i);
        (void)arr->Set(ctx, static_cast<u32>(i), s(iso, nm ? nm : ""));
      }
      info.GetReturnValue().Set(arr);
    }

    void stmt_get_params_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* sh = unwrap_stmt(info.This());
      if (!sh || !sh->stmt) {
        info.GetReturnValue().Set(0_v8(iso));
        return;
      }
      info.GetReturnValue().Set(Integer::New(iso, sqlite3_bind_parameter_count(sh->stmt)));
    }

    void stmt_get_native(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Null(info.GetIsolate()));
    }

    // --- Statement construction --------------------------------------------

    Local<Object> wrap_statement(Isolate* iso, Local<Context> ctx, db_holder* owner,
                                 sqlite3_stmt* stmt, std::string sql) {
      auto tpl = stmt_tpl_table()[iso].Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      auto obj = fn->NewInstance(ctx).ToLocalChecked();
      auto* sh = new stmt_holder{};
      sh->stmt = stmt;
      sh->owner = owner;
      sh->sql = std::move(sql);
      owner->open_stmts.push_back(sh);
      set_native(iso, obj, sh, TAG_SQLITE_STATEMENT);
      sh->bind(iso, obj);
      return obj;
    }

    void stmt_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso,
                               "Statement is not user-constructible; use Database.prototype.query");
        return;
      }
      // Internal-only call from wrap_statement; fields filled by caller.
    }

    // --- Database methods --------------------------------------------------

    bool ensure_db_live(Isolate* iso, db_holder* dh) {
      if (!dh)
        return throw_error(iso, "invalid database");
      if (dh->closed || !dh->db)
        return throw_error(iso, "database is closed");
      return true;
    }

    void db_query_or_prepare(const FunctionCallbackInfo<Value>& info, bool /*cache*/) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* dh = unwrap_db(info.This());
      if (!ensure_db_live(iso, dh))
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "Database.query/prepare expects a SQL string");
        return;
      }
      String::Utf8Value sql(iso, info[0]);
      sqlite3_stmt* stmt = nullptr;
      const char* tail = nullptr;
      int rc = sqlite3_prepare_v2(dh->db, *sql ? *sql : "", static_cast<int>(sql.length()), &stmt,
                                  &tail);
      if (rc != SQLITE_OK) {
        (void)throw_sqlite(iso, dh->db);
        return;
      }
      if (!stmt) {
        (void)throw_error(iso, "Database.query: empty SQL");
        return;
      }
      info.GetReturnValue().Set(
          wrap_statement(iso, ctx, dh, stmt, std::string(*sql ? *sql : "", sql.length())));
    }

    void db_query(const FunctionCallbackInfo<Value>& info) {
      db_query_or_prepare(info, true);
    }
    void db_prepare(const FunctionCallbackInfo<Value>& info) {
      db_query_or_prepare(info, false);
    }

    void db_run(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* dh = unwrap_db(info.This());
      if (!ensure_db_live(iso, dh))
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "Database.run expects a SQL string");
        return;
      }
      String::Utf8Value sql(iso, info[0]);
      // No params: defer to sqlite3_exec for multi-statement support.
      if (info.Length() == 1) {
        char* err = nullptr;
        int rc = sqlite3_exec(dh->db, *sql ? *sql : "", nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
          std::string msg = err ? err : "sqlite error";
          if (err)
            sqlite3_free(err);
          (void)throw_error(iso, msg);
          return;
        }
        info.GetReturnValue().Set(make_run_result(iso, ctx, dh));
        return;
      }
      // With parameters: prepare + bind + step. Multi-statement SQL is not
      // supported in this path (matches Bun: parameterised exec uses one
      // statement at a time).
      sqlite3_stmt* stmt = nullptr;
      int rc = sqlite3_prepare_v2(dh->db, *sql ? *sql : "", static_cast<int>(sql.length()), &stmt,
                                  nullptr);
      if (rc != SQLITE_OK || !stmt) {
        if (stmt)
          sqlite3_finalize(stmt);
        (void)throw_sqlite(iso, dh->db);
        return;
      }
      if (!apply_params(info, 1, stmt, dh->strict, dh->safe_integers)) {
        sqlite3_finalize(stmt);
        return;
      }
      rc = sqlite3_step(stmt);
      if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        (void)throw_sqlite(iso, dh->db);
        return;
      }
      sqlite3_finalize(stmt);
      info.GetReturnValue().Set(make_run_result(iso, ctx, dh));
    }

    void db_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* dh = unwrap_db(info.This());
      if (!dh || dh->closed)
        return;
      bool throw_if_busy = info.Length() > 0 && info[0]->BooleanValue(iso);
      if (throw_if_busy) {
        for (auto* sh : dh->open_stmts) {
          if (sh && !sh->finalized) {
            (void)throw_error(iso, "database has pending statements");
            return;
          }
        }
      }
      close_db_holder(dh);
    }

    void db_serialize(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* dh = unwrap_db(info.This());
      if (!ensure_db_live(iso, dh))
        return;
      const char* schema = "main";
      if (info.Length() > 0 && info[0]->IsString()) {
        String::Utf8Value u(iso, info[0]);
        if (*u)
          schema = *u;
      }
      sqlite3_int64 size = 0;
      unsigned char* buf = sqlite3_serialize(dh->db, schema, &size, 0);
      if (!buf) {
        (void)throw_error(iso, "Database.serialize failed");
        return;
      }
      auto ab = ArrayBuffer::New(iso, static_cast<usize>(size));
      if (size > 0)
        std::memcpy(ab->GetBackingStore()->Data(), buf, static_cast<usize>(size));
      sqlite3_free(buf);
      info.GetReturnValue().Set(Uint8Array::New(ab, 0, static_cast<usize>(size)));
    }

    void db_load_extension(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* dh = unwrap_db(info.This());
      if (!ensure_db_live(iso, dh))
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "loadExtension expects a path string");
        return;
      }
      String::Utf8Value path(iso, info[0]);
      const char* entry = nullptr;
      String::Utf8Value entry_u(iso, info.Length() > 1 ? info[1] : Local<Value>());
      if (info.Length() > 1 && info[1]->IsString())
        entry = *entry_u;
#ifdef SQLITE_OMIT_LOAD_EXTENSION
      (void)path;
      (void)entry;
      (void)throw_error(iso, "sqlite built without load_extension support");
      return;
#else
      sqlite3_db_config(dh->db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, nullptr);
      char* err = nullptr;
      int rc = sqlite3_load_extension(dh->db, *path ? *path : "", entry, &err);
      if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errmsg(dh->db);
        if (err)
          sqlite3_free(err);
        (void)throw_error(iso, msg);
      }
#endif
    }

    void db_file_control(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* dh = unwrap_db(info.This());
      if (!ensure_db_live(iso, dh))
        return;
      if (info.Length() < 2)
        return (void)throw_type_error(iso, "fileControl expects (cmd, value)");
      int op = static_cast<int>(info[0]->Int32Value(ctx).FromMaybe(0));
      auto v = info[1];
      // Supported value shapes: number, TypedArray, null/undefined.
      if (v->IsNullOrUndefined()) {
        int rc = sqlite3_file_control(dh->db, "main", op, nullptr);
        info.GetReturnValue().Set(Integer::New(iso, rc));
        return;
      }
      if (v->IsNumber()) {
        int n = static_cast<int>(v->Int32Value(ctx).FromMaybe(0));
        int rc = sqlite3_file_control(dh->db, "main", op, &n);
        info.GetReturnValue().Set(Integer::New(iso, rc));
        return;
      }
      if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        auto bs = view->Buffer()->GetBackingStore();
        void* p = static_cast<u8*>(bs->Data()) + view->ByteOffset();
        int rc = sqlite3_file_control(dh->db, "main", op, p);
        info.GetReturnValue().Set(Integer::New(iso, rc));
        return;
      }
      (void)throw_type_error(iso, "fileControl value must be number, typed array, or null");
    }

    // --- Transaction wrapper ------------------------------------------------
    //
    // db.transaction(fn) returns a wrapper Function that, when invoked, opens
    // a sqlite transaction (or savepoint when nested), calls `fn` with the
    // forwarded args, and commits — or rolls back if `fn` throws.
    //
    // `.deferred` / `.immediate` / `.exclusive` are sibling functions on the
    // wrapper that pre-set the BEGIN keyword. Nested calls always use a
    // savepoint regardless of the pre-set keyword.

    enum class begin_kind { Default, Deferred, Immediate, Exclusive };

    struct txn_data {
      Global<Function> fn;
      Global<Object> db_obj;
      begin_kind kind = begin_kind::Default;
      Global<Function>* persistent = nullptr;
    };

    void txn_data_finalizer(const WeakCallbackInfo<txn_data>& info) {
      auto* td = info.GetParameter();
      if (td && td->persistent) {
        td->persistent->Reset();
        delete td->persistent;
      }
      delete td;
    }

    const char* begin_sql(begin_kind k) {
      switch (k) {
      case begin_kind::Deferred:
        return "BEGIN DEFERRED";
      case begin_kind::Immediate:
        return "BEGIN IMMEDIATE";
      case begin_kind::Exclusive:
        return "BEGIN EXCLUSIVE";
      case begin_kind::Default:
      default:
        return "BEGIN";
      }
    }

    bool exec_simple(Isolate* iso, sqlite3* db, const char* sql) {
      char* err = nullptr;
      int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
      if (rc != SQLITE_OK) {
        std::string msg = err ? err : "sqlite transaction error";
        if (err)
          sqlite3_free(err);
        return throw_error(iso, msg);
      }
      return true;
    }

    void txn_invoke(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto data = info.Data();
      if (!data->IsExternal()) {
        (void)throw_error(iso, "invalid transaction wrapper");
        return;
      }
      auto* td = external_ptr<txn_data>(data);
      if (!td) {
        (void)throw_error(iso, "invalid transaction wrapper");
        return;
      }
      auto db_obj = td->db_obj.Get(iso);
      auto* dh = unwrap_db(db_obj);
      if (!ensure_db_live(iso, dh))
        return;

      const bool nested = dh->txn_depth > 0;
      const int depth_before = dh->txn_depth;
      std::string sp_name;
      if (nested) {
        sp_name = "fxe_sp_" + std::to_string(depth_before);
        if (!exec_simple(iso, dh->db, ("SAVEPOINT " + sp_name).c_str()))
          return;
      } else {
        if (!exec_simple(iso, dh->db, begin_sql(td->kind)))
          return;
      }
      dh->txn_depth = depth_before + 1;

      // Forward args + this to the wrapped function.
      std::vector<Local<Value>> argv;
      argv.reserve(static_cast<usize>(info.Length()));
      for (int i = 0; i < info.Length(); ++i)
        argv.push_back(info[i]);

      TryCatch tc(iso);
      auto fn = td->fn.Get(iso);
      MaybeLocal<Value> ret = fn->Call(ctx, info.This(), info.Length(), argv.data());

      // Reset depth before commit/rollback so re-entry behaves correctly even
      // if commit itself triggers another statement.
      dh->txn_depth = depth_before;

      if (tc.HasCaught()) {
        // Rollback.
        if (nested) {
          (void)exec_simple(iso, dh->db,
                            ("ROLLBACK TO " + sp_name + "; RELEASE " + sp_name).c_str());
        } else {
          (void)exec_simple(iso, dh->db, "ROLLBACK");
        }
        tc.ReThrow();
        return;
      }

      // Commit.
      bool ok = nested ? exec_simple(iso, dh->db, ("RELEASE " + sp_name).c_str())
                       : exec_simple(iso, dh->db, "COMMIT");
      if (!ok)
        return;

      Local<Value> v;
      if (ret.ToLocal(&v))
        info.GetReturnValue().Set(v);
    }

    Local<Function> make_txn_wrapper(Isolate* iso, Local<Context> ctx, Local<Object> db_obj,
                                     Local<Function> fn, begin_kind k) {
      auto* td = new txn_data{};
      td->fn.Reset(iso, fn);
      td->db_obj.Reset(iso, db_obj);
      td->kind = k;
      auto ext = make_external(iso, td);
      auto wrapper = Function::New(ctx, txn_invoke, ext).ToLocalChecked();
      // Tie td lifetime to the wrapper Function GC.
      auto* persistent = new Global<Function>(iso, wrapper);
      td->persistent = persistent;
      persistent->SetWeak(td, txn_data_finalizer, WeakCallbackType::kParameter);
      return wrapper;
    }

    void db_transaction(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsFunction())
        return (void)throw_type_error(iso, "Database.transaction expects a function");
      auto fn = info[0].As<Function>();
      auto base = make_txn_wrapper(iso, ctx, info.This(), fn, begin_kind::Default);
      auto deferred = make_txn_wrapper(iso, ctx, info.This(), fn, begin_kind::Deferred);
      auto immediate = make_txn_wrapper(iso, ctx, info.This(), fn, begin_kind::Immediate);
      auto exclusive = make_txn_wrapper(iso, ctx, info.This(), fn, begin_kind::Exclusive);
      (void)base->Set(ctx, "deferred"_v8(iso), deferred);
      (void)base->Set(ctx, "immediate"_v8(iso), immediate);
      (void)base->Set(ctx, "exclusive"_v8(iso), exclusive);
      (void)base->Set(ctx, "default"_v8(iso), base);
      info.GetReturnValue().Set(base);
    }

    void db_in_transaction(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* dh = unwrap_db(info.This());
      if (!dh || !dh->db || dh->closed) {
        info.GetReturnValue().Set(False(iso));
        return;
      }
      info.GetReturnValue().Set(Boolean::New(iso, !sqlite3_get_autocommit(dh->db)));
    }

    void db_get_filename(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* dh = unwrap_db(info.This());
      if (!dh || !dh->db) {
        info.GetReturnValue().Set(""_v8(iso));
        return;
      }
      const char* fn = sqlite3_db_filename(dh->db, "main");
      info.GetReturnValue().Set(s(iso, fn ? fn : ""));
    }

    void db_get_handle(const FunctionCallbackInfo<Value>& info) {
      // Bun exposes a numeric handle for FFI bridges; we surface 0 to keep the
      // shape but discourage external use.
      info.GetReturnValue().Set(0.0_v8(info.GetIsolate()));
    }

    // --- Database constructor / deserialize --------------------------------

    bool read_bool_opt(Isolate* iso, Local<Context> ctx, Local<Object> opts, const char* key,
                       bool def) {
      Local<Value> v;
      if (opts->Get(ctx, s(iso, key)).ToLocal(&v) && !v->IsUndefined())
        return v->BooleanValue(iso);
      return def;
    }

    void db_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall())
        return (void)throw_type_error(iso, "Database must be called with `new`");

      std::string filename = ":memory:";
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (info[0]->IsString()) {
          String::Utf8Value u(iso, info[0]);
          filename = *u ? std::string(*u, u.length()) : std::string{};
          if (filename.empty())
            filename = ":memory:";
        } else if (info[0]->IsNumber()) {
          // Bun accepts a flags integer in the first slot too; ignore it here
          // and use defaults.
        } else {
          return (void)throw_type_error(iso, "Database: filename must be a string");
        }
      }

      bool readonly = false, create = true, readwrite = true;
      bool safe_integers = false, strict = false;
      int flags_override = 0;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto opts = info[1].As<Object>();
        readonly = read_bool_opt(iso, ctx, opts, "readonly", false);
        create = read_bool_opt(iso, ctx, opts, "create", true);
        readwrite = read_bool_opt(iso, ctx, opts, "readwrite", true);
        safe_integers = read_bool_opt(iso, ctx, opts, "safeIntegers", false);
        strict = read_bool_opt(iso, ctx, opts, "strict", false);
      } else if (info.Length() >= 2 && info[1]->IsNumber()) {
        flags_override = static_cast<int>(info[1]->Int32Value(ctx).FromMaybe(0));
      }

      int flags = 0;
      if (flags_override) {
        flags = flags_override;
      } else {
        if (readonly) {
          flags = SQLITE_OPEN_READONLY;
        } else {
          flags = readwrite ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READWRITE;
          if (create)
            flags |= SQLITE_OPEN_CREATE;
        }
      }

      sqlite3* db = nullptr;
      int rc = sqlite3_open_v2(filename.c_str(), &db, flags, nullptr);
      if (rc != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        if (db)
          sqlite3_close_v2(db);
        (void)throw_error(iso, msg);
        return;
      }

      auto self = info.This();
      auto* dh = new db_holder{};
      dh->db = db;
      dh->safe_integers = safe_integers;
      dh->strict = strict;
      set_native(iso, self, dh, TAG_SQLITE_DATABASE);
      dh->bind(iso, self);

      // Stash filename for diagnostics.
      (void)self->Set(ctx, "filename"_v8(iso), s(iso, filename));
    }

    void db_static_deserialize(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsArrayBufferView())
        return (void)throw_type_error(iso, "Database.deserialize expects a typed array");
      auto view = info[0].As<ArrayBufferView>();
      bool readonly = false;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        readonly = read_bool_opt(iso, ctx, info[1].As<Object>(), "readonly", false);
      } else if (info.Length() >= 2 && info[1]->IsBoolean()) {
        readonly = info[1]->BooleanValue(iso);
      }

      sqlite3* db = nullptr;
      int rc = sqlite3_open(":memory:", &db);
      if (rc != SQLITE_OK) {
        if (db)
          sqlite3_close_v2(db);
        (void)throw_error(iso, "Database.deserialize: cannot open in-memory db");
        return;
      }
      const usize n = view->ByteLength();
      // Copy the bytes into a sqlite-managed allocation; sqlite takes ownership.
      auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(n ? n : 1));
      if (!copy) {
        sqlite3_close_v2(db);
        (void)throw_error(iso, "Database.deserialize: out of memory");
        return;
      }
      if (n > 0) {
        auto bs = view->Buffer()->GetBackingStore();
        std::memcpy(copy, static_cast<u8*>(bs->Data()) + view->ByteOffset(), n);
      }
      unsigned int dflags = SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE;
      if (readonly)
        dflags |= SQLITE_DESERIALIZE_READONLY;
      rc = sqlite3_deserialize(db, "main", copy, static_cast<sqlite3_int64>(n),
                               static_cast<sqlite3_int64>(n), dflags);
      if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close_v2(db);
        (void)throw_error(iso, msg);
        return;
      }

      // Wrap into a Database instance.
      auto tpl = db_tpl_table()[iso].Get(iso);
      auto fn = tpl->GetFunction(ctx).ToLocalChecked();
      auto self = fn->NewInstance(ctx, 0, nullptr).ToLocalChecked();
      // Replace the freshly-opened ":memory:" handle the constructor created
      // with our deserialized one.
      auto* prev = unwrap_db(self);
      if (prev)
        close_db_holder(prev);
      auto* dh = new db_holder{};
      dh->db = db;
      set_native(iso, self, dh, TAG_SQLITE_DATABASE);
      dh->bind(iso, self);
      info.GetReturnValue().Set(self);
    }

    void db_static_set_custom_sqlite(const FunctionCallbackInfo<Value>& info) {
      // No-op; we link against a fixed sqlite3 build.
      info.GetReturnValue().Set(Boolean::New(info.GetIsolate(), false));
    }

    // --- Template installation ----------------------------------------------

    Local<FunctionTemplate> build_statement_template(Isolate* iso) {
      auto tpl = FunctionTemplate::New(iso, stmt_constructor);
      tpl->SetClassName("Statement"_v8(iso));
      tpl->InstanceTemplate()->SetInternalFieldCount(2);
      auto proto = tpl->PrototypeTemplate();
      proto->Set(iso, "all", FunctionTemplate::New(iso, stmt_all));
      proto->Set(iso, "get", FunctionTemplate::New(iso, stmt_get));
      proto->Set(iso, "values", FunctionTemplate::New(iso, stmt_values));
      proto->Set(iso, "run", FunctionTemplate::New(iso, stmt_run));
      proto->Set(iso, "iterate", FunctionTemplate::New(iso, stmt_iterate));
      proto->Set(iso, "finalize", FunctionTemplate::New(iso, stmt_finalize));
      proto->Set(iso, "toString", FunctionTemplate::New(iso, stmt_to_string));
      proto->Set(iso, "as", FunctionTemplate::New(iso, stmt_as));
      proto->SetAccessorProperty("columnNames"_v8(iso),
                                 FunctionTemplate::New(iso, stmt_get_column_names));
      proto->SetAccessorProperty("paramsCount"_v8(iso),
                                 FunctionTemplate::New(iso, stmt_get_params_count));
      proto->SetAccessorProperty("native"_v8(iso), FunctionTemplate::New(iso, stmt_get_native));
      proto->Set(Symbol::GetIterator(iso), FunctionTemplate::New(iso, stmt_iterate));
      proto->Set(Symbol::GetDispose(iso), FunctionTemplate::New(iso, stmt_finalize));
      return tpl;
    }

    Local<FunctionTemplate> build_database_template(Isolate* iso) {
      auto tpl = FunctionTemplate::New(iso, db_constructor);
      tpl->SetClassName("Database"_v8(iso));
      tpl->InstanceTemplate()->SetInternalFieldCount(2);
      auto proto = tpl->PrototypeTemplate();
      proto->Set(iso, "query", FunctionTemplate::New(iso, db_query));
      proto->Set(iso, "prepare", FunctionTemplate::New(iso, db_prepare));
      proto->Set(iso, "run", FunctionTemplate::New(iso, db_run));
      proto->Set(iso, "exec", FunctionTemplate::New(iso, db_run));
      proto->Set(iso, "close", FunctionTemplate::New(iso, db_close));
      proto->Set(iso, "transaction", FunctionTemplate::New(iso, db_transaction));
      proto->Set(iso, "serialize", FunctionTemplate::New(iso, db_serialize));
      proto->Set(iso, "loadExtension", FunctionTemplate::New(iso, db_load_extension));
      proto->Set(iso, "fileControl", FunctionTemplate::New(iso, db_file_control));
      proto->Set(Symbol::GetDispose(iso), FunctionTemplate::New(iso, db_close));
      proto->SetAccessorProperty("inTransaction"_v8(iso),
                                 FunctionTemplate::New(iso, db_in_transaction));
      proto->SetAccessorProperty("filename"_v8(iso), FunctionTemplate::New(iso, db_get_filename));
      proto->SetAccessorProperty("handle"_v8(iso), FunctionTemplate::New(iso, db_get_handle));
      tpl->Set(iso, "deserialize", FunctionTemplate::New(iso, db_static_deserialize));
      tpl->Set(iso, "setCustomSQLite", FunctionTemplate::New(iso, db_static_set_custom_sqlite));
      return tpl;
    }

    Local<Object> build_constants(Isolate* iso) {
      auto o = Object::New(iso);
      auto ctx = iso->GetCurrentContext();
      auto put = [&](const char* name, int val) {
        (void)o->Set(ctx, s(iso, name), Integer::New(iso, val));
      };
      put("SQLITE_OPEN_READONLY", SQLITE_OPEN_READONLY);
      put("SQLITE_OPEN_READWRITE", SQLITE_OPEN_READWRITE);
      put("SQLITE_OPEN_CREATE", SQLITE_OPEN_CREATE);
      put("SQLITE_OPEN_FULLMUTEX", SQLITE_OPEN_FULLMUTEX);
      put("SQLITE_OPEN_URI", SQLITE_OPEN_URI);
      put("SQLITE_OPEN_MEMORY", SQLITE_OPEN_MEMORY);
      put("SQLITE_OPEN_NOMUTEX", SQLITE_OPEN_NOMUTEX);
      put("SQLITE_OPEN_SHAREDCACHE", SQLITE_OPEN_SHAREDCACHE);
      put("SQLITE_OPEN_PRIVATECACHE", SQLITE_OPEN_PRIVATECACHE);
      put("SQLITE_FCNTL_PERSIST_WAL", SQLITE_FCNTL_PERSIST_WAL);
      put("SQLITE_FCNTL_CHUNK_SIZE", SQLITE_FCNTL_CHUNK_SIZE);
      put("SQLITE_FCNTL_LOCKSTATE", SQLITE_FCNTL_LOCKSTATE);
      put("SQLITE_FCNTL_FILE_POINTER", SQLITE_FCNTL_FILE_POINTER);
      put("SQLITE_FCNTL_SYNC_OMITTED", SQLITE_FCNTL_SYNC_OMITTED);
      put("SQLITE_FCNTL_VFSNAME", SQLITE_FCNTL_VFSNAME);
      put("SQLITE_PREPARE_PERSISTENT", SQLITE_PREPARE_PERSISTENT);
      return o;
    }

    Local<Function> build_version_fn(Isolate* /*iso*/, Local<Context> ctx) {
      auto fn = Function::New(ctx, [](const FunctionCallbackInfo<Value>& info) {
                  info.GetReturnValue().Set(s(info.GetIsolate(), sqlite3_libversion()));
                }).ToLocalChecked();
      return fn;
    }
  } // namespace

  void install_sqlite_bindings(Isolate* iso, Local<ObjectTemplate> /*global*/) {
    HandleScope hs(iso);
    db_tpl_table()[iso].Reset(iso, build_database_template(iso));
    stmt_tpl_table()[iso].Reset(iso, build_statement_template(iso));
  }

  // Synthetic module body. Runs at module evaluation time. The closure
  // resolves the live FunctionTemplate per-isolate and exports the resulting
  // Function objects for `Database` and `constants`.
  namespace {
    MaybeLocal<Value> sqlite_module_evaluate(Local<Context> ctx, Local<Module> mod) {
      auto* iso = Isolate::GetCurrent();
      HandleScope hs(iso);
      auto db_fn_tpl = db_tpl_table()[iso].Get(iso);
      auto stmt_fn_tpl = stmt_tpl_table()[iso].Get(iso);
      Local<Function> db_fn;
      if (!db_fn_tpl->GetFunction(ctx).ToLocal(&db_fn))
        return MaybeLocal<Value>();
      Local<Function> stmt_fn;
      if (!stmt_fn_tpl->GetFunction(ctx).ToLocal(&stmt_fn))
        return MaybeLocal<Value>();
      auto consts = build_constants(iso);
      auto version = build_version_fn(iso, ctx);

      auto set = [&](const char* name, Local<Value> v) -> bool {
        auto m = mod->SetSyntheticModuleExport(iso, s(iso, name), v);
        return m.IsJust() && m.FromJust();
      };
      if (!set("Database", db_fn))
        return MaybeLocal<Value>();
      if (!set("Statement", stmt_fn))
        return MaybeLocal<Value>();
      if (!set("constants", consts))
        return MaybeLocal<Value>();
      if (!set("version", version))
        return MaybeLocal<Value>();
      if (!set("default", db_fn))
        return MaybeLocal<Value>();
      return Local<Value>(True(iso));
    }
  } // namespace

  MaybeLocal<Module> build_sqlite_module(Isolate* iso, Local<Context> /*ctx*/) {
    HandleScope hs(iso);
    std::array<Local<String>, 5> exports{
        "Database"_v8(iso), "Statement"_v8(iso), "constants"_v8(iso),
        "version"_v8(iso),  "default"_v8(iso),
    };
    MemorySpan<const Local<String>> span(exports.data(), exports.size());
    auto module_name = "fxe:sqlite"_v8(iso);
    return Module::CreateSyntheticModule(iso, module_name, span, sqlite_module_evaluate);
  }
} // namespace fxe::js
