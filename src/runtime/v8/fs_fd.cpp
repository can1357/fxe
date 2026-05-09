#include "runtime/v8/fs_fd.hpp"
#include "runtime/capabilities.hpp"
#include "runtime/uv_loop.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fxe/types.hpp>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <fxe/v8_literals.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fxe::runtime {
  namespace {
    using namespace v8;

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    void set(Local<Context> ctx, Local<Object> obj, const char* key, Local<Value> value) {
      (void)obj->Set(ctx, str(Isolate::GetCurrent(), key), value);
    }

    void set_string(Local<Context> ctx, Local<Object> obj, const char* key,
                    std::string_view value) {
      set(ctx, obj, key, str(Isolate::GetCurrent(), value));
    }

    void set_number(Local<Context> ctx, Local<Object> obj, const char* key, double value) {
      set(ctx, obj, key, Number::New(Isolate::GetCurrent(), value));
    }

    void set_bool(Local<Context> ctx, Local<Object> obj, const char* key, bool value) {
      set(ctx, obj, key, Boolean::New(Isolate::GetCurrent(), value));
    }

    std::string string_arg(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      return std::string(*utf8 ? *utf8 : "");
    }

#if defined(_WIN32)
    std::wstring widen_utf8(const std::string& value) {
      if (value.empty())
        return {};
      const int needed =
          MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (needed <= 0)
        return {};
      std::wstring out(static_cast<usize>(needed), L'\0');
      (void)MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                out.data(), needed);
      return out;
    }
#endif

    const char* errno_code(int err) {
      switch (err) {
      case EACCES:
        return "EACCES";
      case EBADF:
        return "EBADF";
      case EEXIST:
        return "EEXIST";
      case EINVAL:
        return "EINVAL";
      case EISDIR:
        return "EISDIR";
      case ENOENT:
        return "ENOENT";
#if defined(ENOTDIR)
      case ENOTDIR:
        return "ENOTDIR";
#endif
      case ENOSPC:
        return "ENOSPC";
      case EPERM:
        return "EPERM";
      default:
        return "EIO";
      }
    }

    Local<Value> make_permission_denied(Isolate* iso, Local<Context> ctx, std::string_view path) {
      std::string msg = "Permission denied: fs access denied for '";
      msg.append(path);
      msg.push_back('\'');
      auto err = Exception::Error(str(iso, msg)).As<Object>();
      (void)err->Set(ctx, "name"_v8(iso), "PermissionDenied"_v8(iso));
      (void)err->Set(ctx, "code"_v8(iso), "EACCES"_v8(iso));
      return err;
    }

    Local<Value> make_errno_error(Isolate* iso, Local<Context> ctx, int err, const char* syscall,
                                  std::string_view path = {}) {
      std::string code = errno_code(err);
      std::string detail = std::strerror(err);
#if FXE_HAS_LIBUV
      const int uv_status = uv_translate_sys_error(err);
      code = uv_err_name(uv_status);
      detail = uv_strerror(uv_status);
#endif
      std::string message = syscall;
      message += ": ";
      message += code;
      message += ": ";
      message += detail;
      if (!path.empty()) {
        message += " '";
        message.append(path);
        message.push_back('\'');
      }
      auto out = Exception::Error(str(iso, message)).As<Object>();
      set_number(ctx, out, "errno", err);
      set_string(ctx, out, "code", code);
      set_string(ctx, out, "syscall", syscall);
      if (!path.empty())
        set_string(ctx, out, "path", path);
      return out;
    }

#if FXE_HAS_LIBUV
    Local<Value> make_uv_error(Isolate* iso, Local<Context> ctx, int status, const char* syscall,
                               std::string_view path = {}) {
      std::string message = syscall;
      message += ": ";
      message += uv_err_name(status);
      message += ": ";
      message += uv_strerror(status);
      if (!path.empty()) {
        message += " '";
        message.append(path);
        message.push_back('\'');
      }
      auto out = Exception::Error(str(iso, message)).As<Object>();
      set_number(ctx, out, "errno", status);
      set_string(ctx, out, "code", uv_err_name(status));
      set_string(ctx, out, "syscall", syscall);
      if (!path.empty())
        set_string(ctx, out, "path", path);
      return out;
    }
