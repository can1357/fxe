// JS bindings for fxe::primitives::*. Registers a `Primitives` namespace
// object on the isolate global with one camelCase function per primitive.
//
// Every function takes a CommandBuffer (or Renderer) as its first argument.
// Subsequent args follow the C++ signature; colors accept either a packed
// RGBA8 number (0xRRGGBBAA) or a 4-tuple of floats in [0,1].

#include "bind_font.hpp"
#include <fxe/command_buffer.hpp>
#include <fxe/font.hpp>
#include <fxe/font/glyph.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_strings.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <v8.h>
#include <vector>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_PATH = 0x50415448u;        // 'PATH'
    constexpr u32 PAINT_KIND_PROP = 0x46505850u; // internal marker value

    struct path_holder {
      primitives::path_2d path;
      Global<Object> self;
    };

    command_buffer* unwrap_any_cb(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      auto o = v.As<Object>();
      if (auto* p = static_cast<command_buffer*>(unwrap(o, TAG_COMMAND_BUFFER)))
        return p;
      if (auto* p = static_cast<command_buffer*>(unwrap(o, TAG_RENDERER)))
        return p;
      return nullptr;
    }

    void path_finalizer(const WeakCallbackInfo<path_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }

    path_holder* unwrap_path(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      return static_cast<path_holder*>(unwrap(v.As<Object>(), TAG_PATH));
    }

    enum drain_opcode : u32 {
      OP_FILL_RECT = 1,
      OP_DRAW_RECT = 2,
      OP_FILL_TRIANGLE = 3,
      OP_DRAW_LINE = 4,
      OP_DRAW_TEXT = 5,
      OP_FILL_PATH = 6,
      OP_STROKE_PATH = 7,
    };

    r8g8b8a8 color_from_floats(const float* p) {
      auto clamp = [](float x) { return u8(x < 0.0f ? 0 : x > 1.0f ? 255 : x * 255.0f); };
      return {clamp(p[0]), clamp(p[1]), clamp(p[2]), clamp(p[3])};
    }

    bool require_params(Isolate* iso, usize pos, usize need, usize len, const char* op) {
      if (pos + need <= len)
        return true;
      std::string msg = std::string("Primitives.drain: truncated params for ") + op;
      (void)throw_range_error(iso, msg);
      return false;
    }

    r8g8b8a8 decode_color([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> v) {
      if (v->IsNumber()) {
        auto u = v->Uint32Value(ctx).FromMaybe(0xffffffffu);
        return r8g8b8a8(u);
      }
      if (v->IsArray()) {
        auto arr = v.As<Array>();
        float c[4] = {1, 1, 1, 1};
        for (u32 i = 0; i < 4 && i < arr->Length(); ++i) {
          Local<Value> e;
          if (arr->Get(ctx, i).ToLocal(&e))
            c[i] = static_cast<float>(e->NumberValue(ctx).FromMaybe(static_cast<double>(c[i])));
        }
        auto clamp = [](float x) { return u8(x < 0 ? 0 : x > 1 ? 255 : x * 255.0f); };
        return {clamp(c[0]), clamp(c[1]), clamp(c[2]), clamp(c[3])};
      }
      return white;
    }

    bool decode_vec4(Local<Value> v, math::vec4& out);

    primitives::paint_value decode_paint(Isolate* iso, Local<Context> ctx, Local<Value> v) {
      if (v->IsObject() && !v->IsArray() && !v->IsFloat32Array() && !v->IsNumber()) {
        auto obj = v.As<Object>();
        Local<Value> marker;
        if (obj->Get(ctx, "__fxePaint"_v8(iso)).ToLocal(&marker) &&
            marker->Uint32Value(ctx).FromMaybe(0) == PAINT_KIND_PROP) {
          primitives::paint_value paint;
          Local<Value> kind_v;
          if (obj->Get(ctx, "kind"_v8(iso)).ToLocal(&kind_v))
            paint.kind = static_cast<primitives::paint_kind>(kind_v->Uint32Value(ctx).FromMaybe(0));
          Local<Value> p0_v;
          if (obj->Get(ctx, "p0"_v8(iso)).ToLocal(&p0_v))
            (void)decode_vec4(p0_v, paint.p0);
          Local<Value> p1_v;
          if (obj->Get(ctx, "p1"_v8(iso)).ToLocal(&p1_v))
            (void)decode_vec4(p1_v, paint.p1);
          Local<Value> stops_v;
          if (obj->Get(ctx, "stops"_v8(iso)).ToLocal(&stops_v) && stops_v->IsFloat32Array()) {
            auto stops = stops_v.As<Float32Array>();
            std::vector<float> raw(stops->Length());
            stops->CopyContents(raw.data(), raw.size() * sizeof(float));
            for (usize i = 0; i + 4 < raw.size(); i += 5) {
              paint.stops.push_back({raw[i], color_from_floats(&raw[i + 1])});
            }
            std::sort(paint.stops.begin(), paint.stops.end(),
                      [](const auto& a, const auto& b) { return a.t < b.t; });
            if (!paint.stops.empty())
              paint.color = paint.stops.front().color;
          }
          return paint;
        }
      }
      return primitives::paint_value::solid(decode_color(iso, ctx, v));
    }

    bool decode_mat4(Local<Value> v, math::mat4x4& out) {
      if (!v->IsFloat32Array())
        return false;
      auto a = v.As<Float32Array>();
      if (a->Length() < 16)
        return false;
      float t[16];
      a->CopyContents(t, sizeof(t));
      out = math::mat4x4(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11],
                         t[12], t[13], t[14], t[15]);
      return true;
    }

    bool decode_vec4(Local<Value> v, math::vec4& out) {
      if (v->IsFloat32Array()) {
        auto a = v.As<Float32Array>();
        if (a->Length() < 4)
          return false;
        float t[4];
        a->CopyContents(t, sizeof(t));
        out = {t[0], t[1], t[2], t[3]};
        return true;
      }
      if (v->IsArray()) {
        auto a = v.As<Array>();
        if (a->Length() < 4)
          return false;
        auto* iso = Isolate::GetCurrent();
        auto ctx = iso->GetCurrentContext();
        float t[4];
        for (u32 i = 0; i < 4; ++i) {
          Local<Value> elt;
          if (!a->Get(ctx, i).ToLocal(&elt))
            return false;
          t[i] = static_cast<float>(elt->NumberValue(ctx).FromMaybe(0.0));
        }
        out = {t[0], t[1], t[2], t[3]};
        return true;
      }
      return false;
    }
    bool decode_vec2(Local<Value> v, math::vec2& out) {
      if (v->IsFloat32Array()) {
        auto a = v.As<Float32Array>();
        if (a->Length() < 2)
          return false;
        float t[2];
        a->CopyContents(t, sizeof(t));
        out = {t[0], t[1]};
        return true;
      }
      if (v->IsArray()) {
        auto a = v.As<Array>();
        if (a->Length() < 2)
          return false;
        auto* iso = Isolate::GetCurrent();
        auto ctx = iso->GetCurrentContext();
        float t[2];
        for (u32 i = 0; i < 2; ++i) {
          Local<Value> elt;
          if (!a->Get(ctx, i).ToLocal(&elt))
            return false;
          t[i] = static_cast<float>(elt->NumberValue(ctx).FromMaybe(0.0));
        }
        out = {t[0], t[1]};
        return true;
      }
      return false;
    }

    double num(Local<Context> ctx, Local<Value> v, double def = 0) {
      return v->NumberValue(ctx).FromMaybe(def);
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    // Helper: argument prelude. Returns nullptr (and throws) on missing cb.
    command_buffer* get_cb(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1) {
        (void)throw_type_error(iso, "missing CommandBuffer");
        return nullptr;
      }
      auto* cb = unwrap_any_cb(info[0]);
      if (!cb) {
        (void)throw_type_error(iso, "first arg must be CommandBuffer");
        return nullptr;
      }
      return cb;
    }

    // -------------------------------------------------------------------------
    // Implementations
    // -------------------------------------------------------------------------
    void p_drawLine(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // drawLine(cb, src[4], dst[4], color, thickness=0)
      math::vec4 src{}, dst{};
      if (info.Length() < 3 || !decode_vec4(info[1], src) || !decode_vec4(info[2], dst)) {
        (void)throw_type_error(iso, "drawLine: src/dst must be Float32Array(4)");
        return;
      }
      auto color = info.Length() >= 4 ? decode_color(iso, ctx, info[3]) : white;
      float thick = info.Length() >= 5 ? float(num(ctx, info[4])) : 0.0f;
      primitives::draw_line(*cb, src, dst, color, thick);
    }

    void p_fillTriangle(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::vec4 a{}, b{}, c{};
      if (info.Length() < 4 || !decode_vec4(info[1], a) || !decode_vec4(info[2], b) ||
          !decode_vec4(info[3], c)) {
        (void)throw_type_error(iso, "fillTriangle: a/b/c must be Float32Array(4)");
        return;
      }
      auto color = info.Length() >= 5 ? decode_color(iso, ctx, info[4]) : white;
      primitives::fill_triangle(*cb, a, b, c, color);
    }

    void p_fillRect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // fillRect(cb, [x,y], [w,h], depth?, color?)        — array form
      // fillRect(cb, x, y, w, h, depth?, color?)          — scalar form
      math::vec2 at{}, sz{};
      int next = 1;
      if (info.Length() >= 2 && info[1]->IsNumber()) {
        at.x = float(num(ctx, info[1]));
        at.y = float(num(ctx, info[2]));
        sz.x = float(num(ctx, info[3]));
        sz.y = float(num(ctx, info[4]));
        next = 5;
      } else if (info.Length() < 3 || !decode_vec2(info[1], at) || !decode_vec2(info[2], sz)) {
        (void)throw_type_error(iso,
                               "fillRect: expected (cb, [x,y], [w,h], …) or (cb, x, y, w, h, …)");
        return;
      } else {
        next = 3;
      }
      float d = info.Length() > next ? float(num(ctx, info[next])) : 0.0f;
      auto paint = info.Length() > next + 1 ? decode_paint(iso, ctx, info[next + 1])
                                            : primitives::paint_value::solid(white);
      primitives::fill_rect(*cb, at, sz, d, paint);
    }

    void p_drawRect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // drawRect(cb, [x,y], [w,h], depth?, color?, thickness?)        — array form
      // drawRect(cb, x, y, w, h, depth?, color?, thickness?)          — scalar form
      math::vec2 at{}, sz{};
      int next = 1;
      if (info.Length() >= 2 && info[1]->IsNumber()) {
        at.x = float(num(ctx, info[1]));
        at.y = float(num(ctx, info[2]));
        sz.x = float(num(ctx, info[3]));
        sz.y = float(num(ctx, info[4]));
        next = 5;
      } else if (info.Length() < 3 || !decode_vec2(info[1], at) || !decode_vec2(info[2], sz)) {
        (void)throw_type_error(iso,
                               "drawRect: expected (cb, [x,y], [w,h], …) or (cb, x, y, w, h, …)");
        return;
      } else {
        next = 3;
      }
      float d = info.Length() > next ? float(num(ctx, info[next])) : 0.0f;
      auto color = info.Length() > next + 1 ? decode_color(iso, ctx, info[next + 1]) : white;
      float thick = info.Length() > next + 2 ? float(num(ctx, info[next + 2])) : 0.0f;
      primitives::draw_rect(*cb, at, sz, d, color, thick);
    }

    // Mat4-only helpers for ellipse/box/cbox/pyramid/sphere/cylinder/quad-rounded.
    template <typename Fn> void mat_color(const FunctionCallbackInfo<Value>& info, Fn&& fn) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::mat4x4 m;
      if (info.Length() < 2 || !decode_mat4(info[1], m)) {
        (void)throw_type_error(iso, "expected mat4 Float32Array(16) at arg 2");
        return;
      }
      auto color = info.Length() >= 3 ? decode_color(iso, ctx, info[2]) : white;
      fn(*cb, m, color, info, ctx, iso);
    }

    void p_fillEllipse(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>& info, Local<Context> ctx, Isolate*) {
        float perc = info.Length() >= 4 ? float(num(ctx, info[3], 1.0)) : 1.0f;
        usize edges = info.Length() >= 5 ? usize(num(ctx, info[4], 64)) : 64;
        primitives::fill_ellipse(cb, m, c, perc, edges);
      });
    }
    void p_drawEllipse(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>& info, Local<Context> ctx, Isolate*) {
        float thick = info.Length() >= 4 ? float(num(ctx, info[3])) : 1.0f;
        float perc = info.Length() >= 5 ? float(num(ctx, info[4], 1.0)) : 1.0f;
        usize edges = info.Length() >= 6 ? usize(num(ctx, info[5], 64)) : 64;
        primitives::draw_ellipse(cb, m, c, thick, perc, edges);
      });
    }
    void p_fillBox(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>&, Local<Context>,
                         Isolate*) { primitives::fill_box(cb, m, c); });
    }
    void p_drawBox(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>& info, Local<Context> ctx, Isolate*) {
        float thick = info.Length() >= 4 ? float(num(ctx, info[3])) : 1.0f;
        primitives::draw_box(cb, m, c, thick);
      });
    }
    void p_fillCbox(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>&, Local<Context>,
                         Isolate*) { primitives::fill_cbox(cb, m, c); });
    }
    void p_drawCbox(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>& info, Local<Context> ctx, Isolate*) {
        float thick = info.Length() >= 4 ? float(num(ctx, info[3])) : 1.0f;
        primitives::draw_cbox(cb, m, c, thick);
      });
    }
    void p_fillSphere(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>& info, Local<Context> ctx, Isolate*) {
        float px = info.Length() >= 4 ? float(num(ctx, info[3], 1.0)) : 1.0f;
        float py = info.Length() >= 5 ? float(num(ctx, info[4], 1.0)) : 1.0f;
        usize edges = info.Length() >= 6 ? usize(num(ctx, info[5], 32)) : 32;
        primitives::fill_sphere(cb, m, c, px, py, edges);
      });
    }
    void p_fillCylinder(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::mat4x4 m;
      if (info.Length() < 2 || !decode_mat4(info[1], m))
        return;
      auto c = info.Length() >= 3 ? decode_color(iso, ctx, info[2]) : white;
      float perc = info.Length() >= 4 ? float(num(ctx, info[3], 1.0)) : 1.0f;
      usize edges = info.Length() >= 5 ? usize(num(ctx, info[4], 64)) : 64;
      primitives::fill_cylinder(*cb, m, primitives::color_list<2>{c, c}, perc, edges);
    }
    void p_fillPyramid(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>&, Local<Context>,
                         Isolate*) { primitives::fill_pyramid(cb, m, c); });
    }
    void p_drawPyramid(const FunctionCallbackInfo<Value>& info) {
      mat_color(info, [](command_buffer& cb, const math::mat4x4& m, r8g8b8a8 c,
                         const FunctionCallbackInfo<Value>& info, Local<Context> ctx, Isolate*) {
        float thick = info.Length() >= 4 ? float(num(ctx, info[3])) : 1.0f;
        primitives::draw_pyramid(cb, m, c, thick);
      });
    }
    void p_fillQuad(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // fillQuad(cb, transform_mat4, color)
      math::mat4x4 m;
      if (info.Length() < 2 || !decode_mat4(info[1], m))
        return;
      auto c = info.Length() >= 3 ? decode_color(iso, ctx, info[2]) : white;
      primitives::fill_quad(*cb, m, c);
    }
    void p_drawQuad(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::mat4x4 m;
      if (info.Length() < 2 || !decode_mat4(info[1], m))
        return;
      auto c = info.Length() >= 3 ? decode_color(iso, ctx, info[2]) : white;
      float thick = info.Length() >= 4 ? float(num(ctx, info[3])) : 1.0f;
      primitives::draw_quad(*cb, m, c, thick);
    }
    void p_fillQuadRounded(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // fillQuadRounded(cb, p1,p2,p3,p4 (vec4 each), rnd[4], color)
      math::vec4 p1, p2, p3, p4;
      if (info.Length() < 5 || !decode_vec4(info[1], p1) || !decode_vec4(info[2], p2) ||
          !decode_vec4(info[3], p3) || !decode_vec4(info[4], p4))
        return;
      primitives::optional_list<float, 4> rnd{0};
      if (info.Length() >= 6 && info[5]->IsFloat32Array()) {
        auto a = info[5].As<Float32Array>();
        float t[4]{};
        a->CopyContents(t, std::min<usize>(sizeof(t), a->ByteLength()));
        rnd[0] = t[0];
        rnd[1] = t[1];
        rnd[2] = t[2];
        rnd[3] = t[3];
      }
      auto c = info.Length() >= 7 ? decode_color(iso, ctx, info[6]) : white;
      primitives::color_list<4> cl{c, c, c, c};
      primitives::fill_quad_rounded(*cb, p1, p2, p3, p4, rnd, cl);
    }
    void p_drawQuadRounded(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::vec4 p1, p2, p3, p4;
      if (info.Length() < 5 || !decode_vec4(info[1], p1) || !decode_vec4(info[2], p2) ||
          !decode_vec4(info[3], p3) || !decode_vec4(info[4], p4))
        return;
      primitives::optional_list<float, 4> rnd{0};
      if (info.Length() >= 6 && info[5]->IsFloat32Array()) {
        auto a = info[5].As<Float32Array>();
        float t[4]{};
        a->CopyContents(t, std::min<usize>(sizeof(t), a->ByteLength()));
        rnd[0] = t[0];
        rnd[1] = t[1];
        rnd[2] = t[2];
        rnd[3] = t[3];
      }
      auto c = info.Length() >= 7 ? decode_color(iso, ctx, info[6]) : white;
      float thick = info.Length() >= 8 ? float(num(ctx, info[7])) : 1.0f;
      primitives::color_list<4> cl{c, c, c, c};
      primitives::draw_quad_rounded(*cb, p1, p2, p3, p4, rnd, cl, thick);
    }
    void p_fillRectRounded(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::mat4x4 m;
      if (info.Length() < 2 || !decode_mat4(info[1], m))
        return;
      primitives::optional_list<float, 4> rnd{0};
      if (info.Length() >= 3 && info[2]->IsFloat32Array()) {
        auto a = info[2].As<Float32Array>();
        float t[4]{};
        a->CopyContents(t, std::min<usize>(sizeof(t), a->ByteLength()));
        rnd[0] = t[0];
        rnd[1] = t[1];
        rnd[2] = t[2];
        rnd[3] = t[3];
      }
      float shift = info.Length() >= 4 ? float(num(ctx, info[3])) : 0.0f;
      auto paint = info.Length() >= 5 ? decode_paint(iso, ctx, info[4])
                                      : primitives::paint_value::solid(white);
      primitives::fill_rect_rounded(*cb, m, rnd, shift, paint);
    }
    void p_drawRectRounded(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::mat4x4 m;
      if (info.Length() < 2 || !decode_mat4(info[1], m))
        return;
      primitives::optional_list<float, 4> rnd{0};
      if (info.Length() >= 3 && info[2]->IsFloat32Array()) {
        auto a = info[2].As<Float32Array>();
        float t[4]{};
        a->CopyContents(t, std::min<usize>(sizeof(t), a->ByteLength()));
        rnd[0] = t[0];
        rnd[1] = t[1];
        rnd[2] = t[2];
        rnd[3] = t[3];
      }
      float shift = info.Length() >= 4 ? float(num(ctx, info[3])) : 0.0f;
      auto c = info.Length() >= 5 ? decode_color(iso, ctx, info[4]) : white;
      float thick = info.Length() >= 6 ? float(num(ctx, info[5])) : 1.0f;
      primitives::color_list<4> cl{c, c, c, c};
      primitives::draw_rect_rounded(*cb, m, rnd, shift, cl, thick);
    }
    void p_drawText(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // drawText(cb, [x,y], depth, text, opts?)              — array form
      //   opts: { color, size, pt }.
      // drawText(cb, x, y, depth, text, size?, color?)       — scalar form
      math::vec2 at{};
      int next = 1;
      if (info.Length() >= 2 && info[1]->IsNumber()) {
        at.x = float(num(ctx, info[1]));
        at.y = float(num(ctx, info[2]));
        next = 3;
      } else if (info.Length() < 4 || !decode_vec2(info[1], at)) {
        (void)throw_type_error(iso, "drawText: expected (cb, [x,y], depth, text, opts?) or "
                                    "(cb, x, y, depth, text, size?, color?)");
        return;
      } else {
        next = 2;
      }
      float d = float(num(ctx, info[next]));
      auto text = utf8(iso, info[next + 1]);
      u32 font_id = 0;
      primitives::text_style style{};
      // Trailing args: either an opts object (array form) or scalar (size, color).
      if (info.Length() > next + 2) {
        auto v = info[next + 2];
        if (v->IsObject() && !v->IsNumber()) {
          auto o = v.As<Object>();
          Local<Value> field;
          if (o->Get(ctx, "color"_v8(iso)).ToLocal(&field))
            style.color = decode_color(iso, ctx, field);
          if (o->Get(ctx, "size"_v8(iso)).ToLocal(&field) && field->IsNumber())
            style.pt = float(field->NumberValue(ctx).FromMaybe(16.0));
          if (o->Get(ctx, "pt"_v8(iso)).ToLocal(&field) && field->IsNumber())
            style.pt = static_cast<float>(
                field->NumberValue(ctx).FromMaybe(static_cast<double>(style.pt)));
          if (o->Get(ctx, "fontId"_v8(iso)).ToLocal(&field) && field->IsNumber())
            font_id = static_cast<u32>(field->NumberValue(ctx).FromMaybe(0.0));
          if (o->Get(ctx, "lineHeight"_v8(iso)).ToLocal(&field) && field->IsNumber())
            style.line_height = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
          // features: ["liga", "calt"] or [["ss01", 1], ...]
          if (o->Get(ctx, "features"_v8(iso)).ToLocal(&field) && field->IsArray()) {
            auto a = field.As<Array>();
            const u32 n = a->Length();
            for (u32 i = 0; i < n; ++i) {
              Local<Value> el;
              if (!a->Get(ctx, i).ToLocal(&el))
                continue;
              std::array<char, 4> tag{' ', ' ', ' ', ' '};
              u32 val = 1;
              if (el->IsString()) {
                String::Utf8Value u(iso, el);
                for (usize k = 0; k < 4 && k < static_cast<usize>(u.length()); ++k)
                  tag[k] = (*u)[k];
              } else if (el->IsArray()) {
                auto pair = el.As<Array>();
                Local<Value> tv;
                if (pair->Get(ctx, 0).ToLocal(&tv) && tv->IsString()) {
                  String::Utf8Value u(iso, tv);
                  for (usize k = 0; k < 4 && k < static_cast<usize>(u.length()); ++k)
                    tag[k] = (*u)[k];
                }
                Local<Value> vv;
                if (pair->Get(ctx, 1).ToLocal(&vv) && vv->IsNumber()) {
                  val = static_cast<u32>(vv->NumberValue(ctx).FromMaybe(1.0));
                }
              }
              style.features.emplace_back(tag, val);
            }
          }
          // variations: { wght: 600, wdth: 110 }
          if (o->Get(ctx, "variations"_v8(iso)).ToLocal(&field) && field->IsObject() &&
              !field->IsArray()) {
            auto vobj = field.As<Object>();
            Local<Array> keys;
            if (vobj->GetOwnPropertyNames(ctx).ToLocal(&keys)) {
              const u32 n = keys->Length();
              for (u32 i = 0; i < n; ++i) {
                Local<Value> kk;
                if (!keys->Get(ctx, i).ToLocal(&kk) || !kk->IsString())
                  continue;
                String::Utf8Value u(iso, kk);
                std::array<char, 4> tag{' ', ' ', ' ', ' '};
                for (usize k = 0; k < 4 && k < static_cast<usize>(u.length()); ++k)
                  tag[k] = (*u)[k];
                Local<Value> vv;
                if (!vobj->Get(ctx, kk).ToLocal(&vv) || !vv->IsNumber())
                  continue;
                style.variations.emplace_back(
                    tag, static_cast<float>(vv->NumberValue(ctx).FromMaybe(0.0)));
              }
            }
          }
        } else if (v->IsNumber()) {
          style.pt = float(num(ctx, v, 16.0));
          if (info.Length() > next + 3)
            style.color = decode_color(iso, ctx, info[next + 3]);
        }
      }
      const auto* font = resolve_font_id(font_id);
      auto out = primitives::draw_text(*cb, at, d, text, font ? *font : get_font_info(), style);
      auto arr = Array::New(iso, 4);
      (void)arr->Set(ctx, 0, Number::New(iso, static_cast<double>(out.x)));
      (void)arr->Set(ctx, 1, Number::New(iso, static_cast<double>(out.y)));
      (void)arr->Set(ctx, 2, Number::New(iso, static_cast<double>(out.z)));
      (void)arr->Set(ctx, 3, Number::New(iso, static_cast<double>(out.w)));
      info.GetReturnValue().Set(arr);
    }

    // drawTextRun(cb, runs: Array<{ x, y, text, size?, color?, depth? }>)
    //
    // Batched draw_text — collapses N V8↔C++ trampolines per frame into one.
    // The text-painter in fxe-ui issues one drawText per wrapped line per
    // Text component per frame; in stress scenes that is ~2k calls/frame and
    // the boundary cost (HandleScope setup, arg validation, utf8 decode
    // dispatch) dominates the actual shaping work. This entry takes the
    // entire run set, decodes each entry once, and drives primitives::draw_text
    // back-to-back against the same CommandBuffer.
    //
    // Trade-off vs. the scalar drawText: this path does *not* accept
    // OpenType features / variations / fontId. Callers needing those (rare)
    // should keep using drawText().
    void p_drawTextRun(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      if (info.Length() < 2 || !info[1]->IsArray()) {
        (void)throw_type_error(iso, "drawTextRun: expected (cb, runs[])");
        return;
      }
      auto runs = info[1].As<Array>();
      const u32 n = runs->Length();
      const auto& default_font = get_font_info();
      // Reused property keys.
      auto k_x = "x"_v8(iso);
      auto k_y = "y"_v8(iso);
      auto k_text = "text"_v8(iso);
      auto k_size = "size"_v8(iso);
      auto k_color = "color"_v8(iso);
      auto k_depth = "depth"_v8(iso);
      for (u32 i = 0; i < n; ++i) {
        Local<Value> ev;
        if (!runs->Get(ctx, i).ToLocal(&ev) || !ev->IsObject())
          continue;
        auto o = ev.As<Object>();
        Local<Value> field;
        math::vec2 at{0, 0};
        if (o->Get(ctx, k_x).ToLocal(&field) && field->IsNumber())
          at.x = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
        if (o->Get(ctx, k_y).ToLocal(&field) && field->IsNumber())
          at.y = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
        float depth = 0.0f;
        if (o->Get(ctx, k_depth).ToLocal(&field) && field->IsNumber())
          depth = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
        std::string text;
        if (o->Get(ctx, k_text).ToLocal(&field) && !field->IsUndefined()) {
          text = utf8(iso, field);
        }
        primitives::text_style style{};
        if (o->Get(ctx, k_size).ToLocal(&field) && field->IsNumber())
          style.pt = static_cast<float>(field->NumberValue(ctx).FromMaybe(16.0));
        if (o->Get(ctx, k_color).ToLocal(&field) && !field->IsUndefined())
          style.color = decode_color(iso, ctx, field);
        (void)primitives::draw_text(*cb, at, depth, text, default_font, style);
      }
    }

    // Decode a text_style options object as used by drawText.
    void decode_text_style_opts(Isolate* iso, Local<Context> ctx, Local<Value> v,
                                primitives::text_style& style, u32* out_font_id) {
      if (!v->IsObject() || v->IsNumber())
        return;
      auto o = v.As<Object>();
      Local<Value> field;
      if (o->Get(ctx, "color"_v8(iso)).ToLocal(&field) && !field->IsUndefined())
        style.color = decode_color(iso, ctx, field);
      if (o->Get(ctx, "size"_v8(iso)).ToLocal(&field) && field->IsNumber())
        style.pt = static_cast<float>(field->NumberValue(ctx).FromMaybe(16.0));
      if (o->Get(ctx, "pt"_v8(iso)).ToLocal(&field) && field->IsNumber())
        style.pt = static_cast<float>(field->NumberValue(ctx).FromMaybe(16.0));
      if (out_font_id && o->Get(ctx, "fontId"_v8(iso)).ToLocal(&field) && field->IsNumber())
        *out_font_id = static_cast<u32>(field->NumberValue(ctx).FromMaybe(0.0));
      if (o->Get(ctx, "lineHeight"_v8(iso)).ToLocal(&field) && field->IsNumber())
        style.line_height = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
      if (o->Get(ctx, "tabSize"_v8(iso)).ToLocal(&field) && field->IsNumber())
        style.tab_size = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
      if (o->Get(ctx, "tabOriginX"_v8(iso)).ToLocal(&field) && field->IsNumber())
        style.tab_origin_x = static_cast<float>(field->NumberValue(ctx).FromMaybe(0.0));
      if (o->Get(ctx, "showWhitespace"_v8(iso)).ToLocal(&field) && field->IsBoolean()) {
        style.whitespace = field->BooleanValue(iso) ? primitives::whitespace_glyphs::visible
                                                    : primitives::whitespace_glyphs::none;
      }
      if (o->Get(ctx, "bold"_v8(iso)).ToLocal(&field) && field->BooleanValue(iso))
        style.flags |= primitives::text_bold;
      if (o->Get(ctx, "italic"_v8(iso)).ToLocal(&field) && field->BooleanValue(iso))
        style.flags |= primitives::text_italic;
      if (o->Get(ctx, "features"_v8(iso)).ToLocal(&field) && field->IsArray()) {
        auto a = field.As<Array>();
        const u32 n = a->Length();
        for (u32 i = 0; i < n; ++i) {
          Local<Value> el;
          if (!a->Get(ctx, i).ToLocal(&el))
            continue;
          std::array<char, 4> tag{' ', ' ', ' ', ' '};
          u32 val = 1;
          if (el->IsString()) {
            String::Utf8Value u(iso, el);
            for (usize k = 0; k < 4 && k < static_cast<usize>(u.length()); ++k)
              tag[k] = (*u)[k];
          } else if (el->IsArray()) {
            auto pair = el.As<Array>();
            Local<Value> tv;
            if (pair->Get(ctx, 0).ToLocal(&tv) && tv->IsString()) {
              String::Utf8Value u(iso, tv);
              for (usize k = 0; k < 4 && k < static_cast<usize>(u.length()); ++k)
                tag[k] = (*u)[k];
            }
            Local<Value> vv;
            if (pair->Get(ctx, 1).ToLocal(&vv) && vv->IsNumber())
              val = static_cast<u32>(vv->NumberValue(ctx).FromMaybe(1.0));
          }
          style.features.emplace_back(tag, val);
        }
      }
    }

    // drawTextSpans(cb, x, y, depth, spans[, opts])
    //  spans: Array<{ text, color?, size?, fontId?, bold?, italic?, underline?,
    //                 strikethrough?, features? }>
    //  opts:  { tabSize?, tabOriginX?, lineHeight?, showWhitespace? }
    // Returns [width, height, advanceX, glyphCount].
    void p_drawTextSpans(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      if (info.Length() < 5 || !info[4]->IsArray()) {
        (void)throw_type_error(iso, "drawTextSpans: expected (cb, x, y, depth, spans[], opts?)");
        return;
      }
      const float x = static_cast<float>(num(ctx, info[1]));
      const float y = static_cast<float>(num(ctx, info[2]));
      const float depth = static_cast<float>(num(ctx, info[3]));
      auto spans_arr = info[4].As<Array>();
      const u32 n = spans_arr->Length();
      primitives::text_style common{};
      u32 common_font_id = 0;
      if (info.Length() >= 6)
        decode_text_style_opts(iso, ctx, info[5], common, &common_font_id);
      auto k_text = "text"_v8(iso);
      auto k_underline = "underline"_v8(iso);
      auto k_strikethrough = "strikethrough"_v8(iso);
      std::vector<std::string> texts;
      texts.reserve(n);
      std::vector<primitives::text_span> spans;
      spans.reserve(n);
      for (u32 i = 0; i < n; ++i) {
        Local<Value> ev;
        if (!spans_arr->Get(ctx, i).ToLocal(&ev) || !ev->IsObject())
          continue;
        auto o = ev.As<Object>();
        Local<Value> field;
        std::string text;
        if (o->Get(ctx, k_text).ToLocal(&field) && !field->IsUndefined())
          text = utf8(iso, field);
        if (text.empty())
          continue;
        primitives::text_span sp;
        sp.style = common;
        u32 font_id = common_font_id;
        decode_text_style_opts(iso, ctx, ev, sp.style, &font_id);
        if (sp.style.pt <= 0.0f)
          sp.style.pt = 16.0f;
        if (font_id != 0) {
          if (const auto* f = resolve_font_id(font_id))
            sp.font = f;
        }
        if (o->Get(ctx, k_underline).ToLocal(&field))
          sp.underline = field->BooleanValue(iso);
        if (o->Get(ctx, k_strikethrough).ToLocal(&field))
          sp.strikethrough = field->BooleanValue(iso);
        texts.emplace_back(std::move(text));
        sp.text = texts.back();
        spans.push_back(sp);
      }
      const auto* fb_font = common_font_id != 0 ? resolve_font_id(common_font_id) : nullptr;
      auto out = primitives::draw_text_spans(*cb, math::vec2{x, y}, depth, spans,
                                             fb_font ? *fb_font : get_font_info());
      auto arr = Array::New(iso, 4);
      (void)arr->Set(ctx, 0, Number::New(iso, static_cast<double>(out.x)));
      (void)arr->Set(ctx, 1, Number::New(iso, static_cast<double>(out.y)));
      (void)arr->Set(ctx, 2, Number::New(iso, static_cast<double>(out.z)));
      (void)arr->Set(ctx, 3, Number::New(iso, static_cast<double>(out.w)));
      info.GetReturnValue().Set(arr);
    }

    // drawSelectionRects(cb, rects: Float32Array (4N: [x,y,w,h, ...]), color?, depth?)
    void p_drawSelectionRects(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      if (info.Length() < 2 || !info[1]->IsFloat32Array()) {
        (void)throw_type_error(iso,
                               "drawSelectionRects: expected (cb, Float32Array, color?, depth?)");
        return;
      }
      auto a = info[1].As<Float32Array>();
      const u32 n = static_cast<u32>(a->Length()) / 4u;
      if (n == 0)
        return;
      std::vector<float> raw(a->Length());
      a->CopyContents(raw.data(), raw.size() * sizeof(float));
      std::vector<math::vec4> rects;
      rects.reserve(n);
      for (u32 i = 0; i < n; ++i)
        rects.emplace_back(raw[i * 4 + 0], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]);
      const auto color = info.Length() >= 3 ? decode_color(iso, ctx, info[2]) : white;
      const float depth = info.Length() >= 4 ? static_cast<float>(num(ctx, info[3])) : 0.0f;
      primitives::draw_selection_rects(*cb, std::span<const math::vec4>{rects}, color, depth);
    }

    // drawDecorationUnderline(cb, x1, x2, y, style, color?, thickness?, depth?)
    void p_drawDecorationUnderline(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      if (info.Length() < 5) {
        (void)throw_type_error(iso, "drawDecorationUnderline: expected (cb, x1, x2, y, style, "
                                    "color?, thickness?, depth?)");
        return;
      }
      const float x1 = static_cast<float>(num(ctx, info[1]));
      const float x2 = static_cast<float>(num(ctx, info[2]));
      const float y = static_cast<float>(num(ctx, info[3]));
      primitives::decoration_style style = primitives::decoration_style::solid;
      if (info[4]->IsString()) {
        auto s = info[4].As<String>();
        if (s == "dashed"_v8)
          style = primitives::decoration_style::dashed;
        else if (s == "dotted"_v8)
          style = primitives::decoration_style::dotted;
        else if (s == "wavy"_v8)
          style = primitives::decoration_style::wavy;
      }
      const auto color = info.Length() >= 6 ? decode_color(iso, ctx, info[5]) : white;
      const float thickness =
          info.Length() >= 7 ? static_cast<float>(num(ctx, info[6], 1.0)) : 1.0f;
      const float depth = info.Length() >= 8 ? static_cast<float>(num(ctx, info[7])) : 0.0f;
      primitives::draw_decoration_underline(*cb, x1, x2, y, style, color, thickness, depth);
    }

    // drawTextureQuad(cb, slot, x, y, w, h, [u0, v0, u1, v1], [tint:Color],
    //                [depth:number])
    //
    // Emits a textured quad sampling from user-texture slot `slot` (0..3),
    // bound on the renderer via `Renderer.bindUserTexture(slot, view)`.
    // UV defaults to the full [0,0]..[1,1] rect; tint defaults to white.
    //
    // Surface-cache flow:
    //   const off = new OffscreenRenderer({width:W, height:H});
    //   /* render subtree into off */
    //   off.endFrame();
    //   renderer.bindUserTexture(0, off);
    //   Primitives.drawTextureQuad(cb, 0, x, y, W, H);
    void p_drawTextureQuad(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      if (info.Length() < 6) {
        (void)throw_type_error(
            iso, "drawTextureQuad: expected (cb, slot, x, y, w, h, [uv], [tint], [depth])");
        return;
      }
      const u32 slot = static_cast<u32>(info[1]->Uint32Value(ctx).FromMaybe(0));
      if (slot >= 4) {
        (void)throw_range_error(iso, "drawTextureQuad: slot out of range (0..3)");
        return;
      }
      const float x = static_cast<float>(num(ctx, info[2]));
      const float y = static_cast<float>(num(ctx, info[3]));
      const float w = static_cast<float>(num(ctx, info[4]));
      const float h = static_cast<float>(num(ctx, info[5]));
      float u0 = 0.0f, v0_ = 0.0f, u1 = 1.0f, v1_ = 1.0f;
      if (info.Length() >= 7 && info[6]->IsArray()) {
        auto a = info[6].As<Array>();
        if (a->Length() >= 4) {
          Local<Value> e;
          if (a->Get(ctx, 0).ToLocal(&e) && e->IsNumber())
            u0 = static_cast<float>(e->NumberValue(ctx).FromMaybe(0.0));
          if (a->Get(ctx, 1).ToLocal(&e) && e->IsNumber())
            v0_ = static_cast<float>(e->NumberValue(ctx).FromMaybe(0.0));
          if (a->Get(ctx, 2).ToLocal(&e) && e->IsNumber())
            u1 = static_cast<float>(e->NumberValue(ctx).FromMaybe(1.0));
          if (a->Get(ctx, 3).ToLocal(&e) && e->IsNumber())
            v1_ = static_cast<float>(e->NumberValue(ctx).FromMaybe(1.0));
        }
      }
      r8g8b8a8 tint{255, 255, 255, 255};
      if (info.Length() >= 8 && !info[7]->IsUndefined())
        tint = decode_color(iso, ctx, info[7]);
      const float depth = info.Length() >= 9 ? static_cast<float>(num(ctx, info[8])) : 0.0f;
      const texture_id tid = user_tex_flag | (slot & user_tex_slot_mask);
      // Triangle strip: TL, TR, BL, BR.
      auto* vp = cb->allocate_strip(4, vertex_topology::triangle);
      vp[0] = make_vertex({x, y}, depth, {u0, v0_}, tid, tint);
      vp[1] = make_vertex({x + w, y}, depth, {u1, v0_}, tid, tint);
      vp[2] = make_vertex({x, y + h}, depth, {u0, v1_}, tid, tint);
      vp[3] = make_vertex({x + w, y + h}, depth, {u1, v1_}, tid, tint);
    }

    void path_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Path: use new");
        return;
      }
      auto* h = new path_holder();
      auto self = info.This();
      set_native(iso, self, h, TAG_PATH);
      h->self.Reset(iso, self);
      h->self.SetWeak(h, path_finalizer, WeakCallbackType::kParameter);
    }

    template <typename Fn> void path_method(const FunctionCallbackInfo<Value>& info, Fn&& fn) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = static_cast<path_holder*>(unwrap(info.This(), TAG_PATH));
      if (!h) {
        (void)throw_type_error(iso, "invalid Path");
        return;
      }
      fn(h->path, ctx);
      info.GetReturnValue().Set(info.This());
    }

    void path_moveTo(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context> ctx) {
        p.move_to(float(num(ctx, info[0])), float(num(ctx, info[1])));
      });
    }
    void path_lineTo(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context> ctx) {
        p.line_to(float(num(ctx, info[0])), float(num(ctx, info[1])));
      });
    }
    void path_quadTo(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context> ctx) {
        p.quad_to(float(num(ctx, info[0])), float(num(ctx, info[1])), float(num(ctx, info[2])),
                  float(num(ctx, info[3])));
      });
    }
    void path_cubicTo(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context> ctx) {
        p.cubic_to(float(num(ctx, info[0])), float(num(ctx, info[1])), float(num(ctx, info[2])),
                   float(num(ctx, info[3])), float(num(ctx, info[4])), float(num(ctx, info[5])));
      });
    }
    void path_arc(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context> ctx) {
        p.arc(float(num(ctx, info[0])), float(num(ctx, info[1])), float(num(ctx, info[2])),
              float(num(ctx, info[3])), float(num(ctx, info[4])),
              info.Length() >= 6 && info[5]->BooleanValue(info.GetIsolate()));
      });
    }
    void path_close(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context>) { p.close(); });
    }
    void path_reset(const FunctionCallbackInfo<Value>& info) {
      path_method(info, [&](primitives::path_2d& p, Local<Context>) { p.reset(); });
    }

    Local<Object> make_paint(Isolate* iso, Local<Context> ctx, primitives::paint_kind kind,
                             math::vec4 p0, math::vec4 p1, Local<Value> stops_v) {
      auto o = Object::New(iso);
      (void)o->Set(ctx, "__fxePaint"_v8(iso), Integer::NewFromUnsigned(iso, PAINT_KIND_PROP));
      (void)o->Set(ctx, "kind"_v8(iso), Integer::NewFromUnsigned(iso, static_cast<u32>(kind)));
      auto a0 = Array::New(iso, 4);
      auto a1 = Array::New(iso, 4);
      for (int i = 0; i < 4; ++i) {
        (void)a0->Set(ctx, static_cast<u32>(i), Number::New(iso, static_cast<double>(p0[i])));
        (void)a1->Set(ctx, static_cast<u32>(i), Number::New(iso, static_cast<double>(p1[i])));
      }
      (void)o->Set(ctx, "p0"_v8(iso), a0);
      (void)o->Set(ctx, "p1"_v8(iso), a1);
      (void)o->Set(ctx, "stops"_v8(iso), stops_v);
      return o;
    }

    void p_linearGradient(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      math::vec2 a{}, b{};
      if (info.Length() < 3 || !decode_vec2(info[0], a) || !decode_vec2(info[1], b) ||
          !info[2]->IsFloat32Array()) {
        (void)throw_type_error(iso, "linearGradient: expected (p0, p1, stops)");
        return;
      }
      info.GetReturnValue().Set(make_paint(iso, ctx, primitives::paint_kind::linear,
                                           {a.x, a.y, 0, 0}, {b.x, b.y, 0, 0}, info[2]));
    }
    void p_radialGradient(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      math::vec2 c{};
      if (info.Length() < 3 || !decode_vec2(info[0], c) || !info[2]->IsFloat32Array()) {
        (void)throw_type_error(iso, "radialGradient: expected (center, radius, stops)");
        return;
      }
      info.GetReturnValue().Set(make_paint(iso, ctx, primitives::paint_kind::radial,
                                           {c.x, c.y, float(num(ctx, info[1], 1.0)), 0}, {},
                                           info[2]));
    }
    void p_conicGradient(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      math::vec2 c{};
      if (info.Length() < 3 || !decode_vec2(info[0], c) || !info[2]->IsFloat32Array()) {
        (void)throw_type_error(iso, "conicGradient: expected (center, angle, stops)");
        return;
      }
      info.GetReturnValue().Set(make_paint(iso, ctx, primitives::paint_kind::conic,
                                           {c.x, c.y, float(num(ctx, info[1])), 0}, {}, info[2]));
    }

    void p_fillPath(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      auto* p = info.Length() >= 2 ? unwrap_path(info[1]) : nullptr;
      if (!p) {
        (void)throw_type_error(
            iso, "fillPath: expected (CommandBuffer, Path, paint?, fillRule?, depth?)");
        return;
      }
      auto paint = info.Length() >= 3 ? decode_paint(iso, ctx, info[2])
                                      : primitives::paint_value::solid(white);
      primitives::fill_rule rule = primitives::fill_rule::nonzero;
      if (info.Length() >= 4 && info[3]->IsString() && info[3].As<String>() == "evenodd"_v8)
        rule = primitives::fill_rule::evenodd;
      float depth = info.Length() >= 5 ? float(num(ctx, info[4])) : 0.0f;
      primitives::fill_path(*cb, p->path, paint, rule, depth);
    }

    void p_strokePath(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      auto* p = info.Length() >= 2 ? unwrap_path(info[1]) : nullptr;
      if (!p) {
        (void)throw_type_error(iso, "strokePath: expected (CommandBuffer, Path, paint?, "
                                    "lineWidth?, join?, cap?, depth?)");
        return;
      }
      auto paint = info.Length() >= 3 ? decode_paint(iso, ctx, info[2])
                                      : primitives::paint_value::solid(white);
      float width = info.Length() >= 4 ? float(num(ctx, info[3], 1.0)) : 1.0f;
      float depth = info.Length() >= 7 ? float(num(ctx, info[6])) : 0.0f;
      primitives::stroke_path(*cb, p->path, paint, width, primitives::line_join::miter,
                              primitives::line_cap::butt, depth);
    }

    struct wrapped_text_native {
      std::vector<std::string> lines;
      std::vector<u32> starts;
      float width = 0.0f;
      float height = 0.0f;
      float line_height = 0.0f;
    };

    bool is_ascii(std::string_view text) {
      return std::all_of(text.begin(), text.end(),
                         [](unsigned char c) { return c <= static_cast<unsigned char>(0x7f); });
    }

    bool is_ascii_space(char c) {
      return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
    }

    float measured_text_width(std::string_view text, float pt, float letter_spacing) {
      if (text.empty())
        return 0.0f;
      auto v = primitives::calc_text(std::string(text), get_font_info(), pt);
      return v.x + std::max(0.0f, static_cast<float>(text.size() - 1) * letter_spacing);
    }

    void push_wrapped_line(wrapped_text_native& out, std::string line, u32 start, float pt,
                           float letter_spacing) {
      out.width = std::max(out.width, measured_text_width(line, pt, letter_spacing));
      out.starts.push_back(start);
      out.lines.push_back(std::move(line));
    }

    std::vector<std::string> break_long_word_native(std::string_view word, float pt,
                                                    float letter_spacing, float limit) {
      std::vector<std::string> out;
      std::string buffer;
      for (char ch : word) {
        std::string next = buffer;
        next.push_back(ch);
        if (measured_text_width(next, pt, letter_spacing) <= limit + 0.5f || buffer.empty()) {
          buffer = std::move(next);
        } else {
          out.push_back(std::move(buffer));
          buffer.assign(1, ch);
        }
      }
      if (!buffer.empty())
        out.push_back(std::move(buffer));
      return out;
    }

    wrapped_text_native wrap_text_native(std::string_view text, float pt, float letter_spacing,
                                         float max_width, float line_height, bool break_words) {
      wrapped_text_native out;
      const bool constrained = std::isfinite(max_width) && max_width > 0.0f;
      const float limit = constrained ? max_width : std::numeric_limits<float>::infinity();
      const std::string probe = text.empty() ? std::string("M") : std::string(text);
      const auto base = primitives::calc_text(probe, get_font_info(), pt);
      out.line_height = std::isnan(line_height) ? base.y : line_height;
      if (text.empty()) {
        push_wrapped_line(out, "", 0, pt, letter_spacing);
        out.height = out.line_height;
        return out;
      }

      usize paragraph_start = 0;
      while (paragraph_start <= text.size()) {
        usize paragraph_end = paragraph_start;
        while (paragraph_end < text.size() && text[paragraph_end] != '\n')
          ++paragraph_end;
        const std::string_view paragraph =
            text.substr(paragraph_start, paragraph_end - paragraph_start);
        if (paragraph.empty()) {
          push_wrapped_line(out, "", static_cast<u32>(paragraph_start), pt, letter_spacing);
        } else if (!constrained) {
          push_wrapped_line(out, std::string(paragraph), static_cast<u32>(paragraph_start), pt,
                            letter_spacing);
        } else {
          std::string current;
          usize current_start = paragraph_start;
          usize i = 0;
          while (i < paragraph.size()) {
            while (i < paragraph.size() && is_ascii_space(paragraph[i]))
              ++i;
            if (i >= paragraph.size())
              break;
            const usize word_begin = i;
            while (i < paragraph.size() && !is_ascii_space(paragraph[i]))
              ++i;
            std::string word(paragraph.substr(word_begin, i - word_begin));
            const usize word_start = paragraph_start + word_begin;
            const std::string candidate = current.empty() ? word : current + " " + word;
            if (measured_text_width(candidate, pt, letter_spacing) <= limit + 0.5f) {
              if (current.empty())
                current_start = word_start;
              current = candidate;
              continue;
            }
            if (!current.empty()) {
              push_wrapped_line(out, current, static_cast<u32>(current_start), pt, letter_spacing);
              current.clear();
            }
            if (break_words && measured_text_width(word, pt, letter_spacing) > limit + 0.5f &&
                word.size() > 1) {
              auto pieces = break_long_word_native(word, pt, letter_spacing, limit);
              usize piece_start = word_start;
              for (usize p = 0; p + 1 < pieces.size(); ++p) {
                push_wrapped_line(out, pieces[p], static_cast<u32>(piece_start), pt,
                                  letter_spacing);
                piece_start += pieces[p].size();
              }
              word = pieces.empty() ? std::string{} : pieces.back();
              current_start = piece_start;
            } else {
              current_start = word_start;
            }
            current = std::move(word);
          }
          if (!current.empty()) {
            push_wrapped_line(out, current, static_cast<u32>(current_start), pt, letter_spacing);
          } else if (out.lines.empty()) {
            push_wrapped_line(out, "", static_cast<u32>(paragraph_start), pt, letter_spacing);
          }
        }
        if (paragraph_end == text.size())
          break;
        paragraph_start = paragraph_end + 1;
      }
      if (out.lines.empty())
        push_wrapped_line(out, "", 0, pt, letter_spacing);
      out.height = out.line_height * static_cast<float>(out.lines.size());
      return out;
    }

    Local<Object> wrapped_text_to_object(Isolate* iso, Local<Context> ctx,
                                         const wrapped_text_native& wrapped) {
      auto obj = Object::New(iso);
      auto lines = Array::New(iso, static_cast<int>(wrapped.lines.size()));
      for (usize i = 0; i < wrapped.lines.size(); ++i) {
        (void)lines->Set(ctx, static_cast<u32>(i),
                         String::NewFromUtf8(iso, wrapped.lines[i].c_str(), NewStringType::kNormal,
                                             static_cast<int>(wrapped.lines[i].size()))
                             .ToLocalChecked());
      }
      auto starts = Array::New(iso, static_cast<int>(wrapped.starts.size()));
      for (usize i = 0; i < wrapped.starts.size(); ++i) {
        (void)starts->Set(ctx, static_cast<u32>(i),
                          Integer::NewFromUnsigned(iso, wrapped.starts[i]));
      }
      (void)obj->Set(ctx, "lines"_v8(iso), lines);
      (void)obj->Set(ctx, "width"_v8(iso), Number::New(iso, static_cast<double>(wrapped.width)));
      (void)obj->Set(ctx, "height"_v8(iso), Number::New(iso, static_cast<double>(wrapped.height)));
      (void)obj->Set(ctx, "lineHeight"_v8(iso),
                     Number::New(iso, static_cast<double>(wrapped.line_height)));
      (void)obj->Set(ctx, "lineStartIndices"_v8(iso), starts);
      return obj;
    }

    void p_wrapTextNative(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      const auto text = info.Length() >= 1 ? utf8(iso, info[0]) : std::string{};
      if (!is_ascii(text)) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      const float pt = info.Length() >= 2 ? float(num(ctx, info[1], 16.0)) : 16.0f;
      const float letter_spacing = info.Length() >= 3 ? float(num(ctx, info[2], 0.0)) : 0.0f;
      const float max_width =
          info.Length() >= 4 ? float(num(ctx, info[3], std::numeric_limits<double>::infinity()))
                             : std::numeric_limits<float>::infinity();
      const float line_height = info.Length() >= 5 && info[4]->IsNumber()
                                    ? float(num(ctx, info[4]))
                                    : std::numeric_limits<float>::quiet_NaN();
      const bool break_words = info.Length() >= 6 && info[5]->BooleanValue(iso);
      const auto wrapped =
          wrap_text_native(text, pt, letter_spacing, max_width, line_height, break_words);
      info.GetReturnValue().Set(wrapped_text_to_object(iso, ctx, wrapped));
    }

    void p_xAtGlyphIndexNative(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      const auto text = info.Length() >= 1 ? utf8(iso, info[0]) : std::string{};
      if (!is_ascii(text)) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      const float pt = info.Length() >= 2 ? float(num(ctx, info[1], 16.0)) : 16.0f;
      const float letter_spacing = info.Length() >= 3 ? float(num(ctx, info[2], 0.0)) : 0.0f;
      const double idx_value = info.Length() >= 4 ? num(ctx, info[3], 0.0) : 0.0;
      const auto clamped = static_cast<usize>(
          std::max(0.0, std::min(std::trunc(idx_value), static_cast<double>(text.size()))));
      const float x =
          measured_text_width(std::string_view(text).substr(0, clamped), pt, letter_spacing);
      info.GetReturnValue().Set(Number::New(iso, static_cast<double>(x)));
    }

    void p_glyphIndexAtNative(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      const auto text = info.Length() >= 1 ? utf8(iso, info[0]) : std::string{};
      if (!is_ascii(text)) {
        info.GetReturnValue().Set(Null(iso));
        return;
      }
      const float pt = info.Length() >= 2 ? float(num(ctx, info[1], 16.0)) : 16.0f;
      const float letter_spacing = info.Length() >= 3 ? float(num(ctx, info[2], 0.0)) : 0.0f;
      const double x = info.Length() >= 4 ? num(ctx, info[3], 0.0) : 0.0;
      if (text.empty() || x <= 0.0) {
        info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, 0));
        return;
      }
      if (!std::isfinite(x)) {
        info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, static_cast<u32>(text.size())));
        return;
      }
      usize lo = 0;
      usize hi = text.size();
      while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        const float left =
            measured_text_width(std::string_view(text).substr(0, mid), pt, letter_spacing);
        const float right =
            measured_text_width(std::string_view(text).substr(0, mid + 1), pt, letter_spacing);
        const double boundary = (static_cast<double>(left) + static_cast<double>(right)) / 2.0;
        if (x < boundary)
          hi = mid;
        else
          lo = mid + 1;
      }
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, static_cast<u32>(lo)));
    }

    void p_calcText(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto text = info.Length() >= 1 ? utf8(iso, info[0]) : std::string{};
      float pt = info.Length() >= 2 ? float(num(ctx, info[1], 16.0)) : 16.0f;
      auto v = primitives::calc_text(text, get_font_info(), pt);
      auto arr = Array::New(iso, 2);
      (void)arr->Set(ctx, 0, Number::New(iso, static_cast<double>(v.x)));
      (void)arr->Set(ctx, 1, Number::New(iso, static_cast<double>(v.y)));
      info.GetReturnValue().Set(arr);
    }
    void p_drain(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 3) {
        (void)throw_type_error(iso, "drain(cmdBuffer, opcodes Uint32Array, params Float32Array)");
        return;
      }
      auto* cb = unwrap_any_cb(info[0]);
      if (!cb) {
        (void)throw_type_error(iso, "drain: first arg must be CommandBuffer");
        return;
      }
      if (!info[1]->IsUint32Array() || !info[2]->IsFloat32Array()) {
        (void)throw_type_error(iso, "drain: expected Uint32Array opcodes and Float32Array params");
        return;
      }

      auto opcodes = info[1].As<Uint32Array>();
      auto params = info[2].As<Float32Array>();
      std::vector<u32> ops(opcodes->Length());
      std::vector<float> p(params->Length());
      opcodes->CopyContents(ops.data(), ops.size() * sizeof(u32));
      params->CopyContents(p.data(), p.size() * sizeof(float));

      usize pos = 0;
      u32 executed = 0;
      for (usize i = 0; i < ops.size(); ++i) {
        switch (ops[i]) {
        case OP_FILL_RECT: {
          if (!require_params(iso, pos, 9, p.size(), "fillRect"))
            return;
          primitives::fill_rect(*cb, math::vec2{p[pos], p[pos + 1]},
                                math::vec2{p[pos + 2], p[pos + 3]}, p[pos + 4],
                                color_from_floats(&p[pos + 5]));
          pos += 9;
          break;
        }
        case OP_DRAW_RECT: {
          if (!require_params(iso, pos, 10, p.size(), "drawRect"))
            return;
          primitives::draw_rect(*cb, math::vec2{p[pos], p[pos + 1]},
                                math::vec2{p[pos + 2], p[pos + 3]}, p[pos + 4],
                                color_from_floats(&p[pos + 5]), p[pos + 9]);
          pos += 10;
          break;
        }
        case OP_FILL_TRIANGLE: {
          if (!require_params(iso, pos, 16, p.size(), "fillTriangle"))
            return;
          primitives::fill_triangle(*cb, math::vec4{p[pos], p[pos + 1], p[pos + 2], p[pos + 3]},
                                    math::vec4{p[pos + 4], p[pos + 5], p[pos + 6], p[pos + 7]},
                                    math::vec4{p[pos + 8], p[pos + 9], p[pos + 10], p[pos + 11]},
                                    color_from_floats(&p[pos + 12]));
          pos += 16;
          break;
        }
        case OP_DRAW_LINE: {
          if (!require_params(iso, pos, 13, p.size(), "drawLine"))
            return;
          primitives::draw_line(*cb, math::vec4{p[pos], p[pos + 1], p[pos + 2], p[pos + 3]},
                                math::vec4{p[pos + 4], p[pos + 5], p[pos + 6], p[pos + 7]},
                                color_from_floats(&p[pos + 8]), p[pos + 12]);
          pos += 13;
          break;
        }
        case OP_DRAW_TEXT: {
          if (!require_params(iso, pos, 9, p.size(), "drawText"))
            return;
          float x = p[pos];
          float y = p[pos + 1];
          float d = p[pos + 2];
          primitives::text_style style{};
          style.pt = p[pos + 3];
          style.color = color_from_floats(&p[pos + 4]);
          auto char_count = static_cast<u32>(p[pos + 8]);
          pos += 9;
          if (!require_params(iso, pos, char_count, p.size(), "drawText text"))
            return;
          std::string text;
          text.reserve(char_count);
          for (u32 n = 0; n < char_count; ++n) {
            auto c = static_cast<unsigned>(p[pos + n]);
            if (c > 0x7f) {
              (void)throw_range_error(iso,
                                      "Primitives.drain drawText supports ASCII codepoints only");
              return;
            }
            text.push_back(static_cast<char>(c));
          }
          pos += char_count;
          (void)primitives::draw_text(*cb, math::vec2{x, y}, d, text, get_font_info(), style);
          break;
        }
        case OP_FILL_PATH:
        case OP_STROKE_PATH: {
          if (!require_params(iso, pos, 8, p.size(),
                              ops[i] == OP_FILL_PATH ? "fillPath" : "strokePath"))
            return;
          const u32 command_count = static_cast<u32>(p[pos]);
          primitives::paint_value paint =
              primitives::paint_value::solid(color_from_floats(&p[pos + 1]));
          const float depth = p[pos + 5];
          const float line_width = p[pos + 6];
          pos += 8; // slot 7 reserved for future fillRule/join/cap packing.
          primitives::path_2d path;
          for (u32 c = 0; c < command_count; ++c) {
            if (!require_params(iso, pos, 1, p.size(), "path command"))
              return;
            const u32 cmd = static_cast<u32>(p[pos++]);
            switch (cmd) {
            case 0:
              if (!require_params(iso, pos, 2, p.size(), "path move"))
                return;
              path.move_to(p[pos], p[pos + 1]);
              pos += 2;
              break;
            case 1:
              if (!require_params(iso, pos, 2, p.size(), "path line"))
                return;
              path.line_to(p[pos], p[pos + 1]);
              pos += 2;
              break;
            case 2:
              if (!require_params(iso, pos, 4, p.size(), "path quad"))
                return;
              path.quad_to(p[pos], p[pos + 1], p[pos + 2], p[pos + 3]);
              pos += 4;
              break;
            case 3:
              if (!require_params(iso, pos, 6, p.size(), "path cubic"))
                return;
              path.cubic_to(p[pos], p[pos + 1], p[pos + 2], p[pos + 3], p[pos + 4], p[pos + 5]);
              pos += 6;
              break;
            case 4:
              if (!require_params(iso, pos, 6, p.size(), "path arc"))
                return;
              path.arc(p[pos], p[pos + 1], p[pos + 2], p[pos + 3], p[pos + 4], p[pos + 5] != 0.0f);
              pos += 6;
              break;
            case 5:
              path.close();
              break;
            default:
              (void)throw_range_error(iso, "Primitives.drain: invalid path command");
              return;
            }
          }
          if (ops[i] == OP_FILL_PATH)
            primitives::fill_path(*cb, path, paint, primitives::fill_rule::nonzero, depth);
          else
            primitives::stroke_path(*cb, path, paint, line_width);
          break;
        }
        default: {
          std::string msg = "Primitives.drain: unsupported opcode " + std::to_string(ops[i]);
          (void)throw_range_error(iso, msg);
          return;
        }
        }
        ++executed;
      }
      if (pos != p.size()) {
        (void)throw_range_error(iso, "Primitives.drain: unused params");
        return;
      }
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, executed));
    }

    void p_blurRect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      // blurRect(cb, x, y, w, h, depth, color, dispersion, screen_w, screen_h)
      float x = float(num(ctx, info[1]));
      float y = float(num(ctx, info[2]));
      float w = float(num(ctx, info[3]));
      float h = float(num(ctx, info[4]));
      float d = float(num(ctx, info[5]));
      auto c = decode_color(iso, ctx, info[6]);
      float disp = float(num(ctx, info[7]));
      float sw = float(num(ctx, info[8]));
      float sh = float(num(ctx, info[9]));
      primitives::blur_rect(*cb, math::vec2{x, y}, math::vec2{w, h}, d, c, disp,
                            math::vec2{sw, sh});
    }

    void p_drawShadowRect(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      primitives::draw_shadow_rect(
          *cb, float(num(ctx, info[1])), float(num(ctx, info[2])), float(num(ctx, info[3])),
          float(num(ctx, info[4])), float(num(ctx, info[5])), decode_color(iso, ctx, info[6]),
          float(num(ctx, info[7])), float(num(ctx, info[8])), float(num(ctx, info[9])),
          float(num(ctx, info[10])), float(num(ctx, info[11])), float(num(ctx, info[12])));
    }

    void p_drawShadowRectRounded(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      primitives::optional_list<float, 4> rnd{0};
      if (info.Length() >= 7 && info[6]->IsFloat32Array()) {
        auto a = info[6].As<Float32Array>();
        float t[4]{};
        a->CopyContents(t, std::min<usize>(sizeof(t), a->ByteLength()));
        rnd[0] = t[0];
        rnd[1] = t[1];
        rnd[2] = t[2];
        rnd[3] = t[3];
      }
      primitives::draw_shadow_rect_rounded(
          *cb, float(num(ctx, info[1])), float(num(ctx, info[2])), float(num(ctx, info[3])),
          float(num(ctx, info[4])), rnd, float(num(ctx, info[5])), decode_color(iso, ctx, info[7]),
          float(num(ctx, info[8])), float(num(ctx, info[9])), float(num(ctx, info[10])),
          float(num(ctx, info[11])), float(num(ctx, info[12])), float(num(ctx, info[13])));
    }
    void p_blurQuad(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      math::vec4 p1, p2, p3, p4;
      if (info.Length() < 5 || !decode_vec4(info[1], p1) || !decode_vec4(info[2], p2) ||
          !decode_vec4(info[3], p3) || !decode_vec4(info[4], p4))
        return;
      auto c = info.Length() >= 6 ? decode_color(iso, ctx, info[5]) : white;
      float disp = info.Length() >= 7 ? float(num(ctx, info[6])) : 0.0f;
      float sw = info.Length() >= 8 ? float(num(ctx, info[7])) : 0.0f;
      float sh = info.Length() >= 9 ? float(num(ctx, info[8])) : 0.0f;
      primitives::color_list<4> cl{c, c, c, c};
      primitives::blur_quad(*cb, p1, p2, p3, p4, cl, disp, math::vec2{sw, sh});
    }
    // drawSprite(cb, spriteId, x, y, w, h, depth?, tint?) — samples from the default spritesheet.
    void p_drawSprite(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* cb = get_cb(info);
      if (!cb)
        return;
      if (info.Length() < 6) {
        (void)throw_type_error(iso, "drawSprite(cb, spriteId, x, y, w, h, depth?, tint?)");
        return;
      }
      const texture_id sprite_id = info[1]->Uint32Value(ctx).FromMaybe(0u);
      const math::vec2 at{float(num(ctx, info[2])), float(num(ctx, info[3]))};
      const math::vec2 size{float(num(ctx, info[4])), float(num(ctx, info[5]))};
      const float depth = info.Length() >= 7 ? float(num(ctx, info[6])) : 0.0f;
      const auto tint = info.Length() >= 8 ? decode_color(iso, ctx, info[7]) : white;
      const auto fallback = [&] { primitives::fill_rect(*cb, at, size, depth, tint); };
      if (sprite_id == null_texture) {
        fallback();
        return;
      }
      auto& sheet = get_default_spritesheet();
      const texture_id resolved = sheet.resolve_if(sprite_id, 0.0f);
      if ((resolved & (msprite_flag | xlsprite_flag)) != 0) {
        fallback();
        return;
      }
      const texture_id sprite_index = resolved & sprite_index_mask;
      if (sprite_index == 0 || sprite_index > sheet.sprites.size()) {
        fallback();
        return;
      }
      const sprite& spr = sheet.sprites[sprite_index - 1];
      const texture_id texture_index = spr.texture & sprite_index_mask;
      if (texture_index == 0 || texture_index > sheet.textures.size()) {
        fallback();
        return;
      }
      const texture_data& td = sheet.textures[texture_index - 1];
      if (td.size.x == 0 || td.size.y == 0) {
        fallback();
        return;
      }
      const float u0 = static_cast<float>(spr.at.x) / static_cast<float>(td.size.x);
      const float v0 = static_cast<float>(spr.at.y) / static_cast<float>(td.size.y);
      const float u1 = static_cast<float>(spr.at.x + spr.size.x) / static_cast<float>(td.size.x);
      const float v1 = static_cast<float>(spr.at.y + spr.size.y) / static_cast<float>(td.size.y);
      auto* vp = cb->allocate_strip(4, vertex_topology::triangle);
      vp[0] = make_vertex({at.x, at.y}, depth, {u0, v0}, spr.texture, tint);
      vp[1] = make_vertex({at.x + size.x, at.y}, depth, {u1, v0}, spr.texture, tint);
      vp[2] = make_vertex({at.x, at.y + size.y}, depth, {u0, v1}, spr.texture, tint);
      vp[3] = make_vertex({at.x + size.x, at.y + size.y}, depth, {u1, v1}, spr.texture, tint);
    }

    // Primitives.atlasEpoch() — combined generation of the shared glyph
    // atlases (mask + color). Caching layers stamp this value alongside any
    // recorded vertex data that references atlas UVs (drawText, drawTextRun,
    // …); a mismatch on replay means the atlas was repacked under them and
    // the stale UVs would sample the wrong glyphs. Wraps to 0 after 2^53.
    void p_atlasEpoch(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto& gc = font::shared_glyph_cache();
      const u64 mask_gen = gc.generation(font::Format::grayscale);
      const u64 color_gen = gc.generation(font::Format::bgra);
      // Mix the two generations into one Number. Both fit comfortably in
      // the safe integer range under realistic eviction rates.
      const double combined = static_cast<double>(mask_gen) + static_cast<double>(color_gen) * 1e9;
      info.GetReturnValue().Set(Number::New(iso, combined));
    }
  } // namespace

  void install_primitives_namespace(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto path_tpl = FunctionTemplate::New(iso, path_ctor);
    path_tpl->SetClassName("Path"_v8(iso));
    path_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto path_proto = path_tpl->PrototypeTemplate();
    path_proto->Set(iso, "moveTo", FunctionTemplate::New(iso, path_moveTo));
    path_proto->Set(iso, "lineTo", FunctionTemplate::New(iso, path_lineTo));
    path_proto->Set(iso, "quadTo", FunctionTemplate::New(iso, path_quadTo));
    path_proto->Set(iso, "cubicTo", FunctionTemplate::New(iso, path_cubicTo));
    path_proto->Set(iso, "arc", FunctionTemplate::New(iso, path_arc));
    path_proto->Set(iso, "close", FunctionTemplate::New(iso, path_close));
    path_proto->Set(iso, "reset", FunctionTemplate::New(iso, path_reset));
    global->Set(iso, "Path", path_tpl);
    auto ns = ObjectTemplate::New(iso);
#define P(name, fn) ns->Set(iso, name, FunctionTemplate::New(iso, fn))
    P("drawLine", p_drawLine);
    P("fillTriangle", p_fillTriangle);
    P("fillRect", p_fillRect);
    P("drawRect", p_drawRect);
    P("fillEllipse", p_fillEllipse);
    P("drawEllipse", p_drawEllipse);
    P("fillBox", p_fillBox);
    P("drawBox", p_drawBox);
    P("fillCbox", p_fillCbox);
    P("drawCbox", p_drawCbox);
    P("fillSphere", p_fillSphere);
    P("fillCylinder", p_fillCylinder);
    P("fillPyramid", p_fillPyramid);
    P("drawPyramid", p_drawPyramid);
    P("fillQuad", p_fillQuad);
    P("drawQuad", p_drawQuad);
    P("fillQuadRounded", p_fillQuadRounded);
    P("drawQuadRounded", p_drawQuadRounded);
    P("fillRectRounded", p_fillRectRounded);
    P("drawRectRounded", p_drawRectRounded);
    P("drawText", p_drawText);
    P("atlasEpoch", p_atlasEpoch);
    P("drawTextSpans", p_drawTextSpans);
    P("drawSelectionRects", p_drawSelectionRects);
    P("drawDecorationUnderline", p_drawDecorationUnderline);
    P("drawTextRun", p_drawTextRun);
    P("drawTextureQuad", p_drawTextureQuad);
    P("calcText", p_calcText);
    P("wrapTextNative", p_wrapTextNative);
    P("xAtGlyphIndexNative", p_xAtGlyphIndexNative);
    P("glyphIndexAtNative", p_glyphIndexAtNative);
    P("linearGradient", p_linearGradient);
    P("radialGradient", p_radialGradient);
    P("conicGradient", p_conicGradient);
    P("fillPath", p_fillPath);
    P("strokePath", p_strokePath);
    P("drawShadowRect", p_drawShadowRect);
    P("drawShadowRectRounded", p_drawShadowRectRounded);
    P("blurRect", p_blurRect);
    P("blurQuad", p_blurQuad);
    P("drawSprite", p_drawSprite);
    P("drain", p_drain);
#define C(name, value) ns->Set(iso, name, Integer::NewFromUnsigned(iso, value))
    C("OP_FILL_RECT", OP_FILL_RECT);
    C("OP_DRAW_RECT", OP_DRAW_RECT);
    C("OP_FILL_TRIANGLE", OP_FILL_TRIANGLE);
    C("OP_DRAW_LINE", OP_DRAW_LINE);
    C("OP_DRAW_TEXT", OP_DRAW_TEXT);
    C("OP_FILL_PATH", OP_FILL_PATH);
    C("OP_STROKE_PATH", OP_STROKE_PATH);
#undef C
#undef P
    global->Set(iso, "Primitives", ns);
  }
} // namespace fxe::js
