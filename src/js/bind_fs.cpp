// JS bindings for a Node.js-compatible subset of `fs`. Sync variants do the
// work inline; async variants do the same work and resolve the returned
// Promise immediately (no thread pool yet — semantics are correct, latency
// is comparable to sync).
//
// Reads with no encoding return Uint8Array (raw bytes). We do NOT have Node
// Buffer, so this differs from Node.

#include "bind_fs.hpp"
#include "bind_timers.hpp"
#include "os/os.hpp"
#include "runtime/capabilities.hpp"
#include "runtime/uv_loop.hpp"
#include "runtime/v8/fs_watcher.hpp"
#include "weak_holder.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fxe/types.hpp>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <v8.h>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace fxe::js {
  namespace {
    using namespace v8;
    namespace fs = std::filesystem;

    // Map errno / std::error_code to a Node-ish code string.
    const char* node_error_code(const std::error_code& ec) {
      if (ec == std::errc::no_such_file_or_directory)
        return "ENOENT";
      if (ec == std::errc::file_exists)
        return "EEXIST";
      if (ec == std::errc::permission_denied)
        return "EACCES";
      if (ec == std::errc::not_a_directory)
        return "ENOTDIR";
      if (ec == std::errc::is_a_directory)
        return "EISDIR";
      if (ec == std::errc::invalid_argument)
        return "EINVAL";
      if (ec == std::errc::directory_not_empty)
        return "ENOTEMPTY";
      if (ec == std::errc::too_many_files_open)
        return "EMFILE";
      if (ec == std::errc::no_space_on_device)
        return "ENOSPC";
      if (ec == std::errc::not_enough_memory)
        return "ENOMEM";
      return "EIO";
    }

    Local<Value> make_error(Isolate* iso, const std::error_code& ec, std::string_view path,
                            const char* syscall) {
      auto ctx = iso->GetCurrentContext();
      std::string msg = std::string(syscall ? syscall : "fs") + ": " + ec.message();
      if (!path.empty()) {
        msg += " '";
        msg.append(path);
        msg += "'";
      }
      auto err = Exception::Error(to_v8_string(iso, msg)).As<Object>();
      set_prop(ctx, err, "code", node_error_code(ec));
      set_prop(ctx, err, "errno", ec.value());
      if (syscall)
        set_prop(ctx, err, "syscall", syscall);
      if (!path.empty())
        set_prop(ctx, err, "path", path);
      return err;
    }

    Local<Value> make_native_error(Isolate* iso, std::string_view message, std::string_view code,
                                   int errno_value, std::string_view syscall,
                                   std::string_view path) {
      auto ctx = iso->GetCurrentContext();
      auto err =
          Exception::Error(to_v8_string(iso, message.empty() ? "native operation failed" : message))
              .As<Object>();
      if (!code.empty())
        set_prop(ctx, err, "code", code);
      set_prop(ctx, err, "errno", errno_value);
      if (!syscall.empty())
        set_prop(ctx, err, "syscall", syscall);
      if (!path.empty())
        set_prop(ctx, err, "path", path);
      return err;
    }

    Local<Value> make_watch_error(Isolate* iso, const fxe::runtime::fs_watch_error& error,
                                  std::string_view fallback_path) {
      const std::string message =
          !error.message.empty() ? error.message : "fs.watch: failed to start watcher";
      return make_native_error(iso, message, error.code, error.errno_value, error.syscall,
                               !error.path.empty() ? std::string_view(error.path) : fallback_path);
    }

    void throw_fs_error(Isolate* iso, const std::error_code& ec, std::string_view path,
                        const char* syscall) {
      iso->ThrowException(make_error(iso, ec, path, syscall));
    }

    std::string fs_permission_message(std::string_view path) {
      std::string msg = "fs access denied for '";
      msg.append(path);
      msg += "'";
      return msg;
    }

    Local<Value> make_permission_denied(Isolate* iso, std::string_view what) {
      auto ctx = iso->GetCurrentContext();
      std::string msg = "Permission denied: ";
      msg.append(what);
      auto err = Exception::Error(to_v8_string(iso, msg)).As<Object>();
      set_prop(ctx, err, "name", "PermissionDenied");
      return err;
    }

    bool guard_fs(Isolate* iso, std::string_view path) {
      if (fxe::runtime::fs_path_allowed(path))
        return true;
      iso->ThrowException(make_permission_denied(iso, fs_permission_message(path)));
      return false;
    }

    // Extract a write payload from a v8::Value. Accepts string (UTF-8) or
    // any TypedArray / ArrayBuffer / DataView. Returns false if neither.
    bool extract_data(Isolate* iso, Local<Value> v, std::vector<u8>& out) {
      if (v->IsString()) {
        auto s = to_std_string(iso, v);
        out.assign(s.begin(), s.end());
        return true;
      }
      if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        usize n = view->ByteLength();
        out.resize(n);
        if (n)
          view->CopyContents(out.data(), n);
        return true;
      }
      if (v->IsArrayBuffer()) {
        auto ab = v.As<ArrayBuffer>();
        usize n = ab->ByteLength();
        out.resize(n);
        if (n)
          std::memcpy(out.data(), ab->Data(), n);
        return true;
      }
      return false;
    }

    // Distinguish utf8-encoded read from raw bytes.
    enum class read_encoding { raw, utf8 };

    read_encoding read_encoding_from(Isolate* iso, Local<Value> opts) {
      if (opts.IsEmpty() || opts->IsNullOrUndefined())
        return read_encoding::raw;
      if (opts->IsString()) {
        auto s = to_std_string(iso, opts);
        if (s == "utf8" || s == "utf-8")
          return read_encoding::utf8;
        return read_encoding::raw;
      }
      if (opts->IsObject()) {
        auto ctx = iso->GetCurrentContext();
        auto o = opts.As<Object>();
        if (auto v = get_prop<Local<Value>>(ctx, o, "encoding"); v && (*v)->IsString()) {
          auto s = to_std_string(iso, *v);
          if (s == "utf8" || s == "utf-8")
            return read_encoding::utf8;
        }
      }
      return read_encoding::raw;
    }

    bool bool_field(Isolate* iso, Local<Value> opts, const char* key, bool defv) {
      if (opts.IsEmpty() || !opts->IsObject())
        return defv;
      auto ctx = iso->GetCurrentContext();
      auto v = get_prop<Local<Value>>(ctx, opts.As<Object>(), key);
      if (!v || (*v)->IsUndefined())
        return defv;
      return (*v)->BooleanValue(iso);
    }

    // Reads file bytes. Sets ec on failure.
    std::vector<u8> read_all(const fs::path& p, std::error_code& ec) {
      std::ifstream f(p, std::ios::binary);
      if (!f) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
      }
      f.seekg(0, std::ios::end);
      auto sz = f.tellg();
      f.seekg(0, std::ios::beg);
      std::vector<u8> buf;
      if (sz > 0)
        buf.resize(static_cast<usize>(sz));
      if (sz > 0)
        f.read(reinterpret_cast<char*>(buf.data()), sz);
      if (f.bad()) {
        ec = std::make_error_code(std::errc::io_error);
        return {};
      }
      return buf;
    }

    bool write_all(const fs::path& p, const std::vector<u8>& data, bool append,
                   std::error_code& ec) {
      auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
      std::ofstream f(p, mode);
      if (!f) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
      }
      if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
      f.flush();
      if (f.bad()) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
      }
      return true;
    }

    // Build a Uint8Array that owns a freshly-allocated backing store.
    Local<Value> bytes_to_uint8(Isolate* iso, const std::vector<u8>& bytes) {
      auto store = ArrayBuffer::NewBackingStore(iso, bytes.size());
      if (!bytes.empty())
        std::memcpy(store->Data(), bytes.data(), bytes.size());
      auto ab = ArrayBuffer::New(iso, std::move(store));
      return Uint8Array::New(ab, 0, bytes.size());
    }

    Local<Object> stat_to_object(Isolate* iso, const fs::path& p, bool follow_symlinks,
                                 std::error_code& ec) {
      auto ctx = iso->GetCurrentContext();
      auto out = Object::New(iso);
      auto st = follow_symlinks ? fs::status(p, ec) : fs::symlink_status(p, ec);
      if (ec)
        return out;
      bool is_file = fs::is_regular_file(st);
      bool is_dir = fs::is_directory(st);
      bool is_symlink = fs::is_symlink(st);
      u64 size = 0;
      if (is_file) {
        size = static_cast<u64>(fs::file_size(p, ec));
        if (ec)
          return out;
      }
      double mtime_ms = 0;
      auto ftime = fs::last_write_time(p, ec);
      if (!ec) {
        // Not portably convertible to system_clock until C++20; cast via
        // duration_cast on the underlying type.
        auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::file_clock::to_sys(ftime));
        mtime_ms = static_cast<double>(sctp.time_since_epoch().count());
      } else {
        ec.clear();
      }
      set_prop(ctx, out, "size", static_cast<double>(size));
      set_prop(ctx, out, "isFile", is_file);
      set_prop(ctx, out, "isDirectory", is_dir);
      set_prop(ctx, out, "isSymbolicLink", is_symlink);
      set_prop(ctx, out, "mtimeMs", mtime_ms);
      set_prop(ctx, out, "atimeMs", mtime_ms);
      set_prop(ctx, out, "ctimeMs", mtime_ms);
      return out;
    }

    Local<Value> readdir_result(Isolate* iso, const fs::path& p, bool with_types,
                                std::error_code& ec) {
      auto ctx = iso->GetCurrentContext();
      std::vector<fs::directory_entry> entries;
      for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator{};
           it.increment(ec))
        entries.push_back(*it);
      if (ec)
        return Local<Value>();
      auto arr = Array::New(iso, static_cast<int>(entries.size()));
      u32 i = 0;
      for (const auto& e : entries) {
        auto name = e.path().filename().generic_string();
        if (with_types) {
          auto o = Object::New(iso);
          set_prop(ctx, o, "name", name);
          set_prop(ctx, o, "isFile", e.is_regular_file(ec));
          set_prop(ctx, o, "isDirectory", e.is_directory(ec));
          set_prop(ctx, o, "isSymbolicLink", e.is_symlink(ec));
          ec.clear();
          set_index(ctx, arr, i++, o);
        } else {
          set_index(ctx, arr, i++, name);
        }
      }
      return arr;
    }

    std::string string_field(Isolate* iso, Local<Value> opts, const char* key,
                             std::string_view defv) {
      if (opts.IsEmpty() || !opts->IsObject())
        return std::string(defv);
      auto ctx = iso->GetCurrentContext();
      auto v = get_prop<Local<Value>>(ctx, opts.As<Object>(), key);
      if (!v || !(*v)->IsString())
        return std::string(defv);
      return to_std_string(iso, *v);
    }

    std::error_code errno_error() {
#if defined(_WIN32)
      return std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
      return std::error_code(errno, std::generic_category());
#endif
    }

    fs::path guarded_symlink_target_path(const std::string& target, const std::string& link_path) {
      fs::path target_path(target);
      if (target_path.is_absolute())
        return target_path;
      fs::path parent = fs::path(link_path).parent_path();
      if (parent.empty())
        parent = fs::current_path();
      return (parent / target_path).lexically_normal();
    }

    bool copy_tree(const fs::path& src, const fs::path& dest, bool dereference,
                   std::error_code& ec) {
      auto source_status = dereference ? fs::status(src, ec) : fs::symlink_status(src, ec);
      if (ec)
        return false;
      if (fs::is_symlink(source_status) && !dereference) {
        auto target = fs::read_symlink(src, ec);
        if (ec)
          return false;
        fs::create_symlink(target, dest, ec);
        return !ec;
      }
      if (fs::is_directory(source_status)) {
        fs::create_directories(dest, ec);
        if (ec)
          return false;
        fs::permissions(dest, source_status.permissions(), fs::perm_options::replace, ec);
        ec.clear();
        for (fs::directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
          if (!copy_tree(it->path(), dest / it->path().filename(), dereference, ec))
            return false;
        }
        return !ec;
      }
      if (fs::is_regular_file(source_status)) {
        fs::create_directories(dest.parent_path(), ec);
        if (ec)
          return false;
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (!ec)
          fs::permissions(dest, source_status.permissions(), fs::perm_options::replace, ec);
        return !ec;
      }
      ec = std::make_error_code(std::errc::operation_not_supported);
      return false;
    }

    bool set_file_times(const fs::path& p, double atime_seconds, double mtime_seconds,
                        bool nofollow, std::error_code& ec) {
#if defined(_WIN32)
      (void)atime_seconds;
      (void)nofollow;
      auto seconds = std::chrono::duration<double>(mtime_seconds);
      auto sys_time = std::chrono::time_point<std::chrono::system_clock>(
          std::chrono::duration_cast<std::chrono::system_clock::duration>(seconds));
      auto file_time = std::chrono::file_clock::from_sys(sys_time);
      fs::last_write_time(p, file_time, ec);
      return !ec;
#else
      timeval times[2]{};
      times[0].tv_sec = static_cast<time_t>(atime_seconds);
      times[0].tv_usec = static_cast<suseconds_t>((atime_seconds - times[0].tv_sec) * 1000000.0);
      times[1].tv_sec = static_cast<time_t>(mtime_seconds);
      times[1].tv_usec = static_cast<suseconds_t>((mtime_seconds - times[1].tv_sec) * 1000000.0);
      int rc = nofollow ? ::lutimes(p.c_str(), times) : ::utimes(p.c_str(), times);
      if (rc != 0) {
        ec = errno_error();
        return false;
      }
      return true;
#endif
    }

    double time_arg_seconds([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> v) {
      double n = v->NumberValue(ctx).FromMaybe(0);
      if (v->IsDate())
        return n / 1000.0;
      return n;
    }

    bool chmod_path(const fs::path& p, int mode, bool nofollow, std::error_code& ec) {
#if defined(_WIN32)
      (void)nofollow;
      DWORD attrs = GetFileAttributesW(p.wstring().c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        ec = errno_error();
        return false;
      }
      if (mode & 0222)
        attrs &= ~FILE_ATTRIBUTE_READONLY;
      else
        attrs |= FILE_ATTRIBUTE_READONLY;
      if (!SetFileAttributesW(p.wstring().c_str(), attrs)) {
        ec = errno_error();
        return false;
      }
      return true;
#else
      int rc = 0;
#if defined(__APPLE__)
      rc = nofollow ? ::lchmod(p.c_str(), static_cast<mode_t>(mode))
                    : ::chmod(p.c_str(), static_cast<mode_t>(mode));
#else
      if (nofollow) {
        ec = std::make_error_code(std::errc::operation_not_supported);
        return false;
      }
      rc = ::chmod(p.c_str(), static_cast<mode_t>(mode));
#endif
      if (rc != 0) {
        ec = errno_error();
        return false;
      }
      return true;
#endif
    }

    bool access_path(const fs::path& p, int mode, std::error_code& ec) {
#if defined(_WIN32)
      int win_mode = 0;
      if (mode & 2)
        win_mode |= 2;
      if (mode & 4)
        win_mode |= 4;
      if (_waccess(p.wstring().c_str(), win_mode) != 0) {
        ec = errno_error();
        return false;
      }
      return true;
#else
      if (::access(p.c_str(), mode) != 0) {
        ec = errno_error();
        return false;
      }
      return true;
#endif
    }

    bool chown_path(const fs::path& p, int uid, int gid, std::error_code& ec) {
#if defined(_WIN32)
      (void)p;
      (void)uid;
      (void)gid;
      ec = std::make_error_code(std::errc::operation_not_supported);
      return false;
#else
      if (::chown(p.c_str(), static_cast<uid_t>(uid), static_cast<gid_t>(gid)) != 0) {
        ec = errno_error();
        return false;
      }
      return true;
#endif
    }

    std::string random_temp_path(const fs::path& p) {
      static std::atomic<u64> counter{0};
      auto tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      std::ostringstream out;
      out << p.generic_string() << ".tmp." << tick << "." << counter.fetch_add(1);
      return std::move(out).str();
    }

    std::vector<std::string> split_pattern(std::string pattern) {
      std::replace(pattern.begin(), pattern.end(), '\\', '/');
      std::vector<std::string> parts;
      std::string current;
      for (char ch : pattern) {
        if (ch == '/') {
          if (!current.empty()) {
            parts.push_back(current);
            current.clear();
          }
        } else {
          current.push_back(ch);
        }
      }
      if (!current.empty())
        parts.push_back(current);
      return parts;
    }

    bool match_component(std::string_view pat, std::string_view text) {
      usize pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
      auto char_match = [&](usize& pidx, char c) {
        if (pidx >= pat.size())
          return false;
        if (pat[pidx] == '?') {
          ++pidx;
          return true;
        }
        if (pat[pidx] == '[') {
          ++pidx;
          bool negate = pidx < pat.size() && (pat[pidx] == '!' || pat[pidx] == '^');
          if (negate)
            ++pidx;
          bool ok = false;
          while (pidx < pat.size() && pat[pidx] != ']') {
            ok = ok || pat[pidx] == c;
            ++pidx;
          }
          if (pidx < pat.size() && pat[pidx] == ']')
            ++pidx;
          return negate ? !ok : ok;
        }
        if (pat[pidx] == c) {
          ++pidx;
          return true;
        }
        return false;
      };
      while (ti < text.size()) {
        if (pi < pat.size() && pat[pi] == '*') {
          star = pi++;
          mark = ti;
        } else {
          usize next = pi;
          if (char_match(next, text[ti])) {
            pi = next;
            ++ti;
          } else if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++mark;
          } else {
            return false;
          }
        }
      }
      while (pi < pat.size() && pat[pi] == '*')
        ++pi;
      return pi == pat.size();
    }

    bool match_parts(const std::vector<std::string>& pat, usize pi,
                     const std::vector<std::string>& parts, usize si) {
      if (pi == pat.size())
        return si == parts.size();
      if (pat[pi] == "**") {
        for (usize i = si; i <= parts.size(); ++i) {
          if (match_parts(pat, pi + 1, parts, i))
            return true;
        }
        return false;
      }
      return si < parts.size() && match_component(pat[pi], parts[si]) &&
             match_parts(pat, pi + 1, parts, si + 1);
    }

    Local<Array> string_array(Isolate* iso, const std::vector<std::string>& values) {
      auto ctx = iso->GetCurrentContext();
      auto arr = Array::New(iso, static_cast<int>(values.size()));
      for (u32 i = 0; i < values.size(); ++i)
        set_index(ctx, arr, i, values[i]);
      return arr;
    }

    bool glob_entries(Isolate* iso, const std::string& pattern, Local<Value> opts,
                      std::vector<std::string>& out, std::error_code& ec,
                      std::string& denied_path) {
      auto cwd_text = string_field(iso, opts, "cwd", ".");
      fs::path cwd(cwd_text);
      auto patterns = split_pattern(pattern);
      if (patterns.empty())
        return true;
      if (!guard_fs(iso, cwd.generic_string()))
        return false;
      fs::recursive_directory_iterator it(cwd, fs::directory_options::skip_permission_denied, ec);
      fs::recursive_directory_iterator end;
      for (; !ec && it != end; it.increment(ec)) {
        auto candidate = it->path();
        if (!fxe::runtime::fs_path_allowed(candidate.generic_string())) {
          denied_path = candidate.generic_string();
          return false;
        }
        auto rel = fs::relative(candidate, cwd, ec);
        if (ec)
          return false;
        auto rel_text = rel.generic_string();
        auto rel_parts = split_pattern(rel_text);
        if (match_parts(patterns, 0, rel_parts, 0))
          out.push_back(rel_text);
      }
      std::sort(out.begin(), out.end());
      return !ec;
    }

    struct glob_iter_state : weak_holder<glob_iter_state> {
      std::vector<std::string> entries;
      usize index = 0;
    };

    glob_iter_state* glob_iter_from(Local<Object> self) {
      if (self->InternalFieldCount() < 1)
        return nullptr;
      return external_ptr<glob_iter_state>(self->GetInternalField(0));
    }

    void glob_iter_async_iterator(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(info.This());
    }

    void glob_iter_next(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto result = Object::New(iso);
      auto* state = glob_iter_from(info.This());
      if (!state || state->index >= state->entries.size()) {
        set_prop(ctx, result, "done", true);
      } else {
        set_prop(ctx, result, "done", false);
        set_prop(ctx, result, "value", state->entries[state->index++]);
      }
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Resolve(ctx, result);
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    Local<Object> make_glob_iterator(Isolate* iso, std::vector<std::string> entries) {
      auto ctx = iso->GetCurrentContext();
      auto tpl = ObjectTemplate::New(iso);
      tpl->SetInternalFieldCount(1);
      tpl->Set(iso, "next", FunctionTemplate::New(iso, glob_iter_next));
      auto obj = tpl->NewInstance(ctx).ToLocalChecked();
      auto* state = new glob_iter_state();
      state->entries = std::move(entries);
      obj->SetInternalField(0, make_external(iso, state));
      state->bind(iso, obj);
      auto fn = Function::New(ctx, glob_iter_async_iterator).ToLocalChecked();
      set_prop(ctx, obj, Symbol::GetAsyncIterator(iso), fn);
      return obj;
    }

    // === sync entrypoints ===

    void fs_read_file_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "readFileSync(path, opts?)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      auto enc = info.Length() >= 2 ? read_encoding_from(iso, info[1]) : read_encoding::raw;
      std::error_code ec;
      auto bytes = read_all(p, ec);
      if (ec) {
        throw_fs_error(iso, ec, p, "open");
        return;
      }
      if (enc == read_encoding::utf8) {
        info.GetReturnValue().Set(to_v8_string(
            iso, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
      } else {
        info.GetReturnValue().Set(bytes_to_uint8(iso, bytes));
      }
    }

    void fs_write_file_sync_impl(const FunctionCallbackInfo<Value>& info, bool append) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "writeFileSync(path, data)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::vector<u8> data;
      if (!extract_data(iso, info[1], data)) {
        (void)throw_type_error(iso, "data must be string or TypedArray");
        return;
      }
      std::error_code ec;
      if (!write_all(p, data, append, ec))
        throw_fs_error(iso, ec, p, append ? "appendFile" : "writeFile");
    }

    void fs_write_file_sync(const FunctionCallbackInfo<Value>& info) {
      fs_write_file_sync_impl(info, false);
    }
    void fs_append_file_sync(const FunctionCallbackInfo<Value>& info) {
      fs_write_file_sync_impl(info, true);
    }

    void fs_exists_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      bool ok = fs::exists(fs::path(p), ec);
      info.GetReturnValue().Set(ok && !ec);
    }

    void fs_stat_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "statSync(path)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      auto out = stat_to_object(iso, fs::path(p), true, ec);
      if (ec) {
        throw_fs_error(iso, ec, p, "stat");
        return;
      }
      info.GetReturnValue().Set(out);
    }

    void fs_readdir_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "readdirSync(path, opts?)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      bool with_types =
          info.Length() >= 2 ? bool_field(iso, info[1], "withFileTypes", false) : false;
      std::error_code ec;
      auto v = readdir_result(iso, fs::path(p), with_types, ec);
      if (ec) {
        throw_fs_error(iso, ec, p, "scandir");
        return;
      }
      info.GetReturnValue().Set(v);
    }

    void fs_mkdir_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "mkdirSync(path, {recursive?})");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      bool recursive = info.Length() >= 2 ? bool_field(iso, info[1], "recursive", false) : false;
      std::error_code ec;
      bool ok = recursive ? fs::create_directories(fs::path(p), ec)
                          : fs::create_directory(fs::path(p), ec);
      (void)ok;
      if (ec)
        throw_fs_error(iso, ec, p, "mkdir");
    }

    void fs_rm_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "rmSync(path, {recursive?, force?})");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      bool recursive = info.Length() >= 2 ? bool_field(iso, info[1], "recursive", false) : false;
      bool force = info.Length() >= 2 ? bool_field(iso, info[1], "force", false) : false;
      std::error_code ec;
      if (recursive)
        fs::remove_all(fs::path(p), ec);
      else
        fs::remove(fs::path(p), ec);
      if (ec && !force)
        throw_fs_error(iso, ec, p, "unlink");
    }

    void fs_rename_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "renameSync(from, to)");
        return;
      }
      auto from = to_std_string(iso, info[0]);
      auto to = to_std_string(iso, info[1]);
      if (!guard_fs(iso, from) || !guard_fs(iso, to))
        return;
      std::error_code ec;
      fs::rename(fs::path(from), fs::path(to), ec);
      if (ec)
        throw_fs_error(iso, ec, from, "rename");
    }

    void fs_realpath_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "realpathSync(path)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      auto canonical = fs::weakly_canonical(fs::path(p), ec);
      if (ec) {
        throw_fs_error(iso, ec, p, "realpath");
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, canonical.generic_string()));
    }

    void fs_copy_file_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "copyFileSync(src, dest)");
        return;
      }
      auto src = to_std_string(iso, info[0]);
      auto dest = to_std_string(iso, info[1]);
      if (!guard_fs(iso, src) || !guard_fs(iso, dest))
        return;
      std::error_code ec;
      fs::copy_file(fs::path(src), fs::path(dest), fs::copy_options::overwrite_existing, ec);
      if (ec)
        throw_fs_error(iso, ec, src, "copyfile");
    }

    void fs_cp_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "cpSync(src, dest, opts?)");
        return;
      }
      auto src = to_std_string(iso, info[0]);
      auto dest = to_std_string(iso, info[1]);
      if (!guard_fs(iso, src) || !guard_fs(iso, dest))
        return;
      bool dereference =
          info.Length() >= 3 ? bool_field(iso, info[2], "dereference", false) : false;
      std::error_code ec;
      if (!copy_tree(fs::path(src), fs::path(dest), dereference, ec))
        throw_fs_error(iso, ec, src, "cp");
    }

    void fs_symlink_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "symlinkSync(target, path, type?)");
        return;
      }
      auto target = to_std_string(iso, info[0]);
      auto link_path = to_std_string(iso, info[1]);
      auto guarded_target = guarded_symlink_target_path(target, link_path).generic_string();
      if (!guard_fs(iso, guarded_target) || !guard_fs(iso, link_path))
        return;
      bool dir_link = false;
      if (info.Length() >= 3 && info[2]->IsString()) {
        auto type = info[2].As<String>();
        dir_link = type == "dir"_v8 || type == "junction"_v8;
      }
      std::error_code ec;
      if (dir_link)
        fs::create_directory_symlink(fs::path(target), fs::path(link_path), ec);
      else
        fs::create_symlink(fs::path(target), fs::path(link_path), ec);
      if (ec)
        throw_fs_error(iso, ec, link_path, "symlink");
    }

    void fs_readlink_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "readlinkSync(path)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      auto target = fs::read_symlink(fs::path(p), ec);
      if (ec) {
        throw_fs_error(iso, ec, p, "readlink");
        return;
      }
      info.GetReturnValue().Set(to_v8_string(iso, target.generic_string()));
    }

    void fs_link_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "linkSync(existing, path)");
        return;
      }
      auto existing = to_std_string(iso, info[0]);
      auto new_path = to_std_string(iso, info[1]);
      if (!guard_fs(iso, existing) || !guard_fs(iso, new_path))
        return;
      std::error_code ec;
      fs::create_hard_link(fs::path(existing), fs::path(new_path), ec);
      if (ec)
        throw_fs_error(iso, ec, existing, "link");
    }

    void fs_lstat_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "lstatSync(path)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      auto out = stat_to_object(iso, fs::path(p), false, ec);
      if (ec) {
        throw_fs_error(iso, ec, p, "lstat");
        return;
      }
      info.GetReturnValue().Set(out);
    }

    void fs_access_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "accessSync(path, mode?)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      int mode = info.Length() >= 2 ? info[1]->Int32Value(ctx).FromMaybe(0) : 0;
      std::error_code ec;
      if (!access_path(fs::path(p), mode, ec))
        throw_fs_error(iso, ec, p, "access");
    }

    void fs_chmod_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "chmodSync(path, mode)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      if (!chmod_path(fs::path(p), info[1]->Int32Value(ctx).FromMaybe(0), false, ec))
        throw_fs_error(iso, ec, p, "chmod");
    }

    void fs_lchmod_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "lchmodSync(path, mode)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      if (!chmod_path(fs::path(p), info[1]->Int32Value(ctx).FromMaybe(0), true, ec))
        throw_fs_error(iso, ec, p, "lchmod");
    }

    void fs_chown_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "chownSync(path, uid, gid)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      if (!chown_path(fs::path(p), info[1]->Int32Value(ctx).FromMaybe(-1),
                      info[2]->Int32Value(ctx).FromMaybe(-1), ec))
        throw_fs_error(iso, ec, p, "chown");
    }

    void fs_utimes_sync_impl(const FunctionCallbackInfo<Value>& info, bool nofollow) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString()) {
        (void)throw_type_error(iso, nofollow ? "lutimesSync(path, atime, mtime)"
                                             : "utimesSync(path, atime, mtime)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::error_code ec;
      if (!set_file_times(fs::path(p), time_arg_seconds(iso, ctx, info[1]),
                          time_arg_seconds(iso, ctx, info[2]), nofollow, ec))
        throw_fs_error(iso, ec, p, nofollow ? "lutimes" : "utimes");
    }

    void fs_utimes_sync(const FunctionCallbackInfo<Value>& info) {
      fs_utimes_sync_impl(info, false);
    }
    void fs_lutimes_sync(const FunctionCallbackInfo<Value>& info) {
      fs_utimes_sync_impl(info, true);
    }

    void fs_write_file_atomic_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "writeFileAtomicSync(path, data)");
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs(iso, p))
        return;
      std::vector<u8> data;
      if (!extract_data(iso, info[1], data)) {
        (void)throw_type_error(iso, "data must be string or TypedArray");
        return;
      }
      auto tmp = random_temp_path(fs::path(p));
      std::error_code ec;
      if (!write_all(tmp, data, false, ec)) {
        std::error_code cleanup_ec;
        fs::remove(fs::path(tmp), cleanup_ec);
        throw_fs_error(iso, ec, p, "writeFileAtomic");
        return;
      }
      fs::rename(fs::path(tmp), fs::path(p), ec);
      if (ec) {
        std::error_code cleanup_ec;
        fs::remove(fs::path(tmp), cleanup_ec);
        throw_fs_error(iso, ec, p, "rename");
      }
    }

    void fs_glob_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "globSync(pattern, opts?)");
        return;
      }
      std::vector<std::string> entries;
      std::error_code ec;
      std::string denied;
      auto pattern = to_std_string(iso, info[0]);
      if (!glob_entries(iso, pattern, info.Length() >= 2 ? info[1] : Undefined(iso), entries, ec,
                        denied)) {
        if (!denied.empty())
          iso->ThrowException(make_permission_denied(iso, fs_permission_message(denied)));
        else if (ec)
          throw_fs_error(iso, ec, pattern, "glob");
        return;
      }
      info.GetReturnValue().Set(string_array(iso, entries));
    }

    void fs_glob(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "glob(pattern, opts?)");
        return;
      }
      std::vector<std::string> entries;
      std::error_code ec;
      std::string denied;
      auto pattern = to_std_string(iso, info[0]);
      if (!glob_entries(iso, pattern, info.Length() >= 2 ? info[1] : Undefined(iso), entries, ec,
                        denied)) {
        if (!denied.empty())
          iso->ThrowException(make_permission_denied(iso, fs_permission_message(denied)));
        else if (ec)
          throw_fs_error(iso, ec, pattern, "glob");
        return;
      }
      info.GetReturnValue().Set(make_glob_iterator(iso, std::move(entries)));
    }

    void fs_lock_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsInt32()) {
        (void)throw_type_error(iso, "lockSync(fd, opts?)");
        return;
      }
      int fd = info[0]->Int32Value(ctx).FromMaybe(-1);
      bool exclusive = info.Length() >= 2 ? bool_field(iso, info[1], "exclusive", true) : true;
      bool non_blocking =
          info.Length() >= 2 ? bool_field(iso, info[1], "nonBlocking", false) : false;
