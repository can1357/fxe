// Match brew V8 ABI: pointer compression + sandbox are enabled in libv8.dylib.
#define V8_COMPRESS_POINTERS 1

#include "bind_offscreen.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/offscreen.hpp>
#include <fxe/spritesheet.hpp>

#include <cstring>
#include <exception>
#include <memory>
#include <unordered_map>
#include <v8.h>

namespace fxe::js {
  v8::Local<v8::FunctionTemplate> get_command_buffer_template(v8::Isolate*);

  namespace {
    using namespace v8;
    using TplGlobal = Global<FunctionTemplate>;

    std::unordered_map<Isolate*, TplGlobal>& offscreen_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }

    void offscreen_reset_for_isolate(Isolate* iso) {
      auto& t = offscreen_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }

    struct offscreen_resetter_register {
      offscreen_resetter_register() {
        register_template_resetter(&offscreen_reset_for_isolate);
      }
    };
    static offscreen_resetter_register s_offscreen_resetter_register;

    struct offscreen_holder {
      std::unique_ptr<offscreen_renderer> owned;
    };

    void offscreen_finalizer(const WeakCallbackInfo<offscreen_holder>& info) {
      delete info.GetParameter();
    }

    void throw_type(Isolate* iso, const char* msg) {
      iso->ThrowException(Exception::TypeError(
          String::NewFromUtf8(iso, msg, NewStringType::kNormal).ToLocalChecked()));
    }

    offscreen_renderer* unwrap_offscreen(Local<Object> self) {
      auto* r = static_cast<renderer*>(unwrap(self, TAG_RENDERER));
      return r ? dynamic_cast<offscreen_renderer*>(r) : nullptr;
    }

    void read_options(Isolate* iso, Local<Context> ctx, Local<Value> value, offscreen_options& opts,
                      bool& ok) {
      ok = false;
      if (value.IsEmpty() || !value->IsObject())
        return;
      auto o = value.As<Object>();
      Local<Value> v;
      if (o->Get(ctx, String::NewFromUtf8Literal(iso, "width")).ToLocal(&v))
        opts.width = v->Uint32Value(ctx).FromMaybe(0);
      if (o->Get(ctx, String::NewFromUtf8Literal(iso, "height")).ToLocal(&v))
        opts.height = v->Uint32Value(ctx).FromMaybe(0);
      if (o->Get(ctx, String::NewFromUtf8Literal(iso, "multisample")).ToLocal(&v) &&
          !v->IsUndefined())
        opts.multisample = v->Uint32Value(ctx).FromMaybe(opts.multisample);
      if (o->Get(ctx, String::NewFromUtf8Literal(iso, "mipLevels")).ToLocal(&v) &&
          !v->IsUndefined())
        opts.mip_levels = v->Uint32Value(ctx).FromMaybe(opts.mip_levels);
      if (o->Get(ctx, String::NewFromUtf8Literal(iso, "enableDepth")).ToLocal(&v) &&
          !v->IsUndefined())
        opts.enable_depth = v->BooleanValue(iso);
      ok = opts.width > 0 && opts.height > 0;
    }

