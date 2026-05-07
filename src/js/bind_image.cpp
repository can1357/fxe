// Match brew V8 ABI: pointer compression + sandbox are enabled in libv8.dylib.
#define V8_COMPRESS_POINTERS 1

// JS bindings for the `Image` namespace and ImageHandle wrapper. ImageHandle
// owns RGBA8 pixel data via std::shared_ptr<texture_data>, so handing the
// underlying buffer to a Spritesheet keeps it alive even after the JS handle
// is disposed.

#include "bind_image.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
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

    void image_finalizer(const WeakCallbackInfo<image_holder>& info) {
      delete info.GetParameter();
    }

    Local<Object> wrap_image(Isolate* iso, Local<Context> ctx, image_holder* h) {
      auto inst =
          image_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      inst->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      inst->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_IMAGE));
      auto* persistent = new Global<Object>(iso, inst);
      persistent->SetWeak(h, image_finalizer, WeakCallbackType::kParameter);
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
      return new image_holder{std::move(td)};
    }

    image_holder* make_holder_from_raw(const u8* bytes, usize len, u32 w, u32 h) {
      if (static_cast<u64>(w) * h * 4 != len)
        return nullptr;
      auto td = std::make_shared<texture_data>();
      td->size = {w, h};
      td->pixels.resize(static_cast<usize>(w) * h);
      std::memcpy(td->pixels.data(), bytes, len);
      return new image_holder{std::move(td)};
    }

    void throw_type(Isolate* iso, const char* msg) {
      iso->ThrowException(Exception::TypeError(
          String::NewFromUtf8(iso, msg, NewStringType::kNormal).ToLocalChecked()));
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
        (void)resolver->Reject(ctx,
                               Exception::TypeError(str(iso, "Image.loadAsync(path: string)")));
        return;
      }
      std::vector<u8> bytes;
      if (!read_file_bytes(utf8(iso, info[0]), bytes)) {
        (void)resolver->Reject(ctx, Exception::Error(str(iso, "Image.loadAsync: read failed")));
        return;
      }
      try {
        auto* h = make_holder_from_decoded(bytes);
        (void)resolver->Resolve(ctx, wrap_image(iso, ctx, h));
      } catch (const std::exception& e) {
        (void)resolver->Reject(ctx, Exception::Error(str(iso, e.what())));
      }
    }

    void s_fromBytes(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsUint8Array())
        return throw_type(iso, "Image.fromBytes(uint8: Uint8Array, [width, height])");
      auto u8a = info[0].As<Uint8Array>();
      std::vector<u8> bytes(u8a->ByteLength());
      u8a->CopyContents(bytes.data(), bytes.size());
      // Two-arg form: raw RGBA pixels with explicit width/height.
      if (info.Length() >= 3 && info[1]->IsNumber() && info[2]->IsNumber()) {
        u32 w = info[1]->Uint32Value(ctx).FromMaybe(0);
        u32 h = info[2]->Uint32Value(ctx).FromMaybe(0);
        auto* holder = make_holder_from_raw(bytes.data(), bytes.size(), w, h);
        if (!holder)
          return throw_type(iso, "Image.fromBytes: byte length mismatches width*height*4");
        info.GetReturnValue().Set(wrap_image(iso, ctx, holder));
        return;
      }
      // One-arg form: encoded image (PNG/JPEG/...). load_texture handles it.
      try {
        auto* h = make_holder_from_decoded(bytes);
        info.GetReturnValue().Set(wrap_image(iso, ctx, h));
      } catch (const std::exception& e) {
        throw_type(iso, e.what());
      }
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
    void m_dispose(const FunctionCallbackInfo<Value>& info) {
      auto* h = static_cast<image_holder*>(unwrap(info.This(), TAG_IMAGE));
      if (h)
        h->tex.reset();
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

  void install_image_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso);
    tpl->SetClassName(String::NewFromUtf8Literal(iso, "ImageHandle"));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "width", FunctionTemplate::New(iso, m_width));
    proto->Set(iso, "height", FunctionTemplate::New(iso, m_height));
    proto->Set(iso, "dispose", FunctionTemplate::New(iso, m_dispose));
    proto->Set(iso, "bytes", FunctionTemplate::New(iso, m_bytes));

    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "load", FunctionTemplate::New(iso, s_load));
    ns->Set(iso, "loadAsync", FunctionTemplate::New(iso, s_loadAsync));
    ns->Set(iso, "fromBytes", FunctionTemplate::New(iso, s_fromBytes));
    global->Set(iso, "Image", ns);

    image_tpl_table()[iso].Reset(iso, tpl);
  }
} // namespace fxe::js