#if defined(_WIN32)
      HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
      OVERLAPPED ov{};
      DWORD flags = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                    (non_blocking ? LOCKFILE_FAIL_IMMEDIATELY : 0);
      if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov))
        throw_fs_error(iso, errno_error(), "", "lock");
#else
      int op = exclusive ? LOCK_EX : LOCK_SH;
      if (non_blocking)
        op |= LOCK_NB;
      if (::flock(fd, op) != 0)
        throw_fs_error(iso, errno_error(), "", "flock");
#endif
    }

    void fs_unlock_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsInt32()) {
        (void)throw_type_error(iso, "unlockSync(fd)");
        return;
      }
      int fd = info[0]->Int32Value(ctx).FromMaybe(-1);
#if defined(_WIN32)
      HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
      OVERLAPPED ov{};
      if (!UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov))
        throw_fs_error(iso, errno_error(), "", "unlock");
#else
      if (::flock(fd, LOCK_UN) != 0)
        throw_fs_error(iso, errno_error(), "", "flock");
#endif
    }
    // === async helpers (libuv thread pool) ===

    Local<Promise> rejected([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> err) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Reject(ctx, err);
      return resolver->GetPromise();
    }

    bool guard_fs_async(Isolate* iso, Local<Context> ctx, const FunctionCallbackInfo<Value>& info,
                        std::string_view path) {
      TryCatch try_catch(iso);
      if (guard_fs(iso, path))
        return true;
      auto err = try_catch.Exception();
      try_catch.Reset();
      info.GetReturnValue().Set(rejected(iso, ctx, err));
      return false;
    }

    struct fs_stat_record {
      u64 size = 0;
      bool is_file = false;
      bool is_dir = false;
      double mtime_ms = 0;
    };

    struct fs_dir_entry_record {
      std::string name;
      bool is_file = false;
      bool is_dir = false;
    };

    bool collect_stat_record(const fs::path& p, fs_stat_record& out, std::error_code& ec,
                             bool follow_symlinks = true) {
      auto st = follow_symlinks ? fs::status(p, ec) : fs::symlink_status(p, ec);
      if (ec)
        return false;
      out.is_file = fs::is_regular_file(st);
      out.is_dir = fs::is_directory(st);
      out.size = 0;
      if (out.is_file) {
        out.size = static_cast<u64>(fs::file_size(p, ec));
        if (ec)
          return false;
      }
      auto ftime = fs::last_write_time(p, ec);
      if (!ec) {
        auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::file_clock::to_sys(ftime));
        out.mtime_ms = static_cast<double>(sctp.time_since_epoch().count());
      } else {
        ec.clear();
      }
      return true;
    }

    Local<Object> stat_record_to_object(Isolate* iso, const fs_stat_record& st) {
      auto ctx = iso->GetCurrentContext();
      auto out = Object::New(iso);
      set_prop(ctx, out, "size", static_cast<double>(st.size));
      set_prop(ctx, out, "isFile", st.is_file);
      set_prop(ctx, out, "isDirectory", st.is_dir);
      set_prop(ctx, out, "mtimeMs", st.mtime_ms);
      set_prop(ctx, out, "atimeMs", st.mtime_ms);
      set_prop(ctx, out, "ctimeMs", st.mtime_ms);
      return out;
    }

    bool collect_readdir_records(const fs::path& p, bool with_types,
                                 std::vector<fs_dir_entry_record>& out, std::error_code& ec) {
      for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator{};
           it.increment(ec)) {
        fs_dir_entry_record entry;
        entry.name = it->path().filename().generic_string();
        if (with_types) {
          entry.is_file = it->is_regular_file(ec);
          if (ec)
            ec.clear();
          entry.is_dir = it->is_directory(ec);
          if (ec)
            ec.clear();
        }
        out.push_back(std::move(entry));
      }
      return !ec;
    }

    Local<Value> readdir_records_to_value(Isolate* iso,
                                          const std::vector<fs_dir_entry_record>& entries,
                                          bool with_types) {
      auto ctx = iso->GetCurrentContext();
      auto arr = Array::New(iso, static_cast<int>(entries.size()));
      u32 i = 0;
      for (const auto& entry : entries) {
        if (with_types) {
          auto o = Object::New(iso);
          set_prop(ctx, o, "name", entry.name);
          set_prop(ctx, o, "isFile", entry.is_file);
          set_prop(ctx, o, "isDirectory", entry.is_dir);
          set_index(ctx, arr, i++, o);
        } else {
          set_index(ctx, arr, i++, entry.name);
        }
      }
      return arr;
    }

    enum class fs_async_kind {
      read_file,
      write_file,
      append_file,
      stat,
      readdir,
      mkdir,
      rm,
      rename,
      realpath,
      exists,
      copy_file,
      cp,
      symlink,
      readlink,
      link,
      lstat,
      access,
      chmod,
      lchmod,
      chown,
      utimes,
      lutimes,
      write_file_atomic,
      lock,
      unlock,
    };

    struct fs_cancel_token {
      // 0 queued, 1 syscall started, 2 aborted before syscall start, 3 completed.
      std::atomic<int> phase{0};
      std::atomic_bool aborted{false};
    };

    struct fs_abort_listener_ctx {
      std::shared_ptr<fs_cancel_token> token;
      Global<Function>* weak_fn = nullptr;
    };

    void fs_abort_listener_finalizer(const WeakCallbackInfo<fs_abort_listener_ctx>& info) {
      auto* ctx = info.GetParameter();
      if (!ctx)
        return;
      if (ctx->weak_fn) {
        ctx->weak_fn->Reset();
        delete ctx->weak_fn;
      }
      delete ctx;
    }

    struct fs_async_work {
#if FXE_HAS_LIBUV
      uv_work_t req{};
#endif
      Isolate* iso = nullptr;
      Global<Context> context;
      Global<Promise::Resolver> resolver;
      Global<Object> signal;
      Global<Function> abort_listener;
      std::shared_ptr<fs_cancel_token> cancel = std::make_shared<fs_cancel_token>();
      fs_async_kind kind = fs_async_kind::exists;
      std::string path;
      std::string path2;
      std::string syscall;
      read_encoding encoding = read_encoding::raw;
      std::vector<u8> data;
      std::vector<u8> bytes;
      fs_stat_record stat;
      std::vector<fs_dir_entry_record> entries;
      std::string text;
      bool recursive = false;
      bool force = false;
      bool with_types = false;
      bool bool_result = false;
      bool dereference = false;
      bool exclusive = true;
      bool non_blocking = false;
      int mode = 0;
      int uid = -1;
      int gid = -1;
      int fd = -1;
      double atime_seconds = 0;
      double mtime_seconds = 0;
      std::error_code ec;
    };

    Local<Value> make_async_error(Isolate* iso, const std::error_code& ec, std::string_view path,
                                  const char* syscall) {
      auto ctx = iso->GetCurrentContext();
      auto err = make_error(iso, ec, path, syscall).As<Object>();
      set_prop(ctx, err, "errno", ec.value());
      return err;
    }

