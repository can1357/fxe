#include "bind_offscreen.hpp"
#include "weak_holder.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/offscreen.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_strings.hpp>
#if FXE_HAS_WGPU
#include "../wgpu/pipeline.hpp"
#endif

#include <cstring>
#include <exception>
#include <fxe/types.hpp>
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

    struct offscreen_holder : weak_holder<offscreen_holder> {
      std::unique_ptr<offscreen_renderer> owned;
    };

    void throw_type(Isolate* iso, const char* msg) {
      (void)throw_type_error(iso, msg);
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
      if (o->Get(ctx, "width"_v8(iso)).ToLocal(&v))
        opts.width = v->Uint32Value(ctx).FromMaybe(0);
      if (o->Get(ctx, "height"_v8(iso)).ToLocal(&v))
        opts.height = v->Uint32Value(ctx).FromMaybe(0);
      if (o->Get(ctx, "multisample"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
        opts.multisample = v->Uint32Value(ctx).FromMaybe(opts.multisample);
      if (o->Get(ctx, "mipLevels"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
        opts.mip_levels = v->Uint32Value(ctx).FromMaybe(opts.mip_levels);
      if (o->Get(ctx, "enableDepth"_v8(iso)).ToLocal(&v) && !v->IsUndefined())
        opts.enable_depth = v->BooleanValue(iso);
      // `parent`: existing Renderer/OffscreenRenderer whose device this
      // offscreen will share. Required for cross-renderer sampling, e.g.
      // when the offscreen's color attachment will be bound on the main
      // window renderer via `bindUserTexture(...)`.
      if (o->Get(ctx, "parent"_v8(iso)).ToLocal(&v) && !v->IsUndefined() && !v->IsNull() &&
          v->IsObject()) {
#if FXE_HAS_WGPU
        auto* parent_r = static_cast<renderer*>(unwrap(v.As<Object>(), TAG_RENDERER));
        if (auto* dpa = dynamic_cast<dawn_pipeline_device_access*>(parent_r)) {
          opts.parent_device = dpa->device();
          opts.parent_queue = dpa->queue();
        }
#endif
      }
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
        (void)throw_error(iso, e.what());
        return;
      }
      if (!r) {
        (void)throw_error(iso, "offscreen create failed");
        return;
      }

      auto& sheet = get_default_spritesheet();
      if (!sheet.textures.empty()) {
        const auto& tex = sheet.textures.front();
        if (!tex.pixels.empty() && tex.size.x > 0 && tex.size.y > 0)
          r->set_atlas(tex.size.x, tex.size.y, reinterpret_cast<const u8*>(tex.pixels.data()));
      }

      auto* h = new offscreen_holder{{}, std::move(r)};
      auto self = info.This();
      set_native(iso, self, h->owned.get(), TAG_RENDERER);
      h->bind(iso, self);
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

    // bindUserTexture(slot, sourceOrNull)
    //
    // Mirrors `Renderer.bindUserTexture` on the offscreen so surface-cache
    // bakes can replicate the parent renderer's slot bindings into their
    // offscreen before queueing a cached subtree. Without this, cached
    // `drawTextureQuad(slot=k, ...)` references baked into a parent surface
    // would sample the offscreen's placeholder atlas instead of the actual
    // nested surface, leaking garbage into the parent's bake.
    void offscreen_bind_user_texture(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_offscreen(info.This());
      if (!r)
        return;
      if (info.Length() < 2) {
        (void)throw_type_error(iso, "bindUserTexture(slot, offscreenOrNull)");
        return;
      }
      auto ctx = iso->GetCurrentContext();
      const u32 slot = info[0]->Uint32Value(ctx).FromMaybe(0);
#if FXE_HAS_WGPU
      if (info[1]->IsNullOrUndefined()) {
        r->bind_user_texture(slot, wgpu::TextureView{});
        return;
      }
      if (!info[1]->IsObject()) {
        (void)throw_type_error(iso, "bindUserTexture: source must be an OffscreenRenderer or null");
        return;
      }
      auto* inner_r = static_cast<renderer*>(unwrap(info[1].As<Object>(), TAG_RENDERER));
      auto* src = inner_r ? dynamic_cast<offscreen_renderer*>(inner_r) : nullptr;
      if (!src) {
        (void)throw_type_error(iso, "bindUserTexture: source must be an OffscreenRenderer");
        return;
      }
      auto view = src->color_texture_view();
      if (!view) {
        (void)throw_error(iso, "bindUserTexture: source has no sampleable color attachment");
        return;
      }
      r->bind_user_texture(slot, std::move(view));
#else
      (void)slot;
      (void)throw_error(iso, "bindUserTexture: WGPU backend not enabled");
#endif
    }
  } // namespace

  void install_offscreen_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, offscreen_constructor);
    tpl->SetClassName("OffscreenRenderer"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);
    tpl->Inherit(get_command_buffer_template(iso));

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "beginFrame", FunctionTemplate::New(iso, offscreen_begin_frame));
    proto->Set(iso, "endFrame", FunctionTemplate::New(iso, offscreen_end_frame));
    proto->Set(iso, "setClearColor", FunctionTemplate::New(iso, offscreen_set_clear_color));
    proto->Set(iso, "readPixels", FunctionTemplate::New(iso, offscreen_read_pixels));
    proto->Set(iso, "bindUserTexture", FunctionTemplate::New(iso, offscreen_bind_user_texture));

    global->Set(iso, "OffscreenRenderer", tpl);
    offscreen_tpl_table()[iso].Reset(iso, tpl);
  }
} // namespace fxe::js
