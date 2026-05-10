// JS bindings for the `Image` namespace and ImageHandle wrapper. ImageHandle
// owns RGBA8 pixel data via std::shared_ptr<texture_data>, so handing the
// underlying buffer to a Spritesheet keeps it alive even after the JS handle
// is disposed.

#include "bind_image.hpp"
#include "runtime/uv_loop.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <fxe/image_animation.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/texture_registry.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
#include <optional>
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
    std::unordered_map<Isolate*, TplGlobal>& animated_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void image_reset_for_isolate(Isolate* iso) {
      auto& images = image_tpl_table();
      if (auto it = images.find(iso); it != images.end()) {
        it->second.Reset();
        images.erase(it);
      }
      auto& animated = animated_tpl_table();
      if (auto it = animated.find(iso); it != animated.end()) {
        it->second.Reset();
        animated.erase(it);
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

    void ensure_animated_textures(animated_image_holder* h) {
      if (!h)
        return;
      if (h->textures.size() < h->frames.size())
        h->textures.resize(h->frames.size(), null_texture);
      for (usize i = 0; i < h->frames.size(); ++i) {
        auto& tex = h->frames[i];
        if (!tex)
          continue;
        if (h->textures[i] == null_texture) {
          h->textures[i] = register_external_texture(tex);
        } else {
          refresh_external_texture(h->textures[i], tex);
        }
      }
    }

    image_holder* make_holder_from_shared(std::shared_ptr<texture_data> decoded,
                                          texture_id texture = null_texture) {
      if (!decoded)
        return nullptr;
      return new image_holder{{}, std::move(decoded), texture};
    }

    Local<Object> wrap_image(Isolate* iso, Local<Context> ctx, image_holder* h) {
      ensure_holder_texture(h);
      auto inst =
          image_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      set_native(iso, inst, h, TAG_IMAGE);
      h->bind(iso, inst);
      return inst;
    }

    Local<Object> wrap_animated(Isolate* iso, Local<Context> ctx, animated_image_holder* h) {
      ensure_animated_textures(h);
      auto inst =
          animated_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      set_native(iso, inst, h, TAG_ANIMATED_IMAGE);
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

    animated_image_holder* make_holder_from_animation(animated_image decoded) {
      auto* holder = new animated_image_holder{};
      holder->duration_ms = decoded.duration_ms;
      holder->delays_ms.reserve(decoded.frames.size());
      holder->frames.reserve(decoded.frames.size());
      for (auto& frame : decoded.frames) {
        holder->delays_ms.push_back(frame.delay_ms);
        holder->frames.push_back(std::make_shared<texture_data>(std::move(frame.image)));
      }
      ensure_animated_textures(holder);
      return holder;
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

    struct animated_decode_request {
#if FXE_HAS_LIBUV
      uv_work_t work{};
#endif
      Isolate* iso = nullptr;
      Global<Context> context;
      Global<Promise::Resolver> resolver;
      std::string path;
      std::vector<u8> encoded;
      std::optional<animated_image> result;
      std::string error;
      bool force_lottie = false;
      int uv_status = 0;
    };

    void animated_decode_worker(animated_decode_request& req) {
      try {
        if (!read_file_bytes(req.path, req.encoded)) {
          req.error = req.force_lottie ? "Image.loadLottie: read failed"
                                       : "Image.loadAnimated: read failed";
          return;
        }
        req.result = req.force_lottie
                         ? load_lottie_placeholder(std::span<const u8>(req.encoded), req.path)
                         : load_animated_image(std::span<const u8>(req.encoded), req.path);
      } catch (const std::exception& e) {
        req.error = e.what();
      }
    }

    void animated_decode_after(std::unique_ptr<animated_decode_request> req) {
      auto* iso = req->iso;
      Locker locker(iso);
      Isolate::Scope isolate_scope(iso);
      HandleScope hs(iso);
      auto ctx = req->context.Get(iso);
      Context::Scope context_scope(ctx);
      auto resolver = req->resolver.Get(iso);

#if FXE_HAS_LIBUV
      if (req->uv_status < 0 && req->error.empty()) {
        req->error =
            std::string("Animated image decode worker failed: ") + uv_strerror(req->uv_status);
      }
#endif

      if (!req->error.empty()) {
        (void)resolver->Reject(ctx, Exception::Error(str(iso, req->error.c_str())));
        req->resolver.Reset();
        req->context.Reset();
        return;
      }

      auto* holder = make_holder_from_animation(std::move(*req->result));
      (void)resolver->Resolve(ctx, wrap_animated(iso, ctx, holder));
      req->resolver.Reset();
      req->context.Reset();
    }

#if FXE_HAS_LIBUV
    void animated_decode_work_cb(uv_work_t* work) {
      animated_decode_worker(*static_cast<animated_decode_request*>(work->data));
    }

    void animated_decode_after_cb(uv_work_t* work, int status) {
      auto* req = static_cast<animated_decode_request*>(work->data);
      if (status < 0)
        req->uv_status = status;
      animated_decode_after(std::unique_ptr<animated_decode_request>(req));
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

    [[nodiscard]] usize animated_frame_index_at(animated_image_holder* h, double time_ms) noexcept {
      if (!h || h->frames.empty() || !std::isfinite(time_ms) || h->duration_ms == 0)
        return 0;
      double local = std::fmod(time_ms, static_cast<double>(h->duration_ms));
      if (local < 0)
        local += static_cast<double>(h->duration_ms);
      u32 elapsed = 0;
      for (usize i = 0; i < h->delays_ms.size(); ++i) {
        elapsed += h->delays_ms[i];
        if (local < static_cast<double>(elapsed))
          return i;
      }
      return h->frames.size() - 1;
    }

    Local<Object> wrap_frame_image(Isolate* iso, Local<Context> ctx, animated_image_holder* h,
                                   usize index) {
      ensure_animated_textures(h);
      return wrap_image(iso, ctx, make_holder_from_shared(h->frames[index], h->textures[index]));
    }

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

    void s_loadAnimated(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)resolver->Reject(ctx,
                               Exception::TypeError("Image.loadAnimated(path: string)"_v8(iso)));
        return;
      }

      auto req = std::make_unique<animated_decode_request>();
      req->path = utf8(iso, info[0]);
      req->iso = iso;
      req->context.Reset(iso, ctx);
      req->resolver.Reset(iso, resolver);
#if FXE_HAS_LIBUV
      if (auto* loop = fxe::runtime::default_loop()) {
        req->work.data = req.get();
        const int rc =
            uv_queue_work(loop, &req->work, animated_decode_work_cb, animated_decode_after_cb);
        if (rc == 0) {
          (void)req.release();
          return;
        }
        req->uv_status = rc;
      }
#endif
      animated_decode_worker(*req);
      animated_decode_after(std::move(req));
    }

    void s_loadLottie(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)resolver->Reject(ctx, Exception::TypeError("Image.loadLottie(path: string)"_v8(iso)));
        return;
      }

      auto req = std::make_unique<animated_decode_request>();
      req->force_lottie = true;
      req->path = utf8(iso, info[0]);
      req->iso = iso;
      req->context.Reset(iso, ctx);
      req->resolver.Reset(iso, resolver);
#if FXE_HAS_LIBUV
      if (auto* loop = fxe::runtime::default_loop()) {
        req->work.data = req.get();
        const int rc =
            uv_queue_work(loop, &req->work, animated_decode_work_cb, animated_decode_after_cb);
        if (rc == 0) {
          (void)req.release();
          return;
        }
        req->uv_status = rc;
      }
#endif
      animated_decode_worker(*req);
      animated_decode_after(std::move(req));
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
      auto ab = ArrayBuffer::New(iso, byte_len);
      if (byte_len)
        std::memcpy(ab->Data(), tex.pixels.data(), byte_len);
      info.GetReturnValue().Set(Uint8Array::New(ab, 0, byte_len));
    }

    void a_frame(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = static_cast<animated_image_holder*>(unwrap(info.This(), TAG_ANIMATED_IMAGE));
      if (!h || h->frames.empty()) {
        info.GetReturnValue().SetUndefined();
        return;
      }
      if (info.Length() < 1 || !info[0]->IsNumber())
        return throw_type(iso, "AnimatedImage.frame(t: number)");
      const double time_ms = info[0]->NumberValue(ctx).FromMaybe(0.0);
      if (!std::isfinite(time_ms))
        return throw_type(iso, "AnimatedImage.frame: t must be a finite number");
      const usize index = animated_frame_index_at(h, time_ms);
      info.GetReturnValue().Set(wrap_frame_image(iso, ctx, h, index));
    }

    void a_dispose(const FunctionCallbackInfo<Value>& info) {
      auto* h = static_cast<animated_image_holder*>(unwrap(info.This(), TAG_ANIMATED_IMAGE));
      if (!h)
        return;
      for (texture_id texture : h->textures)
        release_external_texture_if_unused(texture);
      h->textures.clear();
      h->frames.clear();
      h->delays_ms.clear();
      h->duration_ms = 0;
    }

    void a_get_frame_count(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = static_cast<animated_image_holder*>(unwrap(info.HolderV2(), TAG_ANIMATED_IMAGE));
      info.GetReturnValue().Set(
          Integer::NewFromUnsigned(iso, h ? static_cast<u32>(h->frames.size()) : 0));
    }

    void a_get_duration_ms(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = static_cast<animated_image_holder*>(unwrap(info.HolderV2(), TAG_ANIMATED_IMAGE));
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h ? h->duration_ms : 0));
    }

    void a_get_frames(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = static_cast<animated_image_holder*>(unwrap(info.HolderV2(), TAG_ANIMATED_IMAGE));
      if (!h) {
        info.GetReturnValue().Set(Array::New(iso, 0));
        return;
      }
      auto arr = Array::New(iso, static_cast<int>(h->frames.size()));
      for (usize i = 0; i < h->frames.size(); ++i) {
        auto entry = Object::New(iso);
        (void)entry->Set(ctx, "delayMs"_v8(iso), Integer::NewFromUnsigned(iso, h->delays_ms[i]));
        (void)entry->Set(ctx, "image"_v8(iso), wrap_frame_image(iso, ctx, h, i));
        (void)arr->Set(ctx, static_cast<uint32_t>(i), entry);
      }
      info.GetReturnValue().Set(arr);
    }
  } // namespace

  image_holder* unwrap_image(Local<Value> v) {
    if (v.IsEmpty() || !v->IsObject())
      return nullptr;
    return static_cast<image_holder*>(unwrap(v.As<Object>(), TAG_IMAGE));
  }

  animated_image_holder* unwrap_animated_image(Local<Value> v) {
    if (v.IsEmpty() || !v->IsObject())
      return nullptr;
    return static_cast<animated_image_holder*>(unwrap(v.As<Object>(), TAG_ANIMATED_IMAGE));
  }

  texture_id ensure_image_texture_id(image_holder* h) {
    return ensure_holder_texture(h);
  }

  void image_holder::on_finalize(v8::Isolate*) {
    release_external_texture_if_unused(texture);
  }

  void animated_image_holder::on_finalize(v8::Isolate*) {
    for (texture_id texture : textures)
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

    auto animated_tpl = FunctionTemplate::New(iso);
    animated_tpl->SetClassName("AnimatedImageHandle"_v8(iso));
    auto animated_inst = animated_tpl->InstanceTemplate();
    animated_inst->SetInternalFieldCount(2);
    animated_inst->SetNativeDataProperty("frameCount"_v8(iso), a_get_frame_count, nullptr);
    animated_inst->SetNativeDataProperty("durationMs"_v8(iso), a_get_duration_ms, nullptr);
    animated_inst->SetNativeDataProperty("frames"_v8(iso), a_get_frames, nullptr);
    auto animated_proto = animated_tpl->PrototypeTemplate();
    animated_proto->Set(iso, "frame", FunctionTemplate::New(iso, a_frame));
    animated_proto->Set(iso, "dispose", FunctionTemplate::New(iso, a_dispose));

    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "load", FunctionTemplate::New(iso, s_load));
    ns->Set(iso, "loadAsync", FunctionTemplate::New(iso, s_loadAsync));
    ns->Set(iso, "loadAnimated", FunctionTemplate::New(iso, s_loadAnimated));
    ns->Set(iso, "loadLottie", FunctionTemplate::New(iso, s_loadLottie));
    ns->Set(iso, "decode", FunctionTemplate::New(iso, s_decode));
    ns->Set(iso, "fromPixels", FunctionTemplate::New(iso, s_fromPixels));
    global->Set(iso, "Image", ns);

    image_tpl_table()[iso].Reset(iso, tpl);
    animated_tpl_table()[iso].Reset(iso, animated_tpl);
  }
} // namespace fxe::js