#if FXE_HAS_LIBUV
    Local<Value> make_uv_error(Isolate* iso, int status, std::string_view path,
                               const char* syscall) {
      auto ctx = iso->GetCurrentContext();
      const char* code = uv_err_name(status);
      const char* message = uv_strerror(status);
      std::string msg =
          std::string(syscall ? syscall : "uv") + ": " + (message ? message : "error");
      if (!path.empty()) {
        msg += " '";
        msg.append(path);
        msg += "'";
      }
      auto err = Exception::Error(to_v8_string(iso, msg)).As<Object>();
      set_prop(ctx, err, "code", code ? code : "EIO");
      set_prop(ctx, err, "errno", status);
      if (syscall)
        set_prop(ctx, err, "syscall", syscall);
      if (!path.empty())
        set_prop(ctx, err, "path", path);
      return err;
    }
#endif

    Local<Value> make_abort_error(Isolate* iso, std::string_view reason = {}) {
      auto ctx = iso->GetCurrentContext();
      std::string msg = "The operation was aborted";
      if (!reason.empty()) {
        msg += ": ";
        msg.append(reason);
      }
      auto err = Exception::Error(to_v8_string(iso, msg)).As<Object>();
      set_prop(ctx, err, "name", "AbortError");
      set_prop(ctx, err, "code", "ABORT_ERR");
      return err;
    }

    Local<Object> signal_from_options([[maybe_unused]] Isolate* iso, Local<Context> ctx,
                                      const FunctionCallbackInfo<Value>& info, int index) {
      if (info.Length() <= index || !info[index]->IsObject())
        return Local<Object>();
      auto signal_value = get_prop<Local<Value>>(ctx, info[index].As<Object>(), "signal");
      if (!signal_value || !(*signal_value)->IsObject())
        return Local<Object>();
      return signal_value->As<Object>();
    }

    bool signal_is_aborted(Isolate* iso, Local<Context> ctx, Local<Object> signal) {
      if (signal.IsEmpty())
        return false;
      auto aborted = get_prop<Local<Value>>(ctx, signal, "aborted");
      if (!aborted)
        return false;
      return (*aborted)->BooleanValue(iso);
    }

    std::string signal_reason(Isolate* iso, Local<Context> ctx, Local<Object> signal) {
      if (signal.IsEmpty())
        return {};
      auto reason = get_prop<Local<Value>>(ctx, signal, "reason");
      if (!reason || (*reason)->IsUndefined() || (*reason)->IsNull())
        return {};
      return to_std_string(iso, *reason);
    }

    void install_abort_listener(Isolate* iso, Local<Context> ctx, fs_async_work& work,
                                Local<Object> signal) {
      if (signal.IsEmpty())
        return;
      auto add_value = get_prop<Local<Value>>(ctx, signal, "addEventListener");
      if (!add_value || !(*add_value)->IsFunction())
        return;
      auto* listener_ctx = new fs_abort_listener_ctx{work.cancel, nullptr};
      auto data = make_external(iso, listener_ctx);
      auto listener_maybe = Function::New(
          ctx,
          [](const FunctionCallbackInfo<Value>& cb_info) {
            auto* listener_ctx = external_ptr<fs_abort_listener_ctx>(cb_info.Data());
            if (!listener_ctx || !listener_ctx->token)
              return;
            listener_ctx->token->aborted.store(true, std::memory_order_release);
            int expected = 0;
            (void)listener_ctx->token->phase.compare_exchange_strong(
                expected, 2, std::memory_order_acq_rel, std::memory_order_acquire);
          },
          data);
      if (listener_maybe.IsEmpty()) {
        delete listener_ctx;
        return;
      }
      auto listener = listener_maybe.ToLocalChecked();
      Local<Value> argv[] = {"abort"_v8(iso), listener};
      TryCatch try_catch(iso);
      Local<Value> ignored;
      (void)(*add_value).As<Function>()->Call(ctx, signal, 2, argv).ToLocal(&ignored);
      if (try_catch.HasCaught()) {
        try_catch.Reset();
        delete listener_ctx;
        return;
      }
      work.signal.Reset(iso, signal);
      work.abort_listener.Reset(iso, listener);
      auto* weak_fn = new Global<Function>(iso, listener);
      listener_ctx->weak_fn = weak_fn;
      weak_fn->SetWeak(listener_ctx, fs_abort_listener_finalizer, WeakCallbackType::kParameter);
    }

    void remove_abort_listener(Isolate* iso, Local<Context> ctx, fs_async_work& work) {
      if (work.signal.IsEmpty() || work.abort_listener.IsEmpty())
        return;
      auto signal = work.signal.Get(iso);
      auto remove_value = get_prop<Local<Value>>(ctx, signal, "removeEventListener");
      if (!remove_value || !(*remove_value)->IsFunction()) {
        work.abort_listener.Reset();
        work.signal.Reset();
        return;
      }
      auto listener = work.abort_listener.Get(iso);
      Local<Value> argv[] = {"abort"_v8(iso), listener};
      TryCatch try_catch(iso);
      Local<Value> ignored;
      (void)(*remove_value).As<Function>()->Call(ctx, signal, 2, argv).ToLocal(&ignored);
      if (try_catch.HasCaught())
        try_catch.Reset();
      work.abort_listener.Reset();
      work.signal.Reset();
    }

    void perform_fs_async_work(fs_async_work& work) {
      switch (work.kind) {
      case fs_async_kind::read_file:
        work.bytes = read_all(fs::path(work.path), work.ec);
        break;
      case fs_async_kind::write_file:
        (void)write_all(fs::path(work.path), work.data, false, work.ec);
        break;
      case fs_async_kind::append_file:
        (void)write_all(fs::path(work.path), work.data, true, work.ec);
        break;
      case fs_async_kind::stat:
        (void)collect_stat_record(fs::path(work.path), work.stat, work.ec, true);
        break;
      case fs_async_kind::lstat:
        (void)collect_stat_record(fs::path(work.path), work.stat, work.ec, false);
        break;
      case fs_async_kind::readdir:
        (void)collect_readdir_records(fs::path(work.path), work.with_types, work.entries, work.ec);
        break;
      case fs_async_kind::mkdir:
        if (work.recursive)
          fs::create_directories(fs::path(work.path), work.ec);
        else
          fs::create_directory(fs::path(work.path), work.ec);
        break;
      case fs_async_kind::rm:
        if (work.recursive)
          fs::remove_all(fs::path(work.path), work.ec);
        else
          fs::remove(fs::path(work.path), work.ec);
        if (work.ec && work.force)
          work.ec.clear();
        break;
      case fs_async_kind::rename:
        fs::rename(fs::path(work.path), fs::path(work.path2), work.ec);
        break;
      case fs_async_kind::realpath: {
        auto canonical = fs::weakly_canonical(fs::path(work.path), work.ec);
        if (!work.ec)
          work.text = canonical.generic_string();
        break;
      }
      case fs_async_kind::exists:
        work.bool_result = fs::exists(fs::path(work.path), work.ec) && !work.ec;
        if (work.ec)
          work.ec.clear();
        break;
      case fs_async_kind::copy_file:
        fs::copy_file(fs::path(work.path), fs::path(work.path2),
                      fs::copy_options::overwrite_existing, work.ec);
        break;
      case fs_async_kind::cp:
        (void)copy_tree(fs::path(work.path), fs::path(work.path2), work.dereference, work.ec);
        break;
      case fs_async_kind::symlink:
        if (work.text == "dir" || work.text == "junction")
          fs::create_directory_symlink(fs::path(work.path), fs::path(work.path2), work.ec);
        else
          fs::create_symlink(fs::path(work.path), fs::path(work.path2), work.ec);
        break;
      case fs_async_kind::readlink: {
        auto target = fs::read_symlink(fs::path(work.path), work.ec);
        if (!work.ec)
          work.text = target.generic_string();
        break;
      }
      case fs_async_kind::link:
        fs::create_hard_link(fs::path(work.path), fs::path(work.path2), work.ec);
        break;
      case fs_async_kind::access:
        (void)access_path(fs::path(work.path), work.mode, work.ec);
        break;
      case fs_async_kind::chmod:
        (void)chmod_path(fs::path(work.path), work.mode, false, work.ec);
        break;
      case fs_async_kind::lchmod:
        (void)chmod_path(fs::path(work.path), work.mode, true, work.ec);
        break;
      case fs_async_kind::chown:
        (void)chown_path(fs::path(work.path), work.uid, work.gid, work.ec);
        break;
      case fs_async_kind::utimes:
        (void)set_file_times(fs::path(work.path), work.atime_seconds, work.mtime_seconds, false,
                             work.ec);
        break;
      case fs_async_kind::lutimes:
        (void)set_file_times(fs::path(work.path), work.atime_seconds, work.mtime_seconds, true,
                             work.ec);
        break;
      case fs_async_kind::write_file_atomic: {
        auto tmp = random_temp_path(fs::path(work.path));
        if (!write_all(tmp, work.data, false, work.ec)) {
          std::error_code cleanup_ec;
          fs::remove(fs::path(tmp), cleanup_ec);
          break;
        }
        fs::rename(fs::path(tmp), fs::path(work.path), work.ec);
        if (work.ec) {
          std::error_code cleanup_ec;
          fs::remove(fs::path(tmp), cleanup_ec);
        }
        break;
      }
      case fs_async_kind::lock:
#if defined(_WIN32)
        work.ec = std::make_error_code(std::errc::operation_not_supported);
#else
      {
        int op = work.exclusive ? LOCK_EX : LOCK_SH;
        if (work.non_blocking)
          op |= LOCK_NB;
        if (::flock(work.fd, op) != 0)
          work.ec = errno_error();
      }
#endif
        break;
      case fs_async_kind::unlock:
#if defined(_WIN32)
        work.ec = std::make_error_code(std::errc::operation_not_supported);
#else
        if (::flock(work.fd, LOCK_UN) != 0)
          work.ec = errno_error();
#endif
        break;
      }
    }

