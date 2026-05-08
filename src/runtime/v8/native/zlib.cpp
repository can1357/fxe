#include "runtime/v8/native/zlib.hpp"

#include <fxe/types.hpp>
#include <fxe/v8_strings.hpp>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::runtime {
  namespace {
    using namespace v8;

    constexpr usize kGrowChunk = 64u * 1024u;

    Local<String> str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      FunctionCallback cb) {
      auto fn = Function::New(ctx, cb).ToLocalChecked();
      (void)obj->Set(ctx, str(iso, name), fn);
    }

    bool get_property(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      Local<Value>& out) {
      return obj->Get(ctx, str(iso, name)).ToLocal(&out) && !out->IsUndefined() && !out->IsNull();
    }

    int int_option(Isolate* iso, Local<Context> ctx, Local<Value> opts_value, const char* name,
                   int fallback) {
      if (opts_value.IsEmpty() || !opts_value->IsObject())
        return fallback;
      Local<Value> value;
      if (!get_property(iso, ctx, opts_value.As<Object>(), name, value))
        return fallback;
      return value->Int32Value(ctx).FromMaybe(fallback);
    }

    const char* zlib_code_name(int code) {
      switch (code) {
      case Z_ERRNO:
        return "Z_ERRNO";
      case Z_STREAM_ERROR:
        return "Z_STREAM_ERROR";
      case Z_DATA_ERROR:
        return "Z_DATA_ERROR";
      case Z_MEM_ERROR:
        return "Z_MEM_ERROR";
      case Z_BUF_ERROR:
        return "Z_BUF_ERROR";
      case Z_VERSION_ERROR:
        return "Z_VERSION_ERROR";
      default:
        return "Z_UNKNOWN_ERROR";
      }
    }

    void throw_js_error(Isolate* iso, Local<Context> ctx, std::string_view message,
                        std::string_view code) {
      auto err_value = Exception::Error(str(iso, message));
      if (!err_value->IsObject()) {
        iso->ThrowException(err_value);
        return;
      }
      auto err = err_value.As<Object>();
      (void)err->Set(ctx, "code"_v8(iso), str(iso, code));
      iso->ThrowException(err);
    }

    void throw_type_error(Isolate* iso, std::string_view message) {
      iso->ThrowException(Exception::TypeError(str(iso, message)));
    }

    void throw_range_error(Isolate* iso, std::string_view message) {
      iso->ThrowException(Exception::RangeError(str(iso, message)));
    }

    void throw_zlib_error(Isolate* iso, Local<Context> ctx, std::string_view op, int code,
                          const z_stream& stream) {
      std::string message(op);
      message += ": ";
      if (stream.msg != nullptr && *stream.msg != '\0') {
        message += stream.msg;
      } else {
        const char* zmsg = zError(code);
        message += zmsg ? zmsg : "zlib failure";
      }
      throw_js_error(iso, ctx, message, zlib_code_name(code));
    }

    bool fill_input(z_stream& stream, const Bytef* data, usize size, usize& consumed) {
      if (stream.avail_in != 0 || consumed >= size)
        return false;
      const usize remaining = size - consumed;
      const usize chunk = std::min<usize>(remaining, std::numeric_limits<uInt>::max());
      stream.next_in = const_cast<Bytef*>(data + consumed);
      stream.avail_in = static_cast<uInt>(chunk);
      consumed += chunk;
      return true;
    }

    bool read_bytes(Isolate* iso, Local<Value> value, bool allow_string, std::vector<u8>& owned,
                    const Bytef*& data, usize& size, std::string& error) {
      owned.clear();
      data = nullptr;
      size = 0;
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        size = view->ByteLength();
        data = size == 0 ? nullptr
                         : reinterpret_cast<const Bytef*>(backing->Data()) + view->ByteOffset();
        return true;
      }
      if (value->IsArrayBuffer()) {
        auto backing = value.As<ArrayBuffer>()->GetBackingStore();
        size = backing->ByteLength();
        data = size == 0 ? nullptr : reinterpret_cast<const Bytef*>(backing->Data());
        return true;
      }
      if (allow_string && value->IsString()) {
        String::Utf8Value utf8(iso, value);
        if (*utf8 == nullptr) {
          error = "string argument is not valid utf-8";
          return false;
        }
        owned.assign(reinterpret_cast<const u8*>(*utf8),
                     reinterpret_cast<const u8*>(*utf8) + utf8.length());
        size = owned.size();
        data = size == 0 ? nullptr : reinterpret_cast<const Bytef*>(owned.data());
        return true;
      }
      error = allow_string ? "expected Uint8Array, ArrayBuffer, or string"
                           : "expected Uint8Array or ArrayBuffer";
      return false;
    }

    Local<Uint8Array> copy_out(Isolate* iso, const std::vector<u8>& bytes) {
      auto ab = ArrayBuffer::New(iso, bytes.size());
      auto backing = ab->GetBackingStore();
      if (!bytes.empty())
        std::memcpy(backing->Data(), bytes.data(), bytes.size());
      return Uint8Array::New(ab, 0, bytes.size());
    }

    bool compress_sync(Isolate* iso, Local<Context> ctx, const Bytef* data, usize size, int level,
                       int window_bits, int mem_level, int strategy, std::vector<u8>& out,
                       std::string_view op) {
      if (size > std::numeric_limits<uInt>::max()) {
        throw_range_error(iso, "zlib input exceeds 4 GiB single-call limit");
        return false;
      }
      z_stream stream{};
      int rc = deflateInit2(&stream, level, Z_DEFLATED, window_bits, mem_level, strategy);
      if (rc != Z_OK) {
        throw_zlib_error(iso, ctx, op, rc, stream);
        return false;
      }
      stream.next_in = const_cast<Bytef*>(data);
      stream.avail_in = static_cast<uInt>(size);

      const usize bound = std::max<usize>(compressBound(static_cast<uLong>(size)) + 32u, 256u);
      out.assign(bound, 0);
      usize written = 0;
      bool ok = false;
      while (true) {
        if (written == out.size())
          out.resize(out.size() + kGrowChunk);
        const usize capacity = out.size() - written;
        stream.next_out = reinterpret_cast<Bytef*>(out.data() + written);
        stream.avail_out =
            static_cast<uInt>(std::min<usize>(capacity, std::numeric_limits<uInt>::max()));
        rc = deflate(&stream, Z_FINISH);
        written += capacity - stream.avail_out;
        if (rc == Z_STREAM_END) {
          ok = true;
          break;
        }
        if (rc == Z_OK && stream.avail_out == 0)
          continue;
        throw_zlib_error(iso, ctx, op, rc, stream);
        break;
      }
      deflateEnd(&stream);
      if (!ok)
        return false;
      out.resize(written);
      return true;
    }

    bool inflate_sync(Isolate* iso, Local<Context> ctx, const Bytef* data, usize size,
                      int window_bits, std::vector<u8>& out, std::string_view op) {
      if (size > std::numeric_limits<uInt>::max()) {
        throw_range_error(iso, "zlib input exceeds 4 GiB single-call limit");
        return false;
      }
      z_stream stream{};
      int rc = inflateInit2(&stream, window_bits);
      if (rc != Z_OK) {
        throw_zlib_error(iso, ctx, op, rc, stream);
        return false;
      }

      usize consumed = 0;
      out.clear();
      bool ok = false;
      while (true) {
        fill_input(stream, data, size, consumed);
        const usize base = out.size();
        out.resize(base + kGrowChunk);
        stream.next_out = reinterpret_cast<Bytef*>(out.data() + base);
        stream.avail_out = static_cast<uInt>(kGrowChunk);
        rc = inflate(&stream, Z_NO_FLUSH);
        const usize produced = kGrowChunk - stream.avail_out;
        out.resize(base + produced);
        if (rc == Z_STREAM_END) {
          ok = true;
          break;
        }
        if (rc == Z_OK)
          continue;
        throw_zlib_error(iso, ctx, op, rc, stream);
        break;
      }
      inflateEnd(&stream);
      if (!ok)
        return false;
      return true;
    }

    struct sync_options {
      int level = Z_DEFAULT_COMPRESSION;
      int window_bits = 15;
      int mem_level = 8;
      int strategy = Z_DEFAULT_STRATEGY;
    };

    sync_options deflate_options(Isolate* iso, Local<Context> ctx,
                                 const FunctionCallbackInfo<Value>& info, int default_window_bits) {
      Local<Value> opts = info.Length() > 1 ? info[1] : Undefined(iso);
      sync_options out;
      out.level = int_option(iso, ctx, opts, "level", Z_DEFAULT_COMPRESSION);
      out.window_bits = int_option(iso, ctx, opts, "windowBits", default_window_bits);
      out.mem_level = int_option(iso, ctx, opts, "memLevel", 8);
      out.strategy = int_option(iso, ctx, opts, "strategy", Z_DEFAULT_STRATEGY);
      return out;
    }

    int inflate_window_bits(Isolate* iso, Local<Context> ctx,
                            const FunctionCallbackInfo<Value>& info, int fallback) {
      Local<Value> opts = info.Length() > 1 ? info[1] : Undefined(iso);
      return int_option(iso, ctx, opts, "windowBits", fallback);
    }

    void deflate_sync_impl(const FunctionCallbackInfo<Value>& info, std::string_view op,
                           int default_window_bits) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_type_error(iso, "zlib input is required");
        return;
      }
      std::vector<u8> owned;
      const Bytef* data = nullptr;
      usize size = 0;
      std::string error;
      if (!read_bytes(iso, info[0], true, owned, data, size, error)) {
        throw_type_error(iso, error);
        return;
      }
      const auto opts = deflate_options(iso, ctx, info, default_window_bits);
      std::vector<u8> out;
      if (!compress_sync(iso, ctx, data, size, opts.level, opts.window_bits, opts.mem_level,
                         opts.strategy, out, op))
        return;
      info.GetReturnValue().Set(copy_out(iso, out));
    }

    void inflate_sync_impl(const FunctionCallbackInfo<Value>& info, std::string_view op,
                           int default_window_bits) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_type_error(iso, "zlib input is required");
        return;
      }
      std::vector<u8> owned;
      const Bytef* data = nullptr;
      usize size = 0;
      std::string error;
      if (!read_bytes(iso, info[0], false, owned, data, size, error)) {
        throw_type_error(iso, error);
        return;
      }
      std::vector<u8> out;
      if (!inflate_sync(iso, ctx, data, size,
                        inflate_window_bits(iso, ctx, info, default_window_bits), out, op))
        return;
      info.GetReturnValue().Set(copy_out(iso, out));
    }

    void deflate_sync(const FunctionCallbackInfo<Value>& info) {
      deflate_sync_impl(info, "deflateSync", 15);
    }

    void gzip_sync(const FunctionCallbackInfo<Value>& info) {
      deflate_sync_impl(info, "gzipSync", 15 + 16);
    }

    void deflate_raw_sync(const FunctionCallbackInfo<Value>& info) {
      deflate_sync_impl(info, "deflateRawSync", -15);
    }

    void inflate_sync_callback(const FunctionCallbackInfo<Value>& info) {
      inflate_sync_impl(info, "inflateSync", 15);
    }

    void gunzip_sync(const FunctionCallbackInfo<Value>& info) {
      inflate_sync_impl(info, "gunzipSync", 15 + 32);
    }

    void inflate_raw_sync(const FunctionCallbackInfo<Value>& info) {
      inflate_sync_impl(info, "inflateRawSync", -15);
    }

    void crc32_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_type_error(iso, "crc32 input is required");
        return;
      }
      std::vector<u8> owned;
      const Bytef* data = nullptr;
      usize size = 0;
      std::string error;
      if (!read_bytes(iso, info[0], true, owned, data, size, error)) {
        throw_type_error(iso, error);
        return;
      }
      uLong init = 0;
      if (info.Length() > 1)
        init = static_cast<uLong>(info[1]->Uint32Value(ctx).FromMaybe(0));
      const uLong value = crc32(init, data, static_cast<uInt>(size));
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, static_cast<u32>(value)));
    }

    void adler32_callback(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_type_error(iso, "adler32 input is required");
        return;
      }
      std::vector<u8> owned;
      const Bytef* data = nullptr;
      usize size = 0;
      std::string error;
      if (!read_bytes(iso, info[0], true, owned, data, size, error)) {
        throw_type_error(iso, error);
        return;
      }
      uLong init = 0;
      if (info.Length() > 1)
        init = static_cast<uLong>(info[1]->Uint32Value(ctx).FromMaybe(0));
      const uLong value = adler32(init, data, static_cast<uInt>(size));
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, static_cast<u32>(value)));
    }

    Local<Object> make_zlib_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "deflateSync", deflate_sync);
      add_function(iso, ctx, ns, "inflateSync", inflate_sync_callback);
      add_function(iso, ctx, ns, "gzipSync", gzip_sync);
      add_function(iso, ctx, ns, "gunzipSync", gunzip_sync);
      add_function(iso, ctx, ns, "deflateRawSync", deflate_raw_sync);
      add_function(iso, ctx, ns, "inflateRawSync", inflate_raw_sync);
      add_function(iso, ctx, ns, "crc32", crc32_callback);
      add_function(iso, ctx, ns, "adler32", adler32_callback);
      return ns;
    }
  } // namespace

  void install_native_zlib(Isolate* iso, Local<Context> ctx) {
    Local<Value> native_value;
    Local<Object> native;
    if (ctx->Global()->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) &&
        native_value->IsObject()) {
      native = native_value.As<Object>();
    } else {
      native = Object::New(iso);
      (void)ctx->Global()->DefineOwnProperty(ctx, "__fxe_native"_v8(iso), native,
                                             static_cast<PropertyAttribute>(DontEnum));
    }
    (void)native->Set(ctx, "zlib"_v8(iso), make_zlib_namespace(iso, ctx));
  }
} // namespace fxe::runtime