#endif

    void throw_errno_error(Isolate* iso, Local<Context> ctx, int err, const char* syscall,
                           std::string_view path = {}) {
      iso->ThrowException(make_errno_error(iso, ctx, err, syscall, path));
    }

    Local<Promise> rejected([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> error) {
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      (void)resolver->Reject(ctx, error);
      return resolver->GetPromise();
    }

    int open_flag_value(std::string_view flags) {
#if defined(_WIN32)
      constexpr int rdonly = _O_RDONLY;
      constexpr int wronly = _O_WRONLY;
      constexpr int rdwr = _O_RDWR;
      constexpr int creat = _O_CREAT;
      constexpr int trunc = _O_TRUNC;
      constexpr int append = _O_APPEND;
#else
      constexpr int rdonly = O_RDONLY;
      constexpr int wronly = O_WRONLY;
      constexpr int rdwr = O_RDWR;
      constexpr int creat = O_CREAT;
      constexpr int trunc = O_TRUNC;
      constexpr int append = O_APPEND;
#endif
      if (flags == "r")
        return rdonly;
      if (flags == "r+")
        return rdwr;
      if (flags == "w")
        return wronly | creat | trunc;
      if (flags == "w+")
        return rdwr | creat | trunc;
      if (flags == "a")
        return wronly | creat | append;
      if (flags == "a+")
        return rdwr | creat | append;
      return -1;
    }

    bool parse_open_flags(Isolate* iso, Local<Context> ctx, Local<Value> value, int& out) {
      if (value.IsEmpty() || value->IsUndefined() || value->IsNull()) {
        out = open_flag_value("r");
        return true;
      }
      if (value->IsNumber()) {
        out = value->Int32Value(ctx).FromMaybe(0);
#if defined(_WIN32)
        out |= _O_BINARY;
#endif
        return true;
      }
      if (!value->IsString()) {
        iso->ThrowException(
            Exception::TypeError("fs open flags must be a string or number"_v8(iso)));
        return false;
      }
      out = open_flag_value(string_arg(iso, value));
      if (out < 0) {
        iso->ThrowException(Exception::TypeError("unsupported fs open flags"_v8(iso)));
        return false;
      }
#if defined(_WIN32)
      out |= _O_BINARY;
#endif
      return true;
    }

    int open_file(const std::string& path, int flags, int mode, int& err) {
#if defined(_WIN32)
      const auto wide = widen_utf8(path);
      const int fd = _wopen(wide.c_str(), flags, mode);
#else
      const int fd = ::open(path.c_str(), flags, static_cast<mode_t>(mode));
#endif
      if (fd < 0) {
        err = errno;
        return -1;
      }
      err = 0;
      return fd;
    }

    int close_file(int fd) {
#if defined(_WIN32)
      return _close(fd);
#else
      return ::close(fd);
#endif
    }

    i64 read_fd(int fd, u8* data, usize length, i64 position, int& err) {
      if (length == 0) {
        err = 0;
        return 0;
      }
#if defined(_WIN32)
      if (length > static_cast<usize>(std::numeric_limits<unsigned int>::max())) {
        err = EINVAL;
        return -1;
      }
      if (position >= 0 && _lseeki64(fd, position, SEEK_SET) < 0) {
        err = errno;
        return -1;
      }
      const int n = _read(fd, data, static_cast<unsigned int>(length));
#else
      const ssize_t n = position >= 0 ? ::pread(fd, data, length, static_cast<off_t>(position))
                                      : ::read(fd, data, length);
#endif
      if (n < 0) {
        err = errno;
        return -1;
      }
      err = 0;
      return static_cast<i64>(n);
    }

    i64 write_fd(int fd, const u8* data, usize length, i64 position, int& err) {
      if (length == 0) {
        err = 0;
        return 0;
      }
#if defined(_WIN32)
      if (length > static_cast<usize>(std::numeric_limits<unsigned int>::max())) {
        err = EINVAL;
        return -1;
      }
      if (position >= 0 && _lseeki64(fd, position, SEEK_SET) < 0) {
        err = errno;
        return -1;
      }
      const int n = _write(fd, data, static_cast<unsigned int>(length));
#else
      const ssize_t n = position >= 0 ? ::pwrite(fd, data, length, static_cast<off_t>(position))
                                      : ::write(fd, data, length);
#endif
      if (n < 0) {
        err = errno;
        return -1;
      }
      err = 0;
      return static_cast<i64>(n);
    }

    bool truncate_fd(int fd, i64 len, int& err) {
      if (len < 0) {
        err = EINVAL;
        return false;
      }
#if defined(_WIN32)
      if (_chsize_s(fd, static_cast<__int64>(len)) != 0) {
        err = errno;
        return false;
      }
#else
      if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
        err = errno;
        return false;
      }
#endif
      err = 0;
      return true;
    }

    bool datasync_fd(int fd, int& err) {
#if defined(_WIN32)
      if (_commit(fd) != 0) {
        err = errno;
        return false;
      }
#elif defined(__APPLE__)
      if (::fcntl(fd, F_FULLFSYNC) != 0 && ::fsync(fd) != 0) {
        err = errno;
        return false;
      }
#else
      if (::fdatasync(fd) != 0) {
        err = errno;
        return false;
      }
#endif
      err = 0;
      return true;
    }

    struct stat_result {
      u64 size = 0;
      bool is_file = false;
      bool is_directory = false;
      double atime_ms = 0;
      double mtime_ms = 0;
      double ctime_ms = 0;
    };

    bool stat_fd(int fd, stat_result& out, int& err) {
#if defined(_WIN32)
      struct _stat64 st{};
      if (_fstat64(fd, &st) != 0) {
        err = errno;
        return false;
      }
      out.is_file = (st.st_mode & _S_IFMT) == _S_IFREG;
      out.is_directory = (st.st_mode & _S_IFMT) == _S_IFDIR;
#else
      struct stat st{};
      if (::fstat(fd, &st) != 0) {
        err = errno;
        return false;
      }
      out.is_file = S_ISREG(st.st_mode);
      out.is_directory = S_ISDIR(st.st_mode);
#endif
      out.size = static_cast<u64>(st.st_size);
      out.atime_ms = static_cast<double>(st.st_atime) * 1000.0;
      out.mtime_ms = static_cast<double>(st.st_mtime) * 1000.0;
      out.ctime_ms = static_cast<double>(st.st_ctime) * 1000.0;
      err = 0;
      return true;
    }

    Local<Object> fd_object(Isolate* iso, Local<Context> ctx, int fd) {
      auto out = Object::New(iso);
      set_number(ctx, out, "fd", fd);
      return out;
    }

    Local<Object> bytes_read_object(Isolate* iso, Local<Context> ctx, i64 n) {
      auto out = Object::New(iso);
      set_number(ctx, out, "bytesRead", static_cast<double>(n));
      return out;
    }

    Local<Object> bytes_written_object(Isolate* iso, Local<Context> ctx, i64 n) {
      auto out = Object::New(iso);
      set_number(ctx, out, "bytesWritten", static_cast<double>(n));
      return out;
    }

    Local<Object> stat_object(Isolate* iso, Local<Context> ctx, const stat_result& st) {
      auto out = Object::New(iso);
      set_number(ctx, out, "size", static_cast<double>(st.size));
      set_bool(ctx, out, "isFile", st.is_file);
      set_bool(ctx, out, "isDirectory", st.is_directory);
      set_number(ctx, out, "mtimeMs", st.mtime_ms);
      set_number(ctx, out, "atimeMs", st.atime_ms);
      set_number(ctx, out, "ctimeMs", st.ctime_ms);
      return out;
    }

    bool parse_buffer_args(Isolate* iso, Local<Context> ctx,
                           const FunctionCallbackInfo<Value>& info, bool write, u8*& data,
                           usize& length, i64& position, usize* view_offset = nullptr) {
      if (info.Length() < 2 || !info[0]->IsNumber() || !info[1]->IsArrayBufferView()) {
        iso->ThrowException(Exception::TypeError(
            str(iso, write ? "fs_fd.write(fd, buffer, offset, length, position)"
                           : "fs_fd.read(fd, buffer, offset, length, position)")));
        return false;
      }
      auto view = info[1].As<ArrayBufferView>();
      const auto view_length = static_cast<i64>(view->ByteLength());
      const auto offset_value = info.Length() > 2 && !info[2]->IsNullOrUndefined()
                                    ? info[2]->IntegerValue(ctx).FromMaybe(-1)
                                    : 0;
      const auto length_value = info.Length() > 3 && !info[3]->IsNullOrUndefined()
                                    ? info[3]->IntegerValue(ctx).FromMaybe(-1)
                                    : view_length - offset_value;
      if (offset_value < 0 || length_value < 0 || offset_value > view_length ||
          length_value > view_length - offset_value) {
        iso->ThrowException(Exception::RangeError("buffer offset/length out of range"_v8(iso)));
        return false;
      }
      const usize offset = static_cast<usize>(offset_value);
      length = static_cast<usize>(length_value);
      position = -1;
      if (info.Length() > 4 && !info[4]->IsNullOrUndefined())
        position = info[4]->IntegerValue(ctx).FromMaybe(-1);
      auto backing = view->Buffer()->GetBackingStore();
      const usize absolute_offset = view->ByteOffset() + offset;
      data = static_cast<u8*>(backing->Data()) + absolute_offset;
      if (view_offset)
        *view_offset = absolute_offset;
      return true;
    }

    void open_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        iso->ThrowException(
            Exception::TypeError("__fxe_native.fs_fd.openSync(path, flags, mode)"_v8(iso)));
        return;
      }
      const std::string path = string_arg(iso, info[0]);
      if (!fs_path_allowed(path)) {
        iso->ThrowException(make_permission_denied(iso, ctx, path));
        return;
      }
      int flags = 0;
      if (!parse_open_flags(iso, ctx, info.Length() > 1 ? info[1] : Undefined(iso), flags))
        return;
      const int mode = info.Length() > 2 && info[2]->IsNumber()
                           ? info[2]->Int32Value(ctx).FromMaybe(0666)
                           : 0666;
      int err = 0;
      const int fd = open_file(path, flags, mode, err);
      if (fd < 0) {
        throw_errno_error(iso, ctx, err, "open", path);
        return;
      }
      info.GetReturnValue().Set(fd_object(iso, ctx, fd));
    }

    void read_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      u8* data = nullptr;
      usize length = 0;
      i64 position = -1;
      if (!parse_buffer_args(iso, ctx, info, false, data, length, position))
        return;
      int err = 0;
      const auto n = read_fd(info[0]->Int32Value(ctx).FromMaybe(-1), data, length, position, err);
      if (n < 0) {
        throw_errno_error(iso, ctx, err, "read");
        return;
      }
      info.GetReturnValue().Set(bytes_read_object(iso, ctx, n));
    }

    void write_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      u8* data = nullptr;
      usize length = 0;
      i64 position = -1;
      if (!parse_buffer_args(iso, ctx, info, true, data, length, position))
        return;
      int err = 0;
      const auto n = write_fd(info[0]->Int32Value(ctx).FromMaybe(-1), data, length, position, err);
      if (n < 0) {
        throw_errno_error(iso, ctx, err, "write");
        return;
      }
      info.GetReturnValue().Set(bytes_written_object(iso, ctx, n));
    }

    void close_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      if (close_file(fd) != 0) {
        throw_errno_error(iso, ctx, errno, "close");
        return;
      }
      info.GetReturnValue().Set(Undefined(iso));
    }

    void fstat_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      stat_result st;
      int err = 0;
      if (!stat_fd(info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1, st, err)) {
        throw_errno_error(iso, ctx, err, "fstat");
        return;
      }
      info.GetReturnValue().Set(stat_object(iso, ctx, st));
    }

    void ftruncate_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      const i64 len = info.Length() > 1 && !info[1]->IsNullOrUndefined()
                          ? info[1]->IntegerValue(ctx).FromMaybe(0)
                          : 0;
      int err = 0;
      if (!truncate_fd(fd, len, err)) {
        throw_errno_error(iso, ctx, err, "ftruncate");
        return;
      }
      info.GetReturnValue().Set(Undefined(iso));
    }

    void fdatasync_sync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const int fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      int err = 0;
      if (!datasync_fd(fd, err)) {
        throw_errno_error(iso, ctx, err, "fdatasync");
        return;
      }
      info.GetReturnValue().Set(Undefined(iso));
    }

    enum class async_kind { open, read, write, close, fstat, ftruncate, fdatasync };

    struct async_job {
#if FXE_HAS_LIBUV
      uv_work_t req{};
#endif
      Isolate* iso = nullptr;
      Global<Context> context;
      Global<Promise::Resolver> resolver;
      async_kind kind = async_kind::open;
      std::string path;
      int flags = 0;
      int mode = 0666;
      int fd = -1;
      std::vector<u8> bytes;
      std::vector<u8> read_bytes;
      Global<ArrayBuffer> buffer;
      usize buffer_offset = 0;
      usize length = 0;
      i64 position = -1;
      i64 count = 0;
      stat_result stat;
      int err = 0;
      int uv_status = 0;
    };

    void run_job(async_job* job) {
      switch (job->kind) {
      case async_kind::open:
        job->fd = open_file(job->path, job->flags, job->mode, job->err);
        break;
      case async_kind::read:
        job->read_bytes.resize(job->length);
        job->count = read_fd(job->fd, job->read_bytes.data(), job->length, job->position, job->err);
        if (job->count >= 0)
          job->read_bytes.resize(static_cast<usize>(job->count));
        break;
      case async_kind::write:
        job->count =
            write_fd(job->fd, job->bytes.data(), job->bytes.size(), job->position, job->err);
        break;
      case async_kind::close:
        if (close_file(job->fd) != 0)
          job->err = errno;
        break;
      case async_kind::fstat:
        (void)stat_fd(job->fd, job->stat, job->err);
        break;
      case async_kind::ftruncate:
        (void)truncate_fd(job->fd, job->count, job->err);
        break;
      case async_kind::fdatasync:
        (void)datasync_fd(job->fd, job->err);
        break;
      }
    }

    const char* syscall_for(async_kind kind) {
      switch (kind) {
      case async_kind::open:
        return "open";
      case async_kind::read:
        return "read";
      case async_kind::write:
        return "write";
      case async_kind::close:
        return "close";
      case async_kind::fstat:
        return "fstat";
      case async_kind::ftruncate:
        return "ftruncate";
      case async_kind::fdatasync:
        return "fdatasync";
      }
      return "fs";
    }

    void finish_job(std::unique_ptr<async_job> job) {
      auto* iso = job->iso;
      HandleScope hs(iso);
      auto ctx = job->context.Get(iso);
      Context::Scope scope(ctx);
      auto resolver = job->resolver.Get(iso);
      if (job->err != 0) {
        (void)resolver->Reject(
            ctx, make_errno_error(iso, ctx, job->err, syscall_for(job->kind), job->path));
        return;
      }
#if FXE_HAS_LIBUV
      if (job->uv_status < 0) {
        (void)resolver->Reject(
            ctx, make_uv_error(iso, ctx, job->uv_status, syscall_for(job->kind), job->path));
        return;
      }
#endif
      Local<Value> value = Undefined(iso);
      switch (job->kind) {
      case async_kind::open:
        value = fd_object(iso, ctx, job->fd);
        break;
      case async_kind::read: {
        auto buffer = job->buffer.Get(iso);
        auto backing = buffer->GetBackingStore();
        if (!job->read_bytes.empty()) {
          std::memcpy(static_cast<u8*>(backing->Data()) + job->buffer_offset,
                      job->read_bytes.data(), job->read_bytes.size());
        }
        value = bytes_read_object(iso, ctx, job->count);
        break;
      }
      case async_kind::write:
        value = bytes_written_object(iso, ctx, job->count);
        break;
      case async_kind::fstat:
        value = stat_object(iso, ctx, job->stat);
        break;
      case async_kind::close:
      case async_kind::ftruncate:
      case async_kind::fdatasync:
        value = Undefined(iso);
        break;
      }
      (void)resolver->Resolve(ctx, value);
    }