#if FXE_HAS_LIBUV
    void fs_async_work_cb(uv_work_t* req) {
      auto* work = static_cast<fs_async_work*>(req->data);
      if (!work)
        return;
      int expected = 0;
      if (!work->cancel->phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
        wake_event_loop();
        return;
      }
      perform_fs_async_work(*work);
      wake_event_loop();
    }

    void fs_async_after_cb(uv_work_t* req, int status) {
      std::unique_ptr<fs_async_work> work(static_cast<fs_async_work*>(req->data));
      if (!work)
        return;
      auto* iso = work->iso;
      HandleScope hs(iso);
      auto ctx = work->context.Get(iso);
      Context::Scope scope(ctx);
      auto resolver = work->resolver.Get(iso);
      std::string abort_reason =
          work->signal.IsEmpty() ? std::string{} : signal_reason(iso, ctx, work->signal.Get(iso));
      remove_abort_listener(iso, ctx, *work);

      if (status == UV_ECANCELED || work->cancel->aborted.load(std::memory_order_acquire) ||
          work->cancel->phase.load(std::memory_order_acquire) == 2) {
        work->cancel->phase.store(3, std::memory_order_release);
        (void)resolver->Reject(ctx, make_abort_error(iso, abort_reason));
        return;
      }
      work->cancel->phase.store(3, std::memory_order_release);
      if (status < 0) {
        (void)resolver->Reject(ctx, make_uv_error(iso, status, work->path, work->syscall.c_str()));
        return;
      }
      if (work->ec) {
        (void)resolver->Reject(ctx,
                               make_async_error(iso, work->ec, work->path, work->syscall.c_str()));
        return;
      }

      Local<Value> value = Undefined(iso);
      switch (work->kind) {
      case fs_async_kind::read_file:
        if (work->encoding == read_encoding::utf8) {
          value =
              to_v8_string(iso, std::string_view(reinterpret_cast<const char*>(work->bytes.data()),
                                                 work->bytes.size()));
        } else {
          value = bytes_to_uint8(iso, work->bytes);
        }
        break;
      case fs_async_kind::stat:
      case fs_async_kind::lstat:
        value = stat_record_to_object(iso, work->stat);
        break;
      case fs_async_kind::readdir:
        value = readdir_records_to_value(iso, work->entries, work->with_types);
        break;
      case fs_async_kind::realpath:
      case fs_async_kind::readlink:
        value = to_v8_string(iso, work->text);
        break;
      case fs_async_kind::exists:
        value = Boolean::New(iso, work->bool_result);
        break;
      case fs_async_kind::write_file:
      case fs_async_kind::append_file:
      case fs_async_kind::mkdir:
      case fs_async_kind::rm:
      case fs_async_kind::rename:
      case fs_async_kind::copy_file:
      case fs_async_kind::cp:
      case fs_async_kind::symlink:
      case fs_async_kind::link:
      case fs_async_kind::access:
      case fs_async_kind::chmod:
      case fs_async_kind::lchmod:
      case fs_async_kind::chown:
      case fs_async_kind::utimes:
      case fs_async_kind::lutimes:
      case fs_async_kind::write_file_atomic:
      case fs_async_kind::lock:
      case fs_async_kind::unlock:
        value = Undefined(iso);
        break;
      }
      (void)resolver->Resolve(ctx, value);
    }
