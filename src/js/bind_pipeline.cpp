// Match brew V8 ABI: pointer compression + sandbox are enabled in libv8.dylib.
#define V8_COMPRESS_POINTERS 1

// JS bindings for fxe::pipeline.
//
// Type tag 'PIPL'.

#include "bind_pipeline.hpp"

#include "bind_image.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/pipeline.hpp>
#include <fxe/renderer.hpp>
#include <fxe/spritesheet.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    inline constexpr u32 TAG_PIPELINE = 0x5049504Cu; // 'PIPL'

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& pipeline_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }

    void pipeline_reset_for_isolate(Isolate* iso) {
      auto& t = pipeline_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }

    struct pipeline_resetter_register {
      pipeline_resetter_register() {
        register_template_resetter(&pipeline_reset_for_isolate);
      }
    };
    static pipeline_resetter_register s_pipeline_resetter_register;

    struct pipeline_holder {
      std::unique_ptr<pipeline> owned;
      uint32_t vertex_stride = 0;
    };

    void pipeline_finalizer(const WeakCallbackInfo<pipeline_holder>& info) {
      delete info.GetParameter();
    }

    [[nodiscard]] std::string utf8(Isolate* iso, Local<Value> value) {
      String::Utf8Value s(iso, value);
      return *s ? std::string(*s, s.length()) : std::string{};
    }

    void throw_type(Isolate* iso, const char* msg) {
      iso->ThrowException(Exception::TypeError(
          String::NewFromUtf8(iso, msg, NewStringType::kNormal).ToLocalChecked()));
    }

    void throw_error(Isolate* iso, const std::string& msg) {
      iso->ThrowException(Exception::Error(
          String::NewFromUtf8(iso, msg.c_str(), NewStringType::kNormal).ToLocalChecked()));
    }

    renderer* unwrap_renderer(Local<Value> value) {
      if (value.IsEmpty() || !value->IsObject())
        return nullptr;
      return static_cast<renderer*>(unwrap(value.As<Object>(), TAG_RENDERER));
    }

    command_buffer* unwrap_cb(Local<Value> value) {
      if (value.IsEmpty() || !value->IsObject())
        return nullptr;
      if (void* raw = unwrap(value.As<Object>(), TAG_COMMAND_BUFFER))
        return static_cast<command_buffer*>(raw);
      if (void* raw = unwrap(value.As<Object>(), TAG_RENDERER))
        return static_cast<command_buffer*>(static_cast<renderer*>(raw));
      return nullptr;
    }

    pipeline_holder* unwrap_pipeline(Local<Object> self) {
      return static_cast<pipeline_holder*>(unwrap(self, TAG_PIPELINE));
    }

    bool get_value(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                   Local<Value>& out) {
      return obj->Get(ctx, String::NewFromUtf8(iso, key, NewStringType::kNormal).ToLocalChecked())
          .ToLocal(&out);
    }

    bool get_required_string(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                             std::string& out) {
      Local<Value> value;
      if (!get_value(iso, ctx, obj, key, value) || !value->IsString())
        return false;
      out = utf8(iso, value);
      return true;
    }

    void get_optional_string(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                             std::string& out) {
      Local<Value> value;
      if (get_value(iso, ctx, obj, key, value) && value->IsString())
        out = utf8(iso, value);
    }

    bool get_required_u32(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                          uint32_t& out) {
      Local<Value> value;
      if (!get_value(iso, ctx, obj, key, value) || !value->IsNumber())
        return false;
      out = value->Uint32Value(ctx).FromMaybe(0);
      return true;
    }

    void get_optional_bool(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* key,
                           bool& out) {
      Local<Value> value;
      if (get_value(iso, ctx, obj, key, value) && !value->IsUndefined())
        out = value->BooleanValue(iso);
    }

    bool parse_vertex_format(const std::string& s, vertex_attribute::format& out) {
      if (s == "f32")
        out = vertex_attribute::format::f32;
      else if (s == "f32x2")
        out = vertex_attribute::format::f32x2;
      else if (s == "f32x3")
        out = vertex_attribute::format::f32x3;
      else if (s == "f32x4")
        out = vertex_attribute::format::f32x4;
      else if (s == "u32")
        out = vertex_attribute::format::u32;
      else if (s == "u8x4-norm")
        out = vertex_attribute::format::u8x4_norm;
      else
        return false;
      return true;
    }

    bool parse_desc(Isolate* iso, Local<Context> ctx, Local<Value> value, pipeline_desc& desc) {
      if (value.IsEmpty() || !value->IsObject())
        return false;
      auto obj = value.As<Object>();
      if (!get_required_string(iso, ctx, obj, "wgsl", desc.wgsl))
        return false;
      get_optional_string(iso, ctx, obj, "vsEntry", desc.vs_entry);
      get_optional_string(iso, ctx, obj, "fsEntry", desc.fs_entry);
      if (!get_required_u32(iso, ctx, obj, "vertexStride", desc.vertex_stride))
        return false;
      get_optional_bool(iso, ctx, obj, "depthTest", desc.depth_test);
      get_optional_bool(iso, ctx, obj, "blend", desc.blend);

      Local<Value> attrs_value;
      if (!get_value(iso, ctx, obj, "attrs", attrs_value) || !attrs_value->IsArray())
        return false;
      auto attrs = attrs_value.As<Array>();
      desc.attrs.clear();
      desc.attrs.reserve(attrs->Length());
      for (uint32_t i = 0; i < attrs->Length(); ++i) {
        Local<Value> attr_value;
        if (!attrs->Get(ctx, i).ToLocal(&attr_value) || !attr_value->IsObject())
          return false;
        auto attr_obj = attr_value.As<Object>();
        vertex_attribute attr{};
        if (!get_required_u32(iso, ctx, attr_obj, "location", attr.shader_location))
          return false;
        if (!get_required_u32(iso, ctx, attr_obj, "offset", attr.offset))
          return false;
        std::string format;
        if (!get_required_string(iso, ctx, attr_obj, "format", format))
          return false;
        if (!parse_vertex_format(format, attr.fmt)) {
          throw_type(iso, "Pipeline: unknown vertex attribute format");
          return false;
        }
        desc.attrs.push_back(attr);
      }
      return true;
    }

    void pipeline_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "Pipeline must be invoked with new");
        return;
      }
      if (info.Length() < 2) {
        throw_type(iso, "Pipeline(renderer, desc)");
        return;
      }
      auto* r = unwrap_renderer(info[0]);
      if (!r) {
        throw_type(iso, "Pipeline: first arg must be Renderer");
        return;
      }
      pipeline_desc desc;
      if (!parse_desc(iso, ctx, info[1], desc)) {
        if (!iso->HasPendingException())
          throw_type(iso, "Pipeline: desc must include wgsl, vertexStride, and attrs");
        return;
      }
      try {
        auto p = pipeline::create(*r, desc);
        auto* h = new pipeline_holder{std::move(p), desc.vertex_stride};
        auto self = info.This();
        self->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
        self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_PIPELINE));
        auto* persistent = new Global<Object>(iso, self);
        persistent->SetWeak(h, pipeline_finalizer, WeakCallbackType::kParameter);
      } catch (const std::exception& e) {
        throw_error(iso, e.what());
      }
    }

    void pipeline_update_uniforms(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = unwrap_pipeline(info.This());
      if (!h || !h->owned)
        return;
      if (info.Length() < 1 || (!info[0]->IsFloat32Array() && !info[0]->IsUint8Array())) {
        throw_type(iso, "Pipeline.updateUniforms(Float32Array | Uint8Array)");
        return;
      }
      auto view = info[0].As<ArrayBufferView>();
      std::vector<uint8_t> bytes(view->ByteLength());
      if (!bytes.empty())
        view->CopyContents(bytes.data(), bytes.size());
      h->owned->update_uniforms(bytes.data(), bytes.size());
    }

    void pipeline_bind_texture(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_pipeline(info.This());
      if (!h || !h->owned)
        return;
      if (info.Length() < 2 || !info[0]->IsNumber()) {
        throw_type(iso, "Pipeline.bindTexture(binding, imageOrTextureId)");
        return;
      }
      const uint32_t binding = info[0]->Uint32Value(ctx).FromMaybe(0);
      texture_id tex = null_texture;
      if (info[1]->IsNumber()) {
        tex = info[1]->Uint32Value(ctx).FromMaybe(null_texture);
      } else {
        auto* img = unwrap_image(info[1]);
        if (!img || !img->tex) {
          throw_type(iso, "Pipeline.bindTexture: arg 2 must be ImageHandle or texture id");
          return;
        }
      }
      h->owned->bind_texture(binding, tex);
    }

    void pipeline_draw(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = unwrap_pipeline(info.This());
      if (!h || !h->owned)
        return;
      if (info.Length() < 4) {
        throw_type(iso, "Pipeline.draw(cb, vertices, indices, matrix)");
        return;
      }
      auto* cb = unwrap_cb(info[0]);
      if (!cb) {
        throw_type(iso, "Pipeline.draw: first arg must be CommandBuffer or Renderer");
        return;
      }
      if (!info[1]->IsFloat32Array() || !info[2]->IsUint32Array() || !info[3]->IsFloat32Array()) {
        throw_type(iso, "Pipeline.draw: expected Float32Array, Uint32Array, Float32Array(16)");
        return;
      }
      auto vertices = info[1].As<Float32Array>();
      auto indices = info[2].As<Uint32Array>();
      auto matrix = info[3].As<Float32Array>();
      if (matrix->Length() < 16) {
        throw_type(iso, "Pipeline.draw: matrix must be Float32Array(16)");
        return;
      }
      if (h->vertex_stride == 0 || vertices->ByteLength() % h->vertex_stride != 0) {
        throw_type(iso, "Pipeline.draw: vertices byteLength must be a multiple of vertexStride");
        return;
      }

      std::vector<float> vertex_data(vertices->Length());
      if (!vertex_data.empty())
        vertices->CopyContents(vertex_data.data(), vertex_data.size() * sizeof(float));
      std::vector<uint32_t> index_data(indices->Length());
      if (!index_data.empty())
        indices->CopyContents(index_data.data(), index_data.size() * sizeof(uint32_t));
      float mat[16]{};
      matrix->CopyContents(mat, sizeof(mat));
      const size_t vertex_count = vertices->ByteLength() / h->vertex_stride;
      h->owned->draw(*cb, vertex_data.data(), vertex_count, index_data.data(), index_data.size(),
                     mat);
    }
  } // namespace

  void install_pipeline_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, pipeline_constructor);
    tpl->SetClassName(String::NewFromUtf8Literal(iso, "Pipeline"));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "updateUniforms", FunctionTemplate::New(iso, pipeline_update_uniforms));
    proto->Set(iso, "bindTexture", FunctionTemplate::New(iso, pipeline_bind_texture));
    proto->Set(iso, "draw", FunctionTemplate::New(iso, pipeline_draw));

    global->Set(iso, "Pipeline", tpl);
    pipeline_tpl_table()[iso].Reset(iso, tpl);
  }
} // namespace fxe::js
