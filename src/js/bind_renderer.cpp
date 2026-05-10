// JS bindings for fxe::renderer. Renderer composes its own command-buffer
// storage and exposes the CommandBuffer-compatible mutation methods explicitly.
//
// Type tag 'REND'.

#include "bind_pipeline.hpp"
#include "js_command_buffer.hpp"
#include "weak_holder.hpp"
#include <algorithm>
#include <cmath>
#include <fxe/js_bindings.hpp>
#include <fxe/offscreen.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <fxe/v8_template_cache.hpp>
#include <fxe/window.hpp>
#include <memory>
#include <unordered_map>
#include <v8.h>

namespace fxe::js {

  namespace {
    using namespace v8;
    struct rend_tag {};
    using rend_tpl_cache = template_isolate_cache<rend_tag>;

    // Owned-renderer holder. Borrowed wraps (install_renderer_global) skip
    // ownership so the engine retains lifetime control.
    struct rend_holder : weak_holder<rend_holder> {
      std::unique_ptr<renderer> owned;

      void on_finalize(v8::Isolate* iso) {
        if (owned)
          unregister_renderer_for_isolate(iso, owned.get());
      }
    };

    renderer* unwrap_rend(Local<Object> self) {
      return static_cast<renderer*>(unwrap(self, TAG_RENDERER));
    }