#endif

    void queue_fs_async(const FunctionCallbackInfo<Value>& info,
                        std::unique_ptr<fs_async_work> work, Local<Object> signal) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());

      if (!signal.IsEmpty() && signal_is_aborted(iso, ctx, signal)) {
        (void)resolver->Reject(ctx, make_abort_error(iso, signal_reason(iso, ctx, signal)));
        return;
      }

      work->iso = iso;
      work->context.Reset(iso, ctx);
      work->resolver.Reset(iso, resolver);
      install_abort_listener(iso, ctx, *work, signal);

#if FXE_HAS_LIBUV
      auto* loop = fxe::runtime::default_loop();
      work->req.data = work.get();
      if (!loop) {
        fs_async_work_cb(&work->req);
        fs_async_after_cb(&work->req, 0);
        (void)work.release();
        return;
      }
      int rc = uv_queue_work(loop, &work->req, fs_async_work_cb, fs_async_after_cb);
      if (rc != 0) {
        fs_async_work_cb(&work->req);
        fs_async_after_cb(&work->req, 0);
        (void)work.release();
        return;
      }
      (void)work.release();
#else
      work->cancel->phase.store(1, std::memory_order_release);
      perform_fs_async_work(*work);
      work->cancel->phase.store(3, std::memory_order_release);
      remove_abort_listener(iso, ctx, *work);
      if (work->cancel->aborted.load(std::memory_order_acquire)) {
        (void)resolver->Reject(ctx, make_abort_error(iso));
        return;
      }
      if (work->ec) {
        (void)resolver->Reject(ctx,
                               make_async_error(iso, work->ec, work->path, work->syscall.c_str()));
        return;
      }
      Local<Value> value = Undefined(iso);
      switch (work->kind) {
      case fs_async_kind::read_file:
        if (work->encoding == read_encoding::utf8) {
          value =
              to_v8_string(iso, std::string_view(reinterpret_cast<const char*>(work->bytes.data()),
                                                 work->bytes.size()));
        } else {
          value = bytes_to_uint8(iso, work->bytes);
        }
        break;
      case fs_async_kind::stat:
      case fs_async_kind::lstat:
        value = stat_record_to_object(iso, work->stat);
        break;
      case fs_async_kind::readdir:
        value = readdir_records_to_value(iso, work->entries, work->with_types);
        break;
      case fs_async_kind::realpath:
      case fs_async_kind::readlink:
        value = to_v8_string(iso, work->text);
        break;
      case fs_async_kind::exists:
        value = Boolean::New(iso, work->bool_result);
        break;
      default:
        value = Undefined(iso);
        break;
      }
      (void)resolver->Resolve(ctx, value);