#if FXE_HAS_LIBUV
    void work_cb(uv_work_t* req) {
      run_job(static_cast<async_job*>(req->data));
    }

    void after_work_cb(uv_work_t* req, int status) {
      auto* job = static_cast<async_job*>(req->data);
      if (status < 0)
        job->uv_status = status;
      finish_job(std::unique_ptr<async_job>(job));
    }
#endif

    void queue_job(const FunctionCallbackInfo<Value>& info, std::unique_ptr<async_job> job) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      auto promise = resolver->GetPromise();
      job->iso = iso;
      job->context.Reset(iso, ctx);
      job->resolver.Reset(iso, resolver);
#if FXE_HAS_LIBUV
      if (auto* loop = fxe::runtime::default_loop()) {
        job->req.data = job.get();
        const int rc = uv_queue_work(loop, &job->req, work_cb, after_work_cb);
        if (rc == 0) {
          (void)job.release();
          info.GetReturnValue().Set(promise);
          return;
        }
        job->uv_status = rc;
      }
#endif
      run_job(job.get());
      finish_job(std::move(job));
      info.GetReturnValue().Set(promise);
    }

    bool prepare_buffer_job(Isolate* iso, Local<Context> ctx,
                            const FunctionCallbackInfo<Value>& info, bool write, async_job& job) {
      u8* data = nullptr;
      if (!parse_buffer_args(iso, ctx, info, write, data, job.length, job.position,
                             write ? nullptr : &job.buffer_offset))
        return false;
      job.fd = info[0]->Int32Value(ctx).FromMaybe(-1);
      if (write) {
        job.bytes.assign(data, data + job.length);
      } else {
        auto view = info[1].As<ArrayBufferView>();
        job.buffer.Reset(iso, view->Buffer());
      }
      return true;
    }

    void open_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        info.GetReturnValue().Set(rejected(
            iso, ctx, Exception::TypeError("__fxe_native.fs_fd.open(path, flags, mode)"_v8(iso))));
        return;
      }
      const std::string path = string_arg(iso, info[0]);
      if (!fs_path_allowed(path)) {
        info.GetReturnValue().Set(rejected(iso, ctx, make_permission_denied(iso, ctx, path)));
        return;
      }
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::open;
      job->path = path;
      TryCatch try_catch(iso);
      if (!parse_open_flags(iso, ctx, info.Length() > 1 ? info[1] : Undefined(iso), job->flags)) {
        auto err = try_catch.Exception();
        try_catch.Reset();
        info.GetReturnValue().Set(rejected(iso, ctx, err));
        return;
      }
      job->mode = info.Length() > 2 && info[2]->IsNumber()
                      ? info[2]->Int32Value(ctx).FromMaybe(0666)
                      : 0666;
      queue_job(info, std::move(job));
    }

    void read_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::read;
      if (!prepare_buffer_job(iso, ctx, info, false, *job))
        return;
      queue_job(info, std::move(job));
    }

    void write_async(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::write;
      if (!prepare_buffer_job(iso, ctx, info, true, *job))
        return;
      queue_job(info, std::move(job));
    }

    void close_async(const FunctionCallbackInfo<Value>& info) {
      auto ctx = info.GetIsolate()->GetCurrentContext();
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::close;
      job->fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      queue_job(info, std::move(job));
    }

    void fstat_async(const FunctionCallbackInfo<Value>& info) {
      auto ctx = info.GetIsolate()->GetCurrentContext();
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::fstat;
      job->fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      queue_job(info, std::move(job));
    }

    void ftruncate_async(const FunctionCallbackInfo<Value>& info) {
      auto ctx = info.GetIsolate()->GetCurrentContext();
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::ftruncate;
      job->fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      job->count = info.Length() > 1 && !info[1]->IsNullOrUndefined()
                       ? info[1]->IntegerValue(ctx).FromMaybe(0)
                       : 0;
      queue_job(info, std::move(job));
    }

    void fdatasync_async(const FunctionCallbackInfo<Value>& info) {
      auto ctx = info.GetIsolate()->GetCurrentContext();
      auto job = std::make_unique<async_job>();
      job->kind = async_kind::fdatasync;
      job->fd = info.Length() > 0 ? info[0]->Int32Value(ctx).FromMaybe(-1) : -1;
      queue_job(info, std::move(job));
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> ns, const char* name,
                      FunctionCallback callback) {
      auto fn = Function::New(ctx, callback).ToLocalChecked();
      (void)ns->Set(ctx, str(iso, name), fn);
    }
  } // namespace

  void install_fs_fd_native(Isolate* iso, Local<Context> ctx, Local<Object> native) {
    auto ns = Object::New(iso);
    add_function(iso, ctx, ns, "openSync", open_sync);
    add_function(iso, ctx, ns, "readSync", read_sync);
    add_function(iso, ctx, ns, "writeSync", write_sync);
    add_function(iso, ctx, ns, "closeSync", close_sync);
    add_function(iso, ctx, ns, "fstatSync", fstat_sync);
    add_function(iso, ctx, ns, "ftruncateSync", ftruncate_sync);
    add_function(iso, ctx, ns, "fdatasyncSync", fdatasync_sync);
    add_function(iso, ctx, ns, "open", open_async);
    add_function(iso, ctx, ns, "read", read_async);
    add_function(iso, ctx, ns, "write", write_async);
    add_function(iso, ctx, ns, "close", close_async);
    add_function(iso, ctx, ns, "fstat", fstat_async);
    add_function(iso, ctx, ns, "ftruncate", ftruncate_async);
    add_function(iso, ctx, ns, "fdatasync", fdatasync_async);
    (void)native->Set(ctx, "fs_fd"_v8(iso), ns);
  }
} // namespace fxe::runtime