    void offscreen_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "OffscreenRenderer must be invoked with new");
        return;
      }
      offscreen_options opts;
      bool ok = false;
      if (info.Length() >= 1)
        read_options(iso, ctx, info[0], opts, ok);
      if (!ok) {
        throw_type(iso,
                   "OffscreenRenderer({ width, height, multisample?, mipLevels?, enableDepth? })");
        return;
      }

      std::unique_ptr<offscreen_renderer> r;
      try {
        r = offscreen_renderer::create(opts);
      } catch (const std::exception& e) {
        iso->ThrowException(Exception::Error(
            String::NewFromUtf8(iso, e.what(), NewStringType::kNormal).ToLocalChecked()));
        return;
      }
      if (!r) {
        iso->ThrowException(
            Exception::Error(String::NewFromUtf8Literal(iso, "offscreen create failed")));
        return;
      }

      auto& sheet = get_default_spritesheet();
      if (!sheet.textures.empty()) {
        const auto& tex = sheet.textures.front();
        if (!tex.pixels.empty() && tex.size.x > 0 && tex.size.y > 0)
          r->set_atlas(tex.size.x, tex.size.y, reinterpret_cast<const u8*>(tex.pixels.data()));
      }

      auto* h = new offscreen_holder{std::move(r)};
      auto self = info.This();
      self->SetInternalField(
          0, External::New(iso, h->owned.get(), v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_RENDERER));
      auto* persistent = new Global<Object>(iso, self);
      persistent->SetWeak(h, offscreen_finalizer, WeakCallbackType::kParameter);
    }

    bool read_vec3(Local<Value> value, math::vec3& out) {
      if (!value->IsFloat32Array())
        return false;
      auto a = value.As<Float32Array>();
      if (a->Length() < 3)
        return false;
      float t[3];
      a->CopyContents(t, sizeof(t));
      out = {t[0], t[1], t[2]};
      return true;
    }

    bool read_mat4(Local<Value> value, math::mat4x4& out) {
      if (!value->IsFloat32Array())
        return false;
      auto a = value.As<Float32Array>();
      if (a->Length() < 16)
        return false;
      float t[16];
      a->CopyContents(t, sizeof(t));
      out = math::mat4x4(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11],
                         t[12], t[13], t[14], t[15]);
      return true;
    }

    void offscreen_begin_frame(const FunctionCallbackInfo<Value>& info) {
      auto* r = unwrap_offscreen(info.This());
      if (!r)
        return;
      math::vec3 eye_pos{}, eye_dir{};
      math::mat4x4 wvp = math::identity();
      if (info.Length() >= 1)
        (void)read_vec3(info[0], eye_pos);
      if (info.Length() >= 2)
        (void)read_vec3(info[1], eye_dir);
      if (info.Length() >= 3)
        (void)read_mat4(info[2], wvp);
      r->begin_frame(eye_pos, eye_dir, wvp);
    }

    void offscreen_end_frame(const FunctionCallbackInfo<Value>& info) {
      auto* r = unwrap_offscreen(info.This());
      if (r)
        r->end_frame();
    }

    void offscreen_set_clear_color(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_offscreen(info.This());
      if (!r)
        return;
      float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      if (info.Length() == 1 && info[0]->IsArray()) {
        auto a = info[0].As<Array>();
        for (u32 i = 0; i < 4 && i < a->Length(); ++i) {
          Local<Value> v;
          if (a->Get(ctx, i).ToLocal(&v))
            c[i] = static_cast<float>(v->NumberValue(ctx).FromMaybe(static_cast<double>(c[i])));
        }
      } else {
        for (int i = 0; i < info.Length() && i < 4; ++i)
          c[i] = static_cast<float>(info[i]->NumberValue(ctx).FromMaybe(static_cast<double>(c[i])));
      }
      r->set_clear_color({c[0], c[1], c[2], c[3]});
    }

    void offscreen_read_pixels(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_offscreen(info.This());
      if (!r)
        return;
      auto pixels = r->read_rgba8();
      auto ab = ArrayBuffer::New(iso, pixels.size());
      if (!pixels.empty())
        std::memcpy(ab->Data(), pixels.data(), pixels.size());
      info.GetReturnValue().Set(Uint8Array::New(ab, 0, pixels.size()));
    }
  } // namespace

  void install_offscreen_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, offscreen_constructor);
    tpl->SetClassName(String::NewFromUtf8Literal(iso, "OffscreenRenderer"));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);
    tpl->Inherit(get_command_buffer_template(iso));

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "beginFrame", FunctionTemplate::New(iso, offscreen_begin_frame));
    proto->Set(iso, "endFrame", FunctionTemplate::New(iso, offscreen_end_frame));
    proto->Set(iso, "setClearColor", FunctionTemplate::New(iso, offscreen_set_clear_color));
    proto->Set(iso, "readPixels", FunctionTemplate::New(iso, offscreen_read_pixels));

    global->Set(iso, "OffscreenRenderer", tpl);
    offscreen_tpl_table()[iso].Reset(iso, tpl);
  }
} // namespace fxe::js
