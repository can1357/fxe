// JS bindings for the `Image` namespace and ImageHandle wrapper. ImageHandle
// owns RGBA8 pixel data via std::shared_ptr<texture_data>, so handing the
// underlying buffer to a Spritesheet keeps it alive even after the JS handle
// is disposed.

#include "bind_image.hpp"
#include "runtime/uv_loop.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <fxe/js_bindings.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/texture_registry.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& image_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void image_reset_for_isolate(Isolate* iso) {
      auto& t = image_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct image_resetter_register {
      image_resetter_register() {
        register_template_resetter(&image_reset_for_isolate);
      }
    };
    static image_resetter_register s_image_resetter_register;

    Local<String> str(Isolate* iso, const char* s) {
      return String::NewFromUtf8(iso, s).ToLocalChecked();
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    texture_id ensure_holder_texture(image_holder* h) {
      if (!h || !h->tex)
        return null_texture;
      if (h->texture == null_texture) {
        h->texture = register_external_texture(h->tex);
      } else {
        refresh_external_texture(h->texture, h->tex);
      }
      return h->texture;
    }

    Local<Object> wrap_image(Isolate* iso, Local<Context> ctx, image_holder* h) {
      ensure_holder_texture(h);
      auto inst =
          image_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      set_native(iso, inst, h, TAG_IMAGE);
      h->bind(iso, inst);
      return inst;
    }

    bool read_file_bytes(const std::string& path, std::vector<u8>& out) {
      std::ifstream f(path, std::ios::binary);
      if (!f)
        return false;
      f.seekg(0, std::ios::end);
      auto sz = f.tellg();
      if (sz < 0)
        return false;
      out.resize(static_cast<usize>(sz));
      f.seekg(0, std::ios::beg);
      f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
      return f.good() || f.eof();
    }

    image_holder* make_holder_from_decoded(std::vector<u8>& encoded) {
      auto td = std::make_shared<texture_data>(load_texture(std::span<const u8>(encoded)));
      return new image_holder{{}, std::move(td)};
    }

    image_holder* make_holder_from_texture(std::unique_ptr<texture_data> decoded) {
      if (!decoded)
        return nullptr;
      auto td = std::make_shared<texture_data>(std::move(*decoded));
      return new image_holder{{}, std::move(td)};
    }

    struct decode_request {
#if FXE_HAS_LIBUV
      uv_work_t work{};
#endif
      Isolate* iso = nullptr;
      Global<Context> context;
      Global<Promise::Resolver> resolver;
      std::vector<u8> encoded;
      std::string path;
      std::unique_ptr<texture_data> result;
      std::string error;
      bool read_path = false;
      int uv_status = 0;
    };

    void image_decode_worker(decode_request& req) {
      try {
        if (req.read_path && !read_file_bytes(req.path, req.encoded)) {
          req.error = "Image.loadAsync: read failed";
          return;
        }
        req.result = std::make_unique<texture_data>(load_texture(std::span<const u8>(req.encoded)));
      } catch (const std::exception& e) {
        req.error = e.what();
      }
    }

    void image_decode_after(std::unique_ptr<decode_request> req) {
      auto* iso = req->iso;
      Locker locker(iso);
      Isolate::Scope isolate_scope(iso);
      HandleScope hs(iso);
      auto ctx = req->context.Get(iso);
      Context::Scope context_scope(ctx);
      auto resolver = req->resolver.Get(iso);

#if FXE_HAS_LIBUV
      if (req->uv_status < 0 && req->error.empty()) {
        req->error = std::string("Image decode worker failed: ") + uv_strerror(req->uv_status);
      }
#endif

      if (!req->error.empty()) {
        (void)resolver->Reject(ctx, Exception::Error(str(iso, req->error.c_str())));
        req->resolver.Reset();
        req->context.Reset();
        return;
      }

      auto* holder = make_holder_from_texture(std::move(req->result));
      (void)resolver->Resolve(ctx, wrap_image(iso, ctx, holder));
      req->resolver.Reset();
      req->context.Reset();
    }

#if FXE_HAS_LIBUV
    void image_decode_work_cb(uv_work_t* work) {
      image_decode_worker(*static_cast<decode_request*>(work->data));
    }

    void image_decode_after_cb(uv_work_t* work, int status) {
      auto* req = static_cast<decode_request*>(work->data);
      if (status < 0)
        req->uv_status = status;
      image_decode_after(std::unique_ptr<decode_request>(req));
    }
#endif

    image_holder* make_holder_from_raw(const u8* bytes, usize len, u32 w, u32 h) {
      if (static_cast<u64>(w) * h * 4 != len)
        return nullptr;
      auto td = std::make_shared<texture_data>();
      td->size = {w, h};
      td->pixels.resize(static_cast<usize>(w) * h);
      std::memcpy(td->pixels.data(), bytes, len);
      return new image_holder{{}, std::move(td)};
    }

    void throw_type(Isolate* iso, const char* msg) {
      (void)throw_type_error(iso, msg);
    }

    // ------------------------------------------------------------------------
    // Static methods on the `Image` namespace.
    // ------------------------------------------------------------------------
    void s_load(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString())
        return throw_type(iso, "Image.load(path: string)");
      std::vector<u8> bytes;
      if (!read_file_bytes(utf8(iso, info[0]), bytes))
        return throw_type(iso, "Image.load: failed to read file");
      try {
        // Intentionally synchronous for callers that explicitly want blocking decode.
        auto* h = make_holder_from_decoded(bytes);
        info.GetReturnValue().Set(wrap_image(iso, ctx, h));
      } catch (const std::exception& e) {
        throw_type(iso, e.what());
      }
    }

    void s_loadAsync(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)resolver->Reject(ctx, Exception::TypeError("Image.loadAsync(path: string)"_v8(iso)));
        return;
      }

      auto req = std::make_unique<decode_request>();
      req->read_path = true;
      req->path = utf8(iso, info[0]);
      req->iso = iso;
      req->context.Reset(iso, ctx);
      req->resolver.Reset(iso, resolver);