    void rend_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Renderer must be invoked with new");
        return;
      }
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "Renderer(window, options)");
        return;
      }
      auto* win = static_cast<window*>(unwrap(info[0].As<Object>(), TAG_WINDOW));
      if (!win) {
        (void)throw_type_error(iso, "Renderer: first arg must be Window");
        return;
      }
      renderer_options opts;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto o = info[1].As<Object>();
        opts.multisample_count =
            get_prop_or<u32>(ctx, o, "multisampleCount"_v8(iso), opts.multisample_count);
        opts.enable_bloom = get_prop_or<bool>(ctx, o, "enableBloom"_v8(iso), opts.enable_bloom);
        opts.vsync = get_prop_or<bool>(ctx, o, "vsync"_v8(iso), opts.vsync);
      }
      const auto& runner_overrides = get_runner_render_overrides();
      if (runner_overrides.override_multisample_count)
        opts.multisample_count = runner_overrides.multisample_count;
      if (runner_overrides.override_bloom)
        opts.enable_bloom = runner_overrides.enable_bloom;
      if (runner_overrides.override_vsync)
        opts.vsync = runner_overrides.vsync;
      std::unique_ptr<renderer> r;
      if (runner_overrides.override_render_surface &&
          runner_overrides.render_surface == runner_render_surface::offscreen) {
        offscreen_options offscreen_opts;
        auto size = win->framebuffer_size();
        offscreen_opts.width = std::max<u32>(size.x, 1u);
        offscreen_opts.height = std::max<u32>(size.y, 1u);
        offscreen_opts.multisample = opts.multisample_count;
        try {
          r = offscreen_renderer::create(offscreen_opts);
        } catch (const std::exception& e) {
          (void)throw_error(iso, "create offscreen renderer failed: {}", e.what());
          return;
        }
        if (r)
          r->set_bloom_enabled(opts.enable_bloom);
      } else {
        r = create_renderer(*win, opts);
      }
      if (!r) {
        (void)throw_error(iso, "create_renderer failed");
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
      auto* h = new rend_holder{{}, std::move(r)};
      auto self = info.This();
      set_native(iso, self, h->owned.get(), TAG_RENDERER);
      h->bind(iso, self);
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
      if (!r->set_multisample_count(n)) {
        (void)throw_error(iso, "setMultisample: unsupported multisample count {}", n);
        return;
      }
    }

    void rend_supported_multisample_counts(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      const auto counts = r->supported_multisample_counts();
      auto out = Array::New(iso, static_cast<int>(counts.size()));
      for (u32 i = 0; i < counts.size(); ++i)
        set_index(ctx, out, i, counts[i]);
      info.GetReturnValue().Set(out);
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

    void rend_set_self_backdrop_blur(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      auto ctx = iso->GetCurrentContext();
      const bool enabled = info.Length() >= 1 && info[0]->BooleanValue(iso);
      const double radius = info.Length() >= 2 ? info[1]->NumberValue(ctx).FromMaybe(24.0) : 24.0;
      r->set_self_backdrop_blur(
          enabled, std::isfinite(radius) && radius > 0.0 ? static_cast<float>(radius) : 24.0f);
    }
    // bindUserTexture(slot, source)
    //
    // `source` may be:
    //   * an OffscreenRenderer instance — its color attachment is bound,
    //   * `null` / `undefined` — clears the slot back to the placeholder.
    //
    // Slot is 0..3 (matches USER_TEX_FLAG slot mask in main.wgsl).
    //
    // The bind survives across frames; rebind after the source is resized
    // since the underlying TextureView identity changes.
    void rend_bind_user_texture(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      if (info.Length() < 2) {
        (void)throw_type_error(iso, "bindUserTexture(slot, offscreenOrNull)");
        return;
      }
      auto ctx = iso->GetCurrentContext();
      const u32 slot = info[0]->Uint32Value(ctx).FromMaybe(0);
#if FXE_HAS_WGPU
      // Null / undefined → clear the slot.
      if (info[1]->IsNullOrUndefined()) {
        r->bind_user_texture(slot, wgpu::TextureView{});
        return;
      }
      if (!info[1]->IsObject()) {
        (void)throw_type_error(iso, "bindUserTexture: source must be an OffscreenRenderer or null");
        return;
      }
      auto* inner_r = static_cast<renderer*>(unwrap(info[1].As<Object>(), TAG_RENDERER));
      auto* off = inner_r ? dynamic_cast<offscreen_renderer*>(inner_r) : nullptr;
      if (!off) {
        (void)throw_type_error(iso, "bindUserTexture: source must be an OffscreenRenderer");
        return;
      }
      auto view = off->color_texture_view();
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

    void rend_screen(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      auto s = r->get_screen();
      auto arr = Array::New(iso, 2);
      set_index(ctx, arr, 0, static_cast<double>(s.x));
      set_index(ctx, arr, 1, static_cast<double>(s.y));
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
      set_index(ctx, arr, 0, static_cast<double>(p.x));
      set_index(ctx, arr, 1, static_cast<double>(p.y));
      set_index(ctx, arr, 2, static_cast<double>(p.z));
      set_index(ctx, arr, 3, static_cast<double>(p.w));
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
      set_index(ctx, at, 0, static_cast<double>(vp.at.x));
      set_index(ctx, at, 1, static_cast<double>(vp.at.y));
      auto sz = Array::New(iso, 2);
      set_index(ctx, sz, 0, static_cast<double>(vp.size.x));
      set_index(ctx, sz, 1, static_cast<double>(vp.size.y));
      set_prop(ctx, out, "at"_v8, at);
      set_prop(ctx, out, "size"_v8, sz);
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
    command_view* unwrap_command_view(Local<Value> value) {
      if (value.IsEmpty() || !value->IsObject())
        return nullptr;
      auto obj = value.As<Object>();
      if (void* raw = unwrap(obj, TAG_COMMAND_BUFFER))
        return static_cast<js_command_buffer*>(raw);
      if (void* raw = unwrap(obj, TAG_RENDERER))
        return static_cast<renderer*>(raw);
      return nullptr;
    }

    Local<ArrayBuffer> array_buffer_view(Isolate* iso, void* data, usize bytes) {
      auto bs = ArrayBuffer::NewBackingStore(data, bytes, [](void*, usize, void*) {}, nullptr);
      return ArrayBuffer::New(iso, std::move(bs));
    }

    bool read_topology_arg(const FunctionCallbackInfo<Value>& info, u32 index, u32& top) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      top = info.Length() > static_cast<int>(index)
                ? info[static_cast<int>(index)]->Uint32Value(ctx).FromMaybe(0)
                : 0;
      if (top >= static_cast<u32>(vertex_topology::max)) {
        (void)throw_range_error(iso, "topology out of range");
        return false;
      }
      return true;
    }

    bool decode_mat4(Local<Value> value, math::mat4x4& out) {
      if (!value->IsFloat32Array())
        return false;
      auto arr = value.As<Float32Array>();
      if (arr->Length() < 16)
        return false;
      float tmp[16];
      arr->CopyContents(tmp, sizeof(tmp));
      out = math::mat4x4(tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7], tmp[8],
                         tmp[9], tmp[10], tmp[11], tmp[12], tmp[13], tmp[14], tmp[15]);
      return true;
    }

    bool decode_vec4(Isolate* iso, Local<Value> value, math::vec4& out) {
      if (value->IsFloat32Array()) {
        auto arr = value.As<Float32Array>();
        if (arr->Length() < 4)
          return false;
        float tmp[4];
        arr->CopyContents(tmp, sizeof(tmp));
        out = {tmp[0], tmp[1], tmp[2], tmp[3]};
        return true;
      }
      if (value->IsArray()) {
        auto arr = value.As<Array>();
        if (arr->Length() < 4)
          return false;
        auto ctx = iso->GetCurrentContext();
        float tmp[4];
        for (u32 i = 0; i != 4; ++i) {
          Local<Value> elt;
          if (!arr->Get(ctx, i).ToLocal(&elt))
            return false;
          tmp[i] = static_cast<float>(elt->NumberValue(ctx).FromMaybe(0.0));
        }
        out = {tmp[0], tmp[1], tmp[2], tmp[3]};
        return true;
      }
      return false;
    }

    void rend_clear(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (r)
        r->clear();
    }

    void rend_epoch(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (r)
        info.GetReturnValue().Set(to_v8(iso, r->epoch()));
    }

    void rend_vertex_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (r)
        info.GetReturnValue().Set(to_v8(iso, r->vertex_count()));
    }

    void rend_index_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      u32 top = 0;
      if (!read_topology_arg(info, 0, top))
        return;
      info.GetReturnValue().Set(to_v8(iso, r->index_count(static_cast<vertex_topology>(top))));
    }

    void rend_is_empty(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (r)
        info.GetReturnValue().Set(r->is_empty());
    }

    void rend_bounds(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      if (r->vertices().empty()) {
        info.GetReturnValue().SetNull();
        return;
      }
      auto [mn, mx] = r->get_boundaries();
      auto out = Object::New(iso);
      set_prop(ctx, out, "x"_v8, static_cast<double>(mn.x));
      set_prop(ctx, out, "y"_v8, static_cast<double>(mn.y));
      set_prop(ctx, out, "width"_v8, static_cast<double>(mx.x - mn.x));
      set_prop(ctx, out, "height"_v8, static_cast<double>(mx.y - mn.y));
      info.GetReturnValue().Set(out);
    }

    void rend_transform(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      math::mat4x4 m{1.0f};
      if (info.Length() < 1 || !decode_mat4(info[0], m)) {
        (void)throw_type_error(iso, "transform: expected Float32Array(16)");
        return;
      }
      r->transform(m);
    }

    void rend_queue(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      if (info.Length() < 1) {
        (void)throw_type_error(iso, "queue: expected CommandBuffer");
        return;
      }
      auto* other = unwrap_command_view(info[0]);
      if (!other) {
        (void)throw_type_error(iso, "queue: argument is not a CommandBuffer");
        return;
      }
      math::mat4x4 m = math::identity();
      if (info.Length() >= 2 && !info[1]->IsUndefined() && !decode_mat4(info[1], m)) {
        (void)throw_type_error(iso, "queue: mat must be Float32Array(16)");
        return;
      }
      std::optional<math::vec4> tint;
      if (info.Length() >= 3 && !info[2]->IsUndefined()) {
        math::vec4 t{1, 1, 1, 1};
        if (!decode_vec4(iso, info[2], t)) {
          (void)throw_type_error(iso, "queue: tint must be Float32Array(4)");
          return;
        }
        tint = t;
      }
      r->queue(*other, m, tint);
    }

    void rend_buffers(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      u32 top = 0;
      if (!read_topology_arg(info, 0, top))
        return;
      const auto verts = r->vertices();
      const auto idxs = r->indices(static_cast<vertex_topology>(top));
      auto out = Object::New(iso);
      auto vab = array_buffer_view(iso, const_cast<vertex*>(verts.data()), verts.size_bytes());
      auto iab = array_buffer_view(iso, const_cast<u32*>(idxs.data()), idxs.size_bytes());
      set_prop(ctx, out, "verts"_v8, Float32Array::New(vab, 0, verts.size_bytes() / sizeof(float)));
      set_prop(ctx, out, "idxs"_v8, Uint32Array::New(iab, 0, idxs.size()));
      set_prop(ctx, out, "epoch"_v8, r->epoch());
      info.GetReturnValue().Set(out);
    }

    void rend_vertex_buffer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      const auto verts = r->vertices();
      auto ab = array_buffer_view(iso, const_cast<vertex*>(verts.data()), verts.size_bytes());
      info.GetReturnValue().Set(Float32Array::New(ab, 0, verts.size_bytes() / sizeof(float)));
    }

    void rend_index_buffer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      u32 top = 0;
      if (!read_topology_arg(info, 0, top))
        return;
      const auto idxs = r->indices(static_cast<vertex_topology>(top));
      auto ab = array_buffer_view(iso, const_cast<u32*>(idxs.data()), idxs.size_bytes());
      info.GetReturnValue().Set(Uint32Array::New(ab, 0, idxs.size()));
    }

    void rend_allocate(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* r = unwrap_rend(info.This());
      if (!r)
        return;
      if (info.Length() < 3) {
        (void)throw_type_error(iso, "allocate(vtx, idx, top)");
        return;
      }
      const u32 vtx = info[0]->Uint32Value(ctx).FromMaybe(0);
      const u32 idx = info[1]->Uint32Value(ctx).FromMaybe(0);
      u32 top = 0;
      if (!read_topology_arg(info, 2, top))
        return;
      const auto topology = static_cast<vertex_topology>(top);
      const u32 base = r->vertex_count();
      const u32 index_base = r->index_count(topology);
      auto [verts, indices] = r->allocate(vtx, idx, topology);
      auto out = Object::New(iso);
      auto vab = array_buffer_view(iso, verts, static_cast<usize>(vtx) * sizeof(vertex));
      auto iab = array_buffer_view(iso, indices, static_cast<usize>(idx) * sizeof(u32));
      set_prop(ctx, out, "verts"_v8,
               Float32Array::New(vab, 0, static_cast<usize>(vtx) * sizeof(vertex) / sizeof(float)));
      set_prop(ctx, out, "idxs"_v8, Uint32Array::New(iab, 0, idx));
      set_prop(ctx, out, "base"_v8, base);
      set_prop(ctx, out, "indexBase"_v8, index_base);
      set_prop(ctx, out, "epoch"_v8, r->epoch());
      info.GetReturnValue().Set(out);
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
    tpl->SetClassName("Renderer"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto proto = tpl->PrototypeTemplate();

    proto->Set(iso, "beginFrame", FunctionTemplate::New(iso, rend_begin_frame));
    proto->Set(iso, "endFrame", FunctionTemplate::New(iso, rend_end_frame));
    proto->Set(iso, "setMultisample", FunctionTemplate::New(iso, rend_set_multisample));
    proto->Set(iso, "supportedMultisampleCounts",
               FunctionTemplate::New(iso, rend_supported_multisample_counts));

    proto->Set(iso, "setBloom", FunctionTemplate::New(iso, rend_set_bloom));
    proto->Set(iso, "setSelfBackdropBlur", FunctionTemplate::New(iso, rend_set_self_backdrop_blur));
    proto->Set(iso, "setClearColor", FunctionTemplate::New(iso, rend_set_clear_color));
    proto->Set(iso, "screen", FunctionTemplate::New(iso, rend_screen));
    proto->Set(iso, "bindUserTexture", FunctionTemplate::New(iso, rend_bind_user_texture));
    proto->Set(iso, "worldToScreen", FunctionTemplate::New(iso, rend_world_to_screen));
    proto->Set(iso, "viewport", FunctionTemplate::New(iso, rend_viewport));
    proto->Set(iso, "clear", FunctionTemplate::New(iso, rend_clear));
    proto->Set(iso, "epoch", FunctionTemplate::New(iso, rend_epoch));
    proto->Set(iso, "vertexCount", FunctionTemplate::New(iso, rend_vertex_count));
    proto->Set(iso, "indexCount", FunctionTemplate::New(iso, rend_index_count));
    proto->Set(iso, "bounds", FunctionTemplate::New(iso, rend_bounds));
    proto->Set(iso, "transform", FunctionTemplate::New(iso, rend_transform));
    proto->Set(iso, "queue", FunctionTemplate::New(iso, rend_queue));
    proto->Set(iso, "buffers", FunctionTemplate::New(iso, rend_buffers));
    proto->Set(iso, "vertexBuffer", FunctionTemplate::New(iso, rend_vertex_buffer));
    proto->Set(iso, "indexBuffer", FunctionTemplate::New(iso, rend_index_buffer));
    proto->Set(iso, "allocate", FunctionTemplate::New(iso, rend_allocate));
    proto->Set(iso, "isEmpty", FunctionTemplate::New(iso, rend_is_empty));

    global->Set(iso, "Renderer", tpl);
    install_pipeline_template(iso, global);
    rend_tpl_cache::install(iso, tpl);
  }

  Local<FunctionTemplate> get_renderer_template(Isolate* iso) {
    return rend_tpl_cache::resolve(iso);
  }

  Local<Object> make_renderer_object(Isolate* iso, Local<Context> ctx, renderer* r) {
    return wrap(iso, ctx, rend_tpl_cache::resolve(iso), r, TAG_RENDERER);
  }
} // namespace fxe::js