#endif
    }

    std::unique_ptr<fs_async_work> make_work(fs_async_kind kind, std::string path,
                                             const char* syscall) {
      auto work = std::make_unique<fs_async_work>();
      work->kind = kind;
      work->path = std::move(path);
      work->syscall = syscall ? syscall : "fs";
      return work;
    }

    void fs_read_file_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("readFile(path, opts?)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      auto work = make_work(fs_async_kind::read_file, p, "open");
      work->encoding = info.Length() >= 2 ? read_encoding_from(iso, info[1]) : read_encoding::raw;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 1));
    }

    void fs_write_file_async_impl(const FunctionCallbackInfo<Value>& info, bool append) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("writeFile(path, data)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      auto work = make_work(append ? fs_async_kind::append_file : fs_async_kind::write_file, p,
                            append ? "appendFile" : "writeFile");
      if (!extract_data(iso, info[1], work->data)) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("data must be string or TypedArray"_v8(iso))));
        return;
      }
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }
    void fs_write_file_async(const FunctionCallbackInfo<Value>& info) {
      fs_write_file_async_impl(info, false);
    }
    void fs_append_file_async(const FunctionCallbackInfo<Value>& info) {
      fs_write_file_async_impl(info, true);
    }

    void fs_stat_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(rejected(iso, ctx, Exception::TypeError("stat(path)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      queue_fs_async(info, make_work(fs_async_kind::stat, p, "stat"),
                     signal_from_options(iso, ctx, info, 1));
    }

    void fs_readdir_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("readdir(path)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      auto work = make_work(fs_async_kind::readdir, p, "scandir");
      work->with_types =
          info.Length() >= 2 ? bool_field(iso, info[1], "withFileTypes", false) : false;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 1));
    }

    void fs_mkdir_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(rejected(iso, ctx, Exception::TypeError("mkdir(path)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      auto work = make_work(fs_async_kind::mkdir, p, "mkdir");
      work->recursive = info.Length() >= 2 ? bool_field(iso, info[1], "recursive", false) : false;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 1));
    }

    void fs_rm_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(rejected(iso, ctx, Exception::TypeError("rm(path)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      auto work = make_work(fs_async_kind::rm, p, "unlink");
      work->recursive = info.Length() >= 2 ? bool_field(iso, info[1], "recursive", false) : false;
      work->force = info.Length() >= 2 ? bool_field(iso, info[1], "force", false) : false;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 1));
    }

    void fs_rename_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("rename(from, to)"_v8(iso))));
        return;
      }
      auto from = to_std_string(iso, info[0]);
      auto to = to_std_string(iso, info[1]);
      if (!guard_fs_async(iso, ctx, info, from) || !guard_fs_async(iso, ctx, info, to))
        return;
      auto work = make_work(fs_async_kind::rename, from, "rename");
      work->path2 = to;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_realpath_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("realpath(path)"_v8(iso))));
        return;
      }
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      queue_fs_async(info, make_work(fs_async_kind::realpath, p, "realpath"),
                     signal_from_options(iso, ctx, info, 1));
    }

    void fs_exists_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto p = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, p))
        return;
      queue_fs_async(info, make_work(fs_async_kind::exists, p, "access"),
                     signal_from_options(iso, ctx, info, 1));
    }

    void fs_copy_file_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("copyFile(src, dest)"_v8(iso))));
        return;
      }
      auto src = to_std_string(iso, info[0]);
      auto dest = to_std_string(iso, info[1]);
      if (!guard_fs_async(iso, ctx, info, src) || !guard_fs_async(iso, ctx, info, dest))
        return;
      auto work = make_work(fs_async_kind::copy_file, src, "copyfile");
      work->path2 = dest;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_cp_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("cp(src, dest, opts?)"_v8(iso))));
        return;
      }
      auto src = to_std_string(iso, info[0]);
      auto dest = to_std_string(iso, info[1]);
      if (!guard_fs_async(iso, ctx, info, src) || !guard_fs_async(iso, ctx, info, dest))
        return;
      auto work = make_work(fs_async_kind::cp, src, "cp");
      work->path2 = dest;
      work->dereference =
          info.Length() >= 3 ? bool_field(iso, info[2], "dereference", false) : false;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_symlink_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("symlink(target, path, type?)"_v8(iso))));
        return;
      }
      auto target = to_std_string(iso, info[0]);
      auto link_path = to_std_string(iso, info[1]);
      auto guarded_target = guarded_symlink_target_path(target, link_path).generic_string();
      if (!guard_fs_async(iso, ctx, info, guarded_target) ||
          !guard_fs_async(iso, ctx, info, link_path))
        return;
      auto work = make_work(fs_async_kind::symlink, target, "symlink");
      work->path2 = link_path;
      work->text = info.Length() >= 3 && info[2]->IsString() ? to_std_string(iso, info[2]) : "";
      auto signal = signal_from_options(iso, ctx, info, 2);
      if (signal.IsEmpty())
        signal = signal_from_options(iso, ctx, info, 3);
      queue_fs_async(info, std::move(work), signal);
    }

    void fs_link_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("link(existing, path)"_v8(iso))));
        return;
      }
      auto existing = to_std_string(iso, info[0]);
      auto new_path = to_std_string(iso, info[1]);
      if (!guard_fs_async(iso, ctx, info, existing) || !guard_fs_async(iso, ctx, info, new_path))
        return;
      auto work = make_work(fs_async_kind::link, existing, "link");
      work->path2 = new_path;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_access_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("access(path, mode?)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      auto work = make_work(fs_async_kind::access, path, "access");
      work->mode =
          info.Length() >= 2 && !info[1]->IsObject() ? info[1]->Int32Value(ctx).FromMaybe(0) : 0;
      auto signal = signal_from_options(iso, ctx, info, 1);
      if (signal.IsEmpty())
        signal = signal_from_options(iso, ctx, info, 2);
      queue_fs_async(info, std::move(work), signal);
    }

    void fs_chmod_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("chmod(path, mode)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      auto work = make_work(fs_async_kind::chmod, path, "chmod");
      work->mode = info[1]->Int32Value(ctx).FromMaybe(0);
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_lchmod_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("lchmod(path, mode)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      auto work = make_work(fs_async_kind::lchmod, path, "lchmod");
      work->mode = info[1]->Int32Value(ctx).FromMaybe(0);
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_chown_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("chown(path, uid, gid)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      auto work = make_work(fs_async_kind::chown, path, "chown");
      work->uid = info[1]->Int32Value(ctx).FromMaybe(-1);
      work->gid = info[2]->Int32Value(ctx).FromMaybe(-1);
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 3));
    }

    void fs_utimes_async_impl(const FunctionCallbackInfo<Value>& info, bool nofollow) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsString()) {
        info.GetReturnValue().Set(rejected(
            iso, ctx,
            Exception::TypeError(to_v8_string(iso, nofollow ? "lutimes(path, atime, mtime)"
                                                            : "utimes(path, atime, mtime)"))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      auto work = make_work(nofollow ? fs_async_kind::lutimes : fs_async_kind::utimes, path,
                            nofollow ? "lutimes" : "utimes");
      work->atime_seconds = time_arg_seconds(iso, ctx, info[1]);
      work->mtime_seconds = time_arg_seconds(iso, ctx, info[2]);
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 3));
    }

    void fs_utimes_async(const FunctionCallbackInfo<Value>& info) {
      fs_utimes_async_impl(info, false);
    }

    void fs_lutimes_async(const FunctionCallbackInfo<Value>& info) {
      fs_utimes_async_impl(info, true);
    }

    void fs_write_file_atomic_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("writeFileAtomic(path, data)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      auto work = make_work(fs_async_kind::write_file_atomic, path, "writeFileAtomic");
      if (!extract_data(iso, info[1], work->data)) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("data must be string or TypedArray"_v8(iso))));
        return;
      }
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 2));
    }

    void fs_lock_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsInt32()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("lock(fd, opts?)"_v8(iso))));
        return;
      }
      auto work = make_work(fs_async_kind::lock, "", "flock");
      work->fd = info[0]->Int32Value(ctx).FromMaybe(-1);
      work->exclusive = info.Length() >= 2 ? bool_field(iso, info[1], "exclusive", true) : true;
      work->non_blocking =
          info.Length() >= 2 ? bool_field(iso, info[1], "nonBlocking", false) : false;
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 1));
    }

    void fs_unlock_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsInt32()) {
        info.GetReturnValue().Set(rejected(iso, ctx, Exception::TypeError("unlock(fd)"_v8(iso))));
        return;
      }
      auto work = make_work(fs_async_kind::unlock, "", "flock");
      work->fd = info[0]->Int32Value(ctx).FromMaybe(-1);
      queue_fs_async(info, std::move(work), signal_from_options(iso, ctx, info, 1));
    }

    void fs_readlink_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(
            rejected(iso, ctx, Exception::TypeError("readlink(path)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      queue_fs_async(info, make_work(fs_async_kind::readlink, path, "readlink"),
                     signal_from_options(iso, ctx, info, 1));
    }

    void fs_lstat_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(rejected(iso, ctx, Exception::TypeError("lstat(path)"_v8(iso))));
        return;
      }
      auto path = to_std_string(iso, info[0]);
      if (!guard_fs_async(iso, ctx, info, path))
        return;
      queue_fs_async(info, make_work(fs_async_kind::lstat, path, "lstat"),
                     signal_from_options(iso, ctx, info, 1));
    }

    struct fs_watch_state {
      Isolate* iso = nullptr;
      Global<Context> context;
      Global<Function> callback;
      std::unique_ptr<fxe::runtime::fs_watcher> watcher;
      std::atomic<bool> closed{false};
      Global<Object>* persistent = nullptr;
    };

    std::string watch_basename(const std::string& path) {
      auto name = fs::path(path).filename().generic_string();
      return name.empty() ? path : name;
    }

    const char* watch_event_name(fxe::runtime::fs_watch_event::kind kind) {
      switch (kind) {
      case fxe::runtime::fs_watch_event::kind::renamed:
      case fxe::runtime::fs_watch_event::kind::created:
      case fxe::runtime::fs_watch_event::kind::deleted:
        return "rename";
      case fxe::runtime::fs_watch_event::kind::changed:
      case fxe::runtime::fs_watch_event::kind::overflow:
      default:
        return "change";
      }
    }

    fs_watch_state* fs_watch_state_from(Local<Object> self) {
      if (self->InternalFieldCount() < 1)
        return nullptr;
      return external_ptr<fs_watch_state>(self->GetInternalField(0));
    }

    void fs_watch_close(const FunctionCallbackInfo<Value>& info) {
      auto* state = fs_watch_state_from(info.This());
      if (!state)
        return;
      bool expected = false;
      if (state->closed.compare_exchange_strong(expected, true) && state->watcher)
        state->watcher->close();
    }

    void fs_watch_finalizer(const WeakCallbackInfo<fs_watch_state>& info) {
      auto* state = info.GetParameter();
      if (!state)
        return;
      state->closed.store(true);
      if (state->watcher)
        state->watcher->close();
      if (state->persistent) {
        state->persistent->Reset();
        delete state->persistent;
        state->persistent = nullptr;
      }
      state->callback.Reset();
      state->context.Reset();
      // The platform watcher may already have queued main-thread callbacks that
      // capture this state. Keep the small state allocation alive for process
      // lifetime rather than risking a use-after-free from a late dispatch.
    }
    void fs_watch(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "watch(path[, options], listener)");
        return;
      }

      Local<Value> options = Undefined(iso);
      Local<Value> callback;
      if (info.Length() >= 2 && info[1]->IsFunction()) {
        callback = info[1];
      } else {
        if (info.Length() >= 2)
          options = info[1];
        if (info.Length() >= 3)
          callback = info[2];
      }
      if (callback.IsEmpty() || !callback->IsFunction()) {
        (void)throw_type_error(iso, "watch listener must be a function");
        return;
      }

      const std::string path = to_std_string(iso, info[0]);
      if (!guard_fs(iso, path))
        return;
      bool recursive = false;
      if (!options->IsUndefined() && options->IsObject()) {
        if (auto recursive_value = get_prop<Local<Value>>(ctx, options.As<Object>(), "recursive"))
          recursive = (*recursive_value)->BooleanValue(iso);
      }

      auto* state = new fs_watch_state();
      state->iso = iso;
      state->context.Reset(iso, ctx);
      state->callback.Reset(iso, callback.As<Function>());
      fxe::runtime::fs_watch_error watch_error;
      state->watcher = fxe::runtime::fs_watcher::create(
          path, recursive,
          [state](const fxe::runtime::fs_watch_event& event) {
            std::string event_path = event.path;
            auto event_kind = event.k;
            fxe::os::post_main_thread_dispatch(
                [state, event_path = std::move(event_path), event_kind] {
                  if (!state || state->closed.load())
                    return;
                  auto* iso = state->iso;
                  HandleScope hs(iso);
                  auto ctx = state->context.Get(iso);
                  Context::Scope scope(ctx);
                  auto cb = state->callback.Get(iso);
                  Local<Value> argv[] = {to_v8_string(iso, watch_event_name(event_kind)),
                                         to_v8_string(iso, watch_basename(event_path))};
                  Local<Value> ignored;
                  (void)cb->Call(ctx, Undefined(iso), 2, argv).ToLocal(&ignored);
                });
          },
          &watch_error);
      if (!state->watcher) {
        state->callback.Reset();
        state->context.Reset();
        delete state;
        iso->ThrowException(make_watch_error(iso, watch_error, path));
        return;
      }
      auto tpl = ObjectTemplate::New(iso);
      tpl->SetInternalFieldCount(1);
      tpl->Set(iso, "close", FunctionTemplate::New(iso, fs_watch_close));
      auto watcher = tpl->NewInstance(ctx).ToLocalChecked();
      watcher->SetInternalField(0, make_external(iso, state));
      auto* persistent = new Global<Object>(iso, watcher);
      state->persistent = persistent;
      persistent->SetWeak(state, fs_watch_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(watcher);
    }
  } // namespace

  void install_fs_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto t = ObjectTemplate::New(iso);
    // sync
    t->Set(iso, "readFileSync", FunctionTemplate::New(iso, fs_read_file_sync));
    t->Set(iso, "writeFileSync", FunctionTemplate::New(iso, fs_write_file_sync));
    t->Set(iso, "appendFileSync", FunctionTemplate::New(iso, fs_append_file_sync));
    t->Set(iso, "existsSync", FunctionTemplate::New(iso, fs_exists_sync));
    t->Set(iso, "statSync", FunctionTemplate::New(iso, fs_stat_sync));
    t->Set(iso, "readdirSync", FunctionTemplate::New(iso, fs_readdir_sync));
    t->Set(iso, "mkdirSync", FunctionTemplate::New(iso, fs_mkdir_sync));
    t->Set(iso, "rmSync", FunctionTemplate::New(iso, fs_rm_sync));
    t->Set(iso, "renameSync", FunctionTemplate::New(iso, fs_rename_sync));
    t->Set(iso, "realpathSync", FunctionTemplate::New(iso, fs_realpath_sync));
    t->Set(iso, "copyFileSync", FunctionTemplate::New(iso, fs_copy_file_sync));
    t->Set(iso, "cpSync", FunctionTemplate::New(iso, fs_cp_sync));
    t->Set(iso, "symlinkSync", FunctionTemplate::New(iso, fs_symlink_sync));
    t->Set(iso, "readlinkSync", FunctionTemplate::New(iso, fs_readlink_sync));
    t->Set(iso, "linkSync", FunctionTemplate::New(iso, fs_link_sync));
    t->Set(iso, "lstatSync", FunctionTemplate::New(iso, fs_lstat_sync));
    t->Set(iso, "accessSync", FunctionTemplate::New(iso, fs_access_sync));
    t->Set(iso, "chmodSync", FunctionTemplate::New(iso, fs_chmod_sync));
    t->Set(iso, "chownSync", FunctionTemplate::New(iso, fs_chown_sync));
    t->Set(iso, "lchmodSync", FunctionTemplate::New(iso, fs_lchmod_sync));
    t->Set(iso, "utimesSync", FunctionTemplate::New(iso, fs_utimes_sync));
    t->Set(iso, "lutimesSync", FunctionTemplate::New(iso, fs_lutimes_sync));
    t->Set(iso, "globSync", FunctionTemplate::New(iso, fs_glob_sync));
    t->Set(iso, "writeFileAtomicSync", FunctionTemplate::New(iso, fs_write_file_atomic_sync));
    t->Set(iso, "lockSync", FunctionTemplate::New(iso, fs_lock_sync));
    t->Set(iso, "unlockSync", FunctionTemplate::New(iso, fs_unlock_sync));
    // async
    t->Set(iso, "readFile", FunctionTemplate::New(iso, fs_read_file_async));
    t->Set(iso, "writeFile", FunctionTemplate::New(iso, fs_write_file_async));
    t->Set(iso, "appendFile", FunctionTemplate::New(iso, fs_append_file_async));
    t->Set(iso, "stat", FunctionTemplate::New(iso, fs_stat_async));
    t->Set(iso, "readdir", FunctionTemplate::New(iso, fs_readdir_async));
    t->Set(iso, "mkdir", FunctionTemplate::New(iso, fs_mkdir_async));
    t->Set(iso, "rm", FunctionTemplate::New(iso, fs_rm_async));
    t->Set(iso, "rename", FunctionTemplate::New(iso, fs_rename_async));
    t->Set(iso, "realpath", FunctionTemplate::New(iso, fs_realpath_async));
    t->Set(iso, "exists", FunctionTemplate::New(iso, fs_exists_async));
    t->Set(iso, "copyFile", FunctionTemplate::New(iso, fs_copy_file_async));
    t->Set(iso, "cp", FunctionTemplate::New(iso, fs_cp_async));
    t->Set(iso, "symlink", FunctionTemplate::New(iso, fs_symlink_async));
    t->Set(iso, "readlink", FunctionTemplate::New(iso, fs_readlink_async));
    t->Set(iso, "link", FunctionTemplate::New(iso, fs_link_async));
    t->Set(iso, "lstat", FunctionTemplate::New(iso, fs_lstat_async));
    t->Set(iso, "access", FunctionTemplate::New(iso, fs_access_async));
    t->Set(iso, "chmod", FunctionTemplate::New(iso, fs_chmod_async));
    t->Set(iso, "chown", FunctionTemplate::New(iso, fs_chown_async));
    t->Set(iso, "lchmod", FunctionTemplate::New(iso, fs_lchmod_async));
    t->Set(iso, "utimes", FunctionTemplate::New(iso, fs_utimes_async));
    t->Set(iso, "lutimes", FunctionTemplate::New(iso, fs_lutimes_async));
    t->Set(iso, "glob", FunctionTemplate::New(iso, fs_glob));
    t->Set(iso, "writeFileAtomic", FunctionTemplate::New(iso, fs_write_file_atomic_async));
    t->Set(iso, "lock", FunctionTemplate::New(iso, fs_lock_async));
    t->Set(iso, "unlock", FunctionTemplate::New(iso, fs_unlock_async));
    t->Set(iso, "watch", FunctionTemplate::New(iso, fs_watch));
    global->Set(iso, "fs", t);
  }
} // namespace fxe::js