#if FXE_HAS_LIBUV
      if (auto* loop = fxe::runtime::default_loop()) {
        req->work.data = req.get();
        const int rc = uv_queue_work(loop, &req->work, image_decode_work_cb, image_decode_after_cb);
        if (rc == 0) {
          (void)req.release();
          return;
        }
        req->uv_status = rc;
      }
#endif
      image_decode_worker(*req);
      image_decode_after(std::move(req));
    }

    void s_fromPixels(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3 || !info[0]->IsUint8Array() || !info[1]->IsNumber() ||
          !info[2]->IsNumber())
        return throw_type(iso, "Image.fromPixels(rgba: Uint8Array, width: number, height: number)");
      auto u8a = info[0].As<Uint8Array>();
      std::vector<u8> bytes(u8a->ByteLength());
      u8a->CopyContents(bytes.data(), bytes.size());
      u32 w = info[1]->Uint32Value(ctx).FromMaybe(0);
      u32 h = info[2]->Uint32Value(ctx).FromMaybe(0);
      auto* holder = make_holder_from_raw(bytes.data(), bytes.size(), w, h);
      if (!holder)
        return throw_type(iso, "Image.fromPixels: byte length mismatches width*height*4");
      info.GetReturnValue().Set(wrap_image(iso, ctx, holder));
    }

    void s_decode(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsUint8Array())
        return throw_type(iso, "Image.decode(encoded: Uint8Array)");
      auto u8a = info[0].As<Uint8Array>();
      std::vector<u8> bytes(u8a->ByteLength());
      u8a->CopyContents(bytes.data(), bytes.size());

      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      auto req = std::make_unique<decode_request>();
      req->encoded = std::move(bytes);
      req->iso = iso;
      req->context.Reset(iso, ctx);
      req->resolver.Reset(iso, resolver);
#if FXE_HAS_LIBUV
      if (auto* loop = fxe::runtime::default_loop()) {
        req->work.data = req.get();
        const int rc = uv_queue_work(loop, &req->work, image_decode_work_cb, image_decode_after_cb);
        if (rc == 0) {
          (void)req.release();
          return;
        }
        req->uv_status = rc;
      }
#endif
      image_decode_worker(*req);
      image_decode_after(std::move(req));
    }

    // ------------------------------------------------------------------------
    // Instance methods.
    // ------------------------------------------------------------------------
    void m_width(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = static_cast<image_holder*>(unwrap(info.This(), TAG_IMAGE));
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h && h->tex ? h->tex->size.x : 0));
    }
    void m_height(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = static_cast<image_holder*>(unwrap(info.This(), TAG_IMAGE));
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h && h->tex ? h->tex->size.y : 0));
    }
    void m_texture_id(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = static_cast<image_holder*>(unwrap(info.This(), TAG_IMAGE));
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, ensure_holder_texture(h)));
    }

    void m_dispose(const FunctionCallbackInfo<Value>& info) {
      auto* h = static_cast<image_holder*>(unwrap(info.This(), TAG_IMAGE));
      if (!h)
        return;
      h->tex.reset();
      release_external_texture_if_unused(h->texture);
    }
    void m_bytes(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = static_cast<image_holder*>(unwrap(info.This(), TAG_IMAGE));
      if (!h || !h->tex) {
        info.GetReturnValue().SetUndefined();
        return;
      }
      const auto& tex = *h->tex;
      const usize byte_len = tex.pixels.size() * sizeof(r8g8b8a8);
      // Copy into a fresh ArrayBuffer so the view's lifetime is detached from
      // the holder. Disposing the image will not invalidate this Uint8Array.
      auto ab = ArrayBuffer::New(iso, byte_len);
      if (byte_len)
        std::memcpy(ab->Data(), tex.pixels.data(), byte_len);
      info.GetReturnValue().Set(Uint8Array::New(ab, 0, byte_len));
    }
  } // namespace

  image_holder* unwrap_image(Local<Value> v) {
    if (v.IsEmpty() || !v->IsObject())
      return nullptr;
    return static_cast<image_holder*>(unwrap(v.As<Object>(), TAG_IMAGE));
  }

  texture_id ensure_image_texture_id(image_holder* h) {
    return ensure_holder_texture(h);
  }

  void image_holder::on_finalize(v8::Isolate*) {
    release_external_texture_if_unused(texture);
  }

  void install_image_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso);
    tpl->SetClassName("ImageHandle"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "width", FunctionTemplate::New(iso, m_width));
    proto->Set(iso, "height", FunctionTemplate::New(iso, m_height));
    proto->Set(iso, "dispose", FunctionTemplate::New(iso, m_dispose));
    proto->Set(iso, "textureId", FunctionTemplate::New(iso, m_texture_id));
    proto->Set(iso, "bytes", FunctionTemplate::New(iso, m_bytes));

    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "load", FunctionTemplate::New(iso, s_load));
    ns->Set(iso, "loadAsync", FunctionTemplate::New(iso, s_loadAsync));
    ns->Set(iso, "decode", FunctionTemplate::New(iso, s_decode));
    ns->Set(iso, "fromPixels", FunctionTemplate::New(iso, s_fromPixels));
    global->Set(iso, "Image", ns);

    image_tpl_table()[iso].Reset(iso, tpl);
  }
} // namespace fxe::js
