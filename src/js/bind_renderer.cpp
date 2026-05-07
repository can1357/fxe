// JS bindings for fxe::renderer. Inherits from the CommandBuffer JS class so
// allocate/clear/transform/queue all show up on Renderer instances unchanged.
//
// Type tag 'REND'.

#include "bind_pipeline.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>
#include <fxe/window.hpp>

#include <memory>
#include <unordered_map>
#include <v8.h>

namespace fxe::js {
  // Defined in bind_command_buffer.cpp.
  v8::Local<v8::FunctionTemplate> get_command_buffer_template(v8::Isolate*);

  namespace {
    using namespace v8;
    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& rend_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void rend_reset_for_isolate(Isolate* iso) {
      auto& t = rend_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct rend_resetter_register {
      rend_resetter_register() {
        register_template_resetter(&rend_reset_for_isolate);
      }
    };
    static rend_resetter_register s_rend_resetter_register;

    // Owned-renderer holder. Borrowed wraps (install_renderer_global) skip
    // ownership so the engine retains lifetime control.
    struct rend_holder {
      std::unique_ptr<renderer> owned;
    };

    void rend_finalizer(const WeakCallbackInfo<rend_holder>& info) {
      auto* h = info.GetParameter();
      if (h && h->owned)
        unregister_renderer_for_isolate(info.GetIsolate(), h->owned.get());
      delete h;
    }

    renderer* unwrap_rend(Local<Object> self) {
      return static_cast<renderer*>(unwrap(self, TAG_RENDERER));
    }

    void rend_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        iso->ThrowException(Exception::TypeError(
            String::NewFromUtf8Literal(iso, "Renderer must be invoked with new")));
        return;
      }
      if (info.Length() < 1 || !info[0]->IsObject()) {
        iso->ThrowException(
            Exception::TypeError(String::NewFromUtf8Literal(iso, "Renderer(window, options)")));
        return;
      }
      auto* win = static_cast<window*>(unwrap(info[0].As<Object>(), TAG_WINDOW));
      if (!win) {
        iso->ThrowException(Exception::TypeError(
            String::NewFromUtf8Literal(iso, "Renderer: first arg must be Window")));
        return;
      }
      renderer_options opts;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto o = info[1].As<Object>();
        Local<Value> v;
        if (o->Get(ctx, String::NewFromUtf8Literal(iso, "multisampleCount")).ToLocal(&v))
          opts.multisample_count = v->Uint32Value(ctx).FromMaybe(opts.multisample_count);
        if (o->Get(ctx, String::NewFromUtf8Literal(iso, "enableBloom")).ToLocal(&v))
          opts.enable_bloom = v->BooleanValue(iso);
        if (o->Get(ctx, String::NewFromUtf8Literal(iso, "vsync")).ToLocal(&v))
          opts.vsync = v->BooleanValue(iso);
      }
      const auto& runner_overrides = get_runner_render_overrides();
      if (runner_overrides.override_multisample_count)
        opts.multisample_count = runner_overrides.multisample_count;
      if (runner_overrides.override_bloom)
        opts.enable_bloom = runner_overrides.enable_bloom;
      if (runner_overrides.override_vsync)
        opts.vsync = runner_overrides.vsync;
      auto r = create_renderer(*win, opts);
      if (!r) {
        iso->ThrowException(
            Exception::Error(String::NewFromUtf8Literal(iso, "create_renderer failed")));
        return;
      }
      // Lazily initialise the default font (system TTF or procedural fallback)
      // and push its glyph atlas into the freshly-built renderer so text
      // primitives sample real coverage instead of the 1x1 white default.
      {
        auto& sheet = get_default_spritesheet();
        if (!sheet.textures.empty()) {
          const auto& tex = sheet.textures.front();
          if (!tex.pixels.empty() && tex.size.x > 0 && tex.size.y > 0) {
            r->set_atlas(tex.size.x, tex.size.y, reinterpret_cast<const u8*>(tex.pixels.data()));
          }
        }
      }
      auto* h = new rend_holder{std::move(r)};
      auto self = info.This();
      self->SetInternalField(
          0, External::New(iso, h->owned.get(), v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_RENDERER));
      auto* persistent = new Global<Object>(iso, self);
      persistent->SetWeak(h, rend_finalizer, WeakCallbackType::kParameter);
      register_renderer_for_isolate(iso, win, h->owned.get());
    }

    void rend_set_multisample(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      auto ctx = iso->GetCurrentContext();
      auto n = info.Length() >= 1 ? info[0]->Uint32Value(ctx).FromMaybe(1) : 1u;
      r->set_multisample_count(n);
    }
    void rend_set_bloom(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      bool b = info.Length() >= 1 && info[0]->BooleanValue(iso);
      r->set_bloom_enabled(b);
    }
    void rend_screen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      auto s = r->get_screen();
      auto arr = Array::New(iso, 2);
      (void)arr->Set(ctx, 0, Number::New(iso, static_cast<double>(s.x)));
      (void)arr->Set(ctx, 1, Number::New(iso, static_cast<double>(s.y)));
      info.GetReturnValue().Set(arr);
    }
    void rend_world_to_screen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r || info.Length() < 1 || !info[0]->IsFloat32Array())
        return;
      auto a = info[0].As<Float32Array>();
      float v[3] = {0, 0, 0};
      a->CopyContents(v, sizeof(v));
      auto p = r->world_to_screen({v[0], v[1], v[2]});
      auto arr = Array::New(iso, 4);
      (void)arr->Set(ctx, 0, Number::New(iso, static_cast<double>(p.x)));
      (void)arr->Set(ctx, 1, Number::New(iso, static_cast<double>(p.y)));
      (void)arr->Set(ctx, 2, Number::New(iso, static_cast<double>(p.z)));
      (void)arr->Set(ctx, 3, Number::New(iso, static_cast<double>(p.w)));
      info.GetReturnValue().Set(arr);
    }
    void rend_viewport(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      const auto& vp = r->viewport();
      auto out = Object::New(iso);
      auto at = Array::New(iso, 2);
      (void)at->Set(ctx, 0, Number::New(iso, static_cast<double>(vp.at.x)));
      (void)at->Set(ctx, 1, Number::New(iso, static_cast<double>(vp.at.y)));
      auto sz = Array::New(iso, 2);
      (void)sz->Set(ctx, 0, Number::New(iso, static_cast<double>(vp.size.x)));
      (void)sz->Set(ctx, 1, Number::New(iso, static_cast<double>(vp.size.y)));
      (void)out->Set(ctx, String::NewFromUtf8Literal(iso, "at"), at);
      (void)out->Set(ctx, String::NewFromUtf8Literal(iso, "size"), sz);
      info.GetReturnValue().Set(out);
    }
    void rend_begin_frame(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      math::vec3 eye_pos{}, eye_dir{};
      math::mat4x4 wvp = math::identity();
      auto extract3 = [](Local<Value> v, math::vec3& out) {
        if (!v->IsFloat32Array())
          return;
        auto a = v.As<Float32Array>();
        if (a->Length() < 3)
          return;
        float t[3];
        a->CopyContents(t, sizeof(t));
        out = {t[0], t[1], t[2]};
      };
      if (info.Length() >= 1)
        extract3(info[0], eye_pos);
      if (info.Length() >= 2)
        extract3(info[1], eye_dir);
      if (info.Length() >= 3 && info[2]->IsFloat32Array()) {
        auto a = info[2].As<Float32Array>();
        if (a->Length() >= 16) {
          float t[16];
          a->CopyContents(t, sizeof(t));
          wvp = math::mat4x4(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10],
                             t[11], t[12], t[13], t[14], t[15]);
        }
      }
      r->begin_frame(eye_pos, eye_dir, wvp);
    }
    void rend_end_frame(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      r->end_frame();
    }
    void rend_set_clear_color(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r || info.Length() < 1)
        return;
      float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      auto read_num = [&](Local<Value> v, float fallback) -> float {
        if (v.IsEmpty())
          return fallback;
        return static_cast<float>(v->NumberValue(ctx).FromMaybe(static_cast<double>(fallback)));
      };
      if (info[0]->IsArray()) {
        auto a = info[0].As<Array>();
        u32 n = a->Length();
        for (u32 i = 0; i < n && i < 4; ++i) {
          Local<Value> v;
          if (a->Get(ctx, i).ToLocal(&v))
            c[i] = read_num(v, c[i]);
        }
      } else {
        c[0] = read_num(info[0], c[0]);
        if (info.Length() >= 2)
          c[1] = read_num(info[1], c[1]);
        if (info.Length() >= 3)
          c[2] = read_num(info[2], c[2]);
        if (info.Length() >= 4)
          c[3] = read_num(info[3], c[3]);
      }
      r->set_clear_color(math::vec4{c[0], c[1], c[2], c[3]});
    }
  } // namespace

  void install_renderer_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, rend_constructor);
    tpl->SetClassName(String::NewFromUtf8Literal(iso, "Renderer"));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);
    // Inherit allocate/clear/queue/transform/epoch/etc. from CommandBuffer.
    tpl->Inherit(get_command_buffer_template(iso));

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "beginFrame", FunctionTemplate::New(iso, rend_begin_frame));
    proto->Set(iso, "endFrame", FunctionTemplate::New(iso, rend_end_frame));
    proto->Set(iso, "setMultisample", FunctionTemplate::New(iso, rend_set_multisample));
    proto->Set(iso, "setBloom", FunctionTemplate::New(iso, rend_set_bloom));
    proto->Set(iso, "setClearColor", FunctionTemplate::New(iso, rend_set_clear_color));
    proto->Set(iso, "screen", FunctionTemplate::New(iso, rend_screen));
    proto->Set(iso, "worldToScreen", FunctionTemplate::New(iso, rend_world_to_screen));
    proto->Set(iso, "viewport", FunctionTemplate::New(iso, rend_viewport));

    global->Set(iso, "Renderer", tpl);
    install_pipeline_template(iso, global);
    rend_tpl_table()[iso].Reset(iso, tpl);
  }

  Local<Object> make_renderer_object(Isolate* iso, Local<Context> ctx, renderer* r) {
    return wrap(iso, ctx, rend_tpl_table()[iso].Get(iso), r, TAG_RENDERER);
  }
} // namespace fxe::js
