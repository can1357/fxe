// JS bindings for the `Spritesheet` global. Wraps fxe::spritesheet, retaining
// shared_ptr refs to source images so an Image.dispose() after add() does not
// invalidate sheet data.

#include "bind_spritesheet.hpp"
#include "bind_image.hpp"

#include <fxe/v8_helpers.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_strings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& sheet_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void sheet_reset_for_isolate(Isolate* iso) {
      auto& t = sheet_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct sheet_resetter_register {
      sheet_resetter_register() {
        register_template_resetter(&sheet_reset_for_isolate);
      }
    };
    static sheet_resetter_register s_sheet_resetter_register;

    void throw_type(Isolate* iso, const char* msg) {
      (void)throw_type_error(iso, msg);
    }

    spritesheet_holder* self_of(Local<Object> obj) {
      return static_cast<spritesheet_holder*>(unwrap(obj, TAG_SPRITESHEET));
    }

    void s_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall())
        return throw_type(iso, "Spritesheet must be constructed with `new`");
      auto self = info.This();
      auto* h = new spritesheet_holder{};
      set_native(iso, self, h, TAG_SPRITESHEET);
      h->bind(iso, self);
    }

    bool decode_rect([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> v, u32 img_w,
                     u32 img_h, math::uvec2& at, math::uvec2& size) {
      if (v.IsEmpty() || v->IsUndefined() || v->IsNull()) {
        at = {0, 0};
        size = {img_w, img_h};
        return true;
      }
      if (!v->IsArray())
        return false;
      auto a = v.As<Array>();
      if (a->Length() < 4)
        return false;
      u32 t[4]{};
      for (u32 i = 0; i < 4; ++i) {
        Local<Value> e;
        if (!a->Get(ctx, i).ToLocal(&e))
          return false;
        t[i] = e->Uint32Value(ctx).FromMaybe(0);
      }
      at = {t[0], t[1]};
      size = {t[2], t[3]};
      return true;
    }

    void m_add(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = self_of(info.This());
      if (!sh)
        return throw_type(iso, "Spritesheet.add: invalid receiver");
      auto* img = unwrap_image(info.Length() >= 1 ? info[0] : Local<Value>());
      if (!img || !img->tex)
        return throw_type(iso, "Spritesheet.add: arg 1 must be a live ImageHandle");

      // Copy the image's pixels into a fresh atlas slot owned by the sheet.
      // The shared_ptr ref is retained so the original image stays alive for
      // anyone else (the JS handle, other sheets) holding it.
      sh->retained.push_back(img->tex);
      texture_data copy = *img->tex;
      const u32 iw = copy.size.x;
      const u32 ih = copy.size.y;
      math::uvec2 at{}, size{};
      if (!decode_rect(iso, ctx, info.Length() >= 2 ? info[1] : Local<Value>(), iw, ih, at, size))
        return throw_type(iso, "Spritesheet.add: rect must be [x,y,w,h]");
      texture_id tex_id = sh->sheet.add_texture(std::move(copy));
      sprite spr{};
      spr.at = at;
      spr.size = size;
      spr.texture = tex_id;
      texture_id sprite_id = sh->sheet.add_sprite(spr);
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, sprite_id));
    }

    void m_addAnimated(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = self_of(info.This());
      if (!sh)
        return throw_type(iso, "Spritesheet.addAnimated: invalid receiver");
      if (info.Length() < 2 || !info[0]->IsArray() || !info[1]->IsArray())
        return throw_type(iso,
                          "Spritesheet.addAnimated(images: ImageHandle[], delaysMs: number[])");
      auto images = info[0].As<Array>();
      auto delays = info[1].As<Array>();
      if (images->Length() == 0)
        return throw_type(iso, "Spritesheet.addAnimated: empty image list");

      // Each frame becomes its own atlas texture; consecutive texture_ids form
      // the asprite range expected by spritesheet::resolve_if.
      texture_id base = 0;
      for (u32 i = 0; i < images->Length(); ++i) {
        Local<Value> e;
        if (!images->Get(ctx, i).ToLocal(&e))
          return throw_type(iso, "Spritesheet.addAnimated: bad image entry");
        auto* img = unwrap_image(e);
        if (!img || !img->tex)
          return throw_type(iso, "Spritesheet.addAnimated: live ImageHandle required");
        sh->retained.push_back(img->tex);
        texture_id id = sh->sheet.add_texture(*img->tex);
        if (i == 0)
          base = id;
      }

      asprite anim{};
      anim.base_texture = base;
      anim.delays.reserve(delays->Length());
      for (u32 i = 0; i < delays->Length(); ++i) {
        Local<Value> e;
        if (!delays->Get(ctx, i).ToLocal(&e))
          continue;
        anim.delays.push_back(static_cast<float>(e->NumberValue(ctx).FromMaybe(0.0)) / 1000.0f);
      }
      sh->sheet.asprites.push_back(std::move(anim));
      texture_id sprite_id = static_cast<texture_id>(sh->sheet.asprites.size()) | asprite_flag;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, sprite_id));
    }

    // Resolves a spriteId at time `time_ms` and returns
    // `{ textureId, u0, v0, u1, v1, width, height }`.
    void m_resolve(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* sh = self_of(info.This());
      if (!sh)
        return throw_type(iso, "Spritesheet.resolve: invalid receiver");
      if (info.Length() < 1)
        return throw_type(iso, "Spritesheet.resolve(spriteId, timeMs?)");
      texture_id requested = info[0]->Uint32Value(ctx).FromMaybe(0);
      float time_s = info.Length() >= 2
                         ? static_cast<float>(info[1]->NumberValue(ctx).FromMaybe(0.0)) / 1000.0f
                         : 0.0f;
      texture_id resolved = sh->sheet.resolve_if(requested, time_s);

      auto out = Object::New(iso);
      auto set = [&](Local<String> k, double v) { (void)out->Set(ctx, k, Number::New(iso, v)); };
      auto setU = [&](Local<String> k, u32 v) {
        (void)out->Set(ctx, k, Integer::NewFromUnsigned(iso, v));
      };

      // Look up the sprite slot. Static sprite ids are stored 1-based in
      // sheet.sprites; texture_data lives at sprite.texture (1-based into
      // sheet.textures).
      texture_id idx = resolved & sprite_mask;
      const sprite* spr = nullptr;
      if ((resolved & (asprite_flag | msprite_flag | xlsprite_flag)) == 0 && idx > 0 &&
          idx <= sh->sheet.sprites.size()) {
        spr = &sh->sheet.sprites[idx - 1];
      }
      if (!spr) {
        setU("textureId"_v8(iso), resolved);
        set("u0"_v8(iso), 0.0);
        set("v0"_v8(iso), 0.0);
        set("u1"_v8(iso), 1.0);
        set("v1"_v8(iso), 1.0);
        setU("width"_v8(iso), 0);
        setU("height"_v8(iso), 0);
        info.GetReturnValue().Set(out);
        return;
      }
      texture_id tex_idx = spr->texture & sprite_mask;
      double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
      if (tex_idx > 0 && tex_idx <= sh->sheet.textures.size()) {
        const auto& td = sh->sheet.textures[tex_idx - 1];
        const double tw = td.size.x ? double(td.size.x) : 1.0;
        const double th = td.size.y ? double(td.size.y) : 1.0;
        u0 = double(spr->at.x) / tw;
        v0 = double(spr->at.y) / th;
        u1 = double(spr->at.x + spr->size.x) / tw;
        v1 = double(spr->at.y + spr->size.y) / th;
      }
      setU("textureId"_v8(iso), spr->texture);
      set("u0"_v8(iso), u0);
      set("v0"_v8(iso), v0);
      set("u1"_v8(iso), u1);
      set("v1"_v8(iso), v1);
      setU("width"_v8(iso), spr->size.x);
      setU("height"_v8(iso), spr->size.y);
      info.GetReturnValue().Set(out);
    }

    void m_dispose(const FunctionCallbackInfo<Value>& info) {
      auto* sh = self_of(info.This());
      if (!sh)
        return;
      sh->sheet.textures.clear();
      sh->sheet.sprites.clear();
      sh->sheet.msprites.clear();
      sh->sheet.asprites.clear();
      sh->sheet.xlsprites.clear();
      sh->retained.clear();
    }
  } // namespace

  spritesheet_holder* unwrap_spritesheet(Local<Value> v) {
    if (v.IsEmpty() || !v->IsObject())
      return nullptr;
    return static_cast<spritesheet_holder*>(unwrap(v.As<Object>(), TAG_SPRITESHEET));
  }

  void install_spritesheet_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, s_constructor);
    tpl->SetClassName("Spritesheet"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "add", FunctionTemplate::New(iso, m_add));
    proto->Set(iso, "addAnimated", FunctionTemplate::New(iso, m_addAnimated));
    proto->Set(iso, "resolve", FunctionTemplate::New(iso, m_resolve));
    proto->Set(iso, "dispose", FunctionTemplate::New(iso, m_dispose));

    global->Set(iso, "Spritesheet", tpl);

    sheet_tpl_table()[iso].Reset(iso, tpl);
  }
} // namespace fxe::js
