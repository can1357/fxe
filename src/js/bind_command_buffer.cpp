// JS bindings for fxe::command_buffer / fxe::js::js_command_buffer.
//
// Type tag 'CMDB'. Internal field layout:
//   0 = v8::External pointing at the native storage object
//   1 = v8::Uint32 type tag (TAG_COMMAND_BUFFER, or TAG_RENDERER for Renderer)
//
// Script-created CommandBuffer instances own a js_command_buffer backed by
// V8 BackingStores. Renderer still inherits these methods until the renderer
// cutover; renderer storage is the legacy C++ command_buffer base.

#include "js_command_buffer.hpp"

#include <fxe/command_buffer.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/renderer.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <fxe/vertex.hpp>

#include <cstdint>
#include <exception>
#include <unordered_map>
#include <v8-fast-api-calls.h>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& cb_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void cb_reset_for_isolate(Isolate* iso) {
      auto& t = cb_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct cb_resetter_register {
      cb_resetter_register() {
        register_template_resetter(&cb_reset_for_isolate);
      }
    };
    static cb_resetter_register s_cb_resetter_register;

    struct cb_holder {
      js_command_buffer* ptr = nullptr;
      bool owned = false;
      Global<Object>* self = nullptr;
    };

    struct cb_target {
      command_sink* sink = nullptr;
      command_view* view = nullptr;
      js_command_buffer* js = nullptr;
      command_buffer* native = nullptr;
    };

    void cb_finalizer(const WeakCallbackInfo<cb_holder>& info) {
      auto* h = info.GetParameter();
      if (!h)
        return;
      if (h->self) {
        h->self->Reset();
        delete h->self;
      }
      if (h->owned)
        delete h->ptr;
      delete h;
    }

    cb_target unwrap_cb(Local<Object> self) {
      if (void* raw = unwrap(self, TAG_COMMAND_BUFFER)) {
        auto* cb = static_cast<js_command_buffer*>(raw);
        return {cb, cb, cb, nullptr};
      }
      if (void* raw = unwrap(self, TAG_RENDERER)) {
        auto* r = static_cast<renderer*>(raw);
        return {r, r, nullptr, nullptr};
      }
      return {};
    }

    void define_counter_property(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                 Local<String> key, u32 value) {
      (void)obj->DefineOwnProperty(ctx, key, Integer::NewFromUnsigned(iso, value), None);
    }

    void sync_js_fields(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                        const js_command_buffer& cb) {
      (void)obj->Set(ctx, "__fxe_v_len"_v8(iso), Integer::NewFromUnsigned(iso, cb.vertex_count()));
      (void)obj->Set(ctx, "__fxe_tri_len"_v8(iso),
                     Integer::NewFromUnsigned(iso, cb.index_count(vertex_topology::triangle)));
      (void)obj->Set(ctx, "__fxe_line_len"_v8(iso),
                     Integer::NewFromUnsigned(iso, cb.index_count(vertex_topology::line)));
      (void)obj->Set(ctx, "__fxe_epoch"_v8(iso), Integer::NewFromUnsigned(iso, cb.epoch_value()));
    }

    void define_js_fields(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                          const js_command_buffer& cb) {
      define_counter_property(iso, ctx, obj, "__fxe_v_len"_v8(iso), cb.vertex_count());
      define_counter_property(iso, ctx, obj, "__fxe_tri_len"_v8(iso),
                              cb.index_count(vertex_topology::triangle));
      define_counter_property(iso, ctx, obj, "__fxe_line_len"_v8(iso),
                              cb.index_count(vertex_topology::line));
      define_counter_property(iso, ctx, obj, "__fxe_epoch"_v8(iso), cb.epoch_value());
    }

    void sync_if_js(const FunctionCallbackInfo<Value>& info, const cb_target& target) {
      if (!target.js)
        return;
      auto* iso = info.GetIsolate();
      sync_js_fields(iso, iso->GetCurrentContext(), info.This(), *target.js);
    }

    void cb_clear(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto target = unwrap_cb(info.This());
      if (!target.sink)
        return;
      target.sink->clear();
      sync_if_js(info, target);
    }

    void cb_epoch(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, target.view->epoch_value()));
    }

    void cb_vertex_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      info.GetReturnValue().Set(
          Integer::NewFromUnsigned(iso, static_cast<u32>(target.view->vertices().size())));
    }

    void cb_index_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      u32 top = 0;
      if (info.Length() >= 1)
        top = info[0]->Uint32Value(ctx).FromMaybe(0);
      if (top >= static_cast<u32>(vertex_topology::max)) {
        (void)throw_range_error(iso, "topology out of range");
        return;
      }
      info.GetReturnValue().Set(Integer::NewFromUnsigned(
          iso, static_cast<u32>(target.view->indices(static_cast<vertex_topology>(top)).size())));
    }

    bool decode_mat4([[maybe_unused]] Isolate* iso, Local<Context> ctx, Local<Value> v,
                     math::mat4x4& out) {
      if (!v->IsFloat32Array())
        return false;
      auto arr = v.As<Float32Array>();
      if (arr->Length() < 16)
        return false;
      float tmp[16];
      arr->CopyContents(tmp, sizeof(tmp));
      // glm::mat4 is column-major and constructible from 16 floats.
      out = math::mat4x4(tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7], tmp[8],
                         tmp[9], tmp[10], tmp[11], tmp[12], tmp[13], tmp[14], tmp[15]);
      (void)ctx;
      return true;
    }

    bool decode_vec4(Isolate* iso, Local<Value> v, math::vec4& out) {
      if (v->IsFloat32Array()) {
        auto arr = v.As<Float32Array>();
        if (arr->Length() < 4)
          return false;
        float tmp[4];
        arr->CopyContents(tmp, sizeof(tmp));
        out = {tmp[0], tmp[1], tmp[2], tmp[3]};
        return true;
      }
      if (v->IsArray()) {
        auto arr = v.As<Array>();
        if (arr->Length() < 4)
          return false;
        auto ctx = iso->GetCurrentContext();
        float tmp[4];
        for (u32 i = 0; i < 4; ++i) {
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

    void cb_transform(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.sink)
        return;
      math::mat4x4 m{1.0f};
      if (info.Length() < 1 || !decode_mat4(iso, ctx, info[0], m)) {
        (void)throw_type_error(iso, "transform: expected Float32Array(16)");
        return;
      }
      target.sink->transform(m);
      sync_if_js(info, target);
    }

    void cb_queue(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.sink)
        return;
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "queue: expected CommandBuffer");
        return;
      }
      auto other = unwrap_cb(info[0].As<Object>());
      if (!other.view) {
        (void)throw_type_error(iso, "queue: argument is not a CommandBuffer");
        return;
      }
      math::mat4x4 m = math::identity();
      if (info.Length() >= 2 && !info[1]->IsUndefined()) {
        if (!decode_mat4(iso, ctx, info[1], m)) {
          (void)throw_type_error(iso, "queue: mat must be Float32Array(16)");
          return;
        }
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
      try {
        target.sink->queue(*other.view, m, tint);
      } catch (const std::exception& e) {
        (void)throw_error(iso, "queue failed: {}", e.what());
        return;
      }
      sync_if_js(info, target);
    }

    Local<ArrayBuffer> array_buffer_view(Isolate* iso, void* data, usize bytes) {
      auto bs = ArrayBuffer::NewBackingStore(data, bytes, [](void*, usize, void*) {}, nullptr);
      return ArrayBuffer::New(iso, std::move(bs));
    }

    Local<ArrayBuffer> backing_array_buffer(Isolate* iso, std::shared_ptr<BackingStore> store) {
      return ArrayBuffer::New(iso, std::move(store));
    }

    void set_buffer_epoch(Isolate* iso, Local<Context> ctx, Local<Object> out, u32 epoch) {
      (void)out->Set(ctx, "epoch"_v8(iso), Integer::NewFromUnsigned(iso, epoch));
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

    void cb_buffers(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      u32 top = 0;
      if (!read_topology_arg(info, 0, top))
        return;

      auto out = Object::New(iso);
      if (target.js) {
        const auto topology = static_cast<vertex_topology>(top);
        auto vab = backing_array_buffer(iso, target.js->vertex_store());
        auto iab = backing_array_buffer(iso, target.js->index_store(topology));
        (void)out->Set(
            ctx, "verts"_v8(iso),
            Float32Array::New(vab, 0, target.js->vertex_count() * sizeof(vertex) / sizeof(float)));
        (void)out->Set(ctx, "idxs"_v8(iso),
                       Uint32Array::New(iab, 0, target.js->index_count(topology)));
        set_buffer_epoch(iso, ctx, out, target.js->epoch_value());
      } else if (target.native) {
        auto& vbuf = target.native->vertex_buffer;
        auto& ibuf = target.native->index_buffers[top];
        auto vab = array_buffer_view(iso, vbuf.data(), vbuf.size() * sizeof(vertex));
        auto iab = array_buffer_view(iso, ibuf.data(), ibuf.size() * sizeof(u32));
        (void)out->Set(ctx, "verts"_v8(iso),
                       Float32Array::New(vab, 0, vbuf.size() * sizeof(vertex) / sizeof(float)));
        (void)out->Set(ctx, "idxs"_v8(iso), Uint32Array::New(iab, 0, ibuf.size()));
        set_buffer_epoch(iso, ctx, out, target.native->epoch);
      }
      info.GetReturnValue().Set(out);
    }

    void cb_vertex_buffer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      if (target.js) {
        auto ab = backing_array_buffer(iso, target.js->vertex_store());
        info.GetReturnValue().Set(
            Float32Array::New(ab, 0, target.js->vertex_count() * sizeof(vertex) / sizeof(float)));
      } else if (target.native) {
        auto& vbuf = target.native->vertex_buffer;
        auto ab = array_buffer_view(iso, vbuf.data(), vbuf.size() * sizeof(vertex));
        info.GetReturnValue().Set(
            Float32Array::New(ab, 0, vbuf.size() * sizeof(vertex) / sizeof(float)));
      }
    }

    // bounds(): { x, y, width, height } | null
    //
    // Axis-aligned bounding box over all queued vertex positions. Returns
    // null when the buffer has no vertices yet. Used by surface caching to
    // size the offscreen target before baking.
    void cb_bounds(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      if (target.view->vertices().empty()) {
        info.GetReturnValue().SetNull();
        return;
      }
      auto [mn, mx] = target.view->get_boundaries();
      auto out = Object::New(iso);
      (void)out->Set(ctx, "x"_v8(iso), Number::New(iso, static_cast<double>(mn.x)));
      (void)out->Set(ctx, "y"_v8(iso), Number::New(iso, static_cast<double>(mn.y)));
      (void)out->Set(ctx, "width"_v8(iso), Number::New(iso, static_cast<double>(mx.x - mn.x)));
      (void)out->Set(ctx, "height"_v8(iso), Number::New(iso, static_cast<double>(mx.y - mn.y)));
      info.GetReturnValue().Set(out);
    }

    void cb_index_buffer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      u32 top = 0;
      if (!read_topology_arg(info, 0, top))
        return;
      if (target.js) {
        const auto topology = static_cast<vertex_topology>(top);
        auto ab = backing_array_buffer(iso, target.js->index_store(topology));
        info.GetReturnValue().Set(Uint32Array::New(ab, 0, target.js->index_count(topology)));
      } else if (target.native) {
        auto& ibuf = target.native->index_buffers[top];
        auto ab = array_buffer_view(iso, ibuf.data(), ibuf.size() * sizeof(u32));
        info.GetReturnValue().Set(Uint32Array::New(ab, 0, ibuf.size()));
      }
    }

    void cb_is_empty(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      info.GetReturnValue().Set(target.view->is_empty());
    }

    void install_owned_js_buffer(Isolate* iso, Local<Context> ctx, Local<Object> inst,
                                 js_command_buffer* fresh) {
      auto* h = new cb_holder{fresh, true, nullptr};
      set_native(iso, inst, fresh, TAG_COMMAND_BUFFER);
      define_js_fields(iso, ctx, inst, *fresh);
      auto* persistent = new Global<Object>(iso, inst);
      fresh->set_js_object(persistent);
      h->self = persistent;
      persistent->SetWeak(h, cb_finalizer, WeakCallbackType::kParameter);
    }

    void cb_clone(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.view)
        return;
      auto tpl = cb_tpl_table()[iso].Get(iso);
      auto inst = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      js_command_buffer* fresh = nullptr;
      try {
        fresh = target.js ? new js_command_buffer(target.js->clone()) : new js_command_buffer(iso);
        if (!target.js)
          fresh->queue(*target.view);
      } catch (const std::exception& e) {
        delete fresh;
        (void)throw_error(iso, "clone failed: {}", e.what());
        return;
      }
      install_owned_js_buffer(iso, ctx, inst, fresh);
      info.GetReturnValue().Set(inst);
    }

    void cb_allocate(const FunctionCallbackInfo<Value>& info) {
      // This callback returns an object containing TypedArray subviews. V8 Fast
      // API callbacks cannot return newly allocated JS objects, so allocate()
      // intentionally remains on the normal V8 path. Returned views alias the
      // command buffer's storage for the reported epoch; mutating operations
      // advance the epoch and callers must reacquire views before reading or
      // writing through them.
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto target = unwrap_cb(info.This());
      if (!target.sink)
        return;
      if (info.Length() < 3) {
        (void)throw_type_error(iso, "allocate(vtx, idx, top)");
        return;
      }
      auto vtx = info[0]->Uint32Value(ctx).FromMaybe(0);
      auto idx = info[1]->Uint32Value(ctx).FromMaybe(0);
      u32 top = 0;
      if (!read_topology_arg(info, 2, top))
        return;
      const auto topology = static_cast<vertex_topology>(top);
      const u32 base = target.js ? target.js->vertex_count()
                                 : static_cast<u32>(target.native->vertex_buffer.size());
      const u32 index_base = target.js ? target.js->index_count(topology)
                                       : static_cast<u32>(target.native->index_buffers[top].size());
      try {
        (void)target.sink->allocate(vtx, idx, topology);
      } catch (const std::exception& e) {
        (void)throw_error(iso, "allocate failed: {}", e.what());
        return;
      }

      auto out = Object::New(iso);
      if (target.js) {
        auto vab = backing_array_buffer(iso, target.js->vertex_store());
        auto iab = backing_array_buffer(iso, target.js->index_store(topology));
        const usize vertex_offset = vtx == 0 ? 0 : static_cast<usize>(base) * sizeof(vertex);
        const usize index_offset = idx == 0 ? 0 : static_cast<usize>(index_base) * sizeof(u32);
        (void)out->Set(ctx, "verts"_v8(iso),
                       Float32Array::New(vab, vertex_offset, vtx * sizeof(vertex) / sizeof(float)));
        (void)out->Set(ctx, "idxs"_v8(iso), Uint32Array::New(iab, index_offset, idx));
        set_buffer_epoch(iso, ctx, out, target.js->epoch_value());
      } else if (target.native) {
        auto& vbuf = target.native->vertex_buffer;
        auto& ibuf = target.native->index_buffers[top];
        auto* vdata = vtx ? static_cast<void*>(vbuf.data() + base) : nullptr;
        auto* idata = idx ? static_cast<void*>(ibuf.data() + index_base) : nullptr;
        auto vab = array_buffer_view(iso, vdata, vtx * sizeof(vertex));
        auto iab = array_buffer_view(iso, idata, idx * sizeof(u32));
        (void)out->Set(ctx, "verts"_v8(iso),
                       Float32Array::New(vab, 0, vtx * sizeof(vertex) / sizeof(float)));
        (void)out->Set(ctx, "idxs"_v8(iso), Uint32Array::New(iab, 0, idx));
        set_buffer_epoch(iso, ctx, out, target.native->epoch);
      }
      (void)out->Set(ctx, "base"_v8(iso), Integer::NewFromUnsigned(iso, base));
      (void)out->Set(ctx, "indexBase"_v8(iso), Integer::NewFromUnsigned(iso, index_base));
      sync_if_js(info, target);
      info.GetReturnValue().Set(out);
    }

    void cb_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "CommandBuffer must be invoked with new");
        return;
      }
      auto ctx = iso->GetCurrentContext();
      auto self = info.This();
      install_owned_js_buffer(iso, ctx, self, new js_command_buffer(iso));
    }
  } // namespace

  // Public helpers used across binding TUs ----------------------------------

  Local<Object> wrap(Isolate* iso, Local<Context> ctx, Local<FunctionTemplate> tpl, void* native,
                     u32 type_tag) {
    auto inst = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
    set_native(iso, inst, native, type_tag);
    return inst;
  }

  void* unwrap(Local<Object> obj, u32 expected_type_tag) {
    if (obj.IsEmpty() || obj->InternalFieldCount() < 2)
      return nullptr;
    auto tag = obj->GetInternalField(1);
    if (tag.IsEmpty() || !tag->IsValue())
      return nullptr;
    auto tag_val = tag.As<Value>();
    if (!tag_val->IsUint32())
      return nullptr;
    auto tag_i = tag_val.As<Uint32>()->Value();
    if (tag_i != expected_type_tag)
      return nullptr;
    auto data = obj->GetInternalField(0);
    if (data.IsEmpty() || !data->IsValue())
      return nullptr;
    auto v = data.As<Value>();
    if (!v->IsExternal())
      return nullptr;
    return v.As<External>()->Value(v8::kExternalPointerTypeTagDefault);
  }

  // Per-binding accessor used by bind_renderer.cpp for FunctionTemplate::Inherit.
  Local<FunctionTemplate> get_command_buffer_template(Isolate* iso) {
    return cb_tpl_table()[iso].Get(iso);
  }

  void install_command_buffer_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, cb_constructor);
    tpl->SetClassName("CommandBuffer"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "clear", FunctionTemplate::New(iso, cb_clear));
    proto->Set(iso, "epoch", FunctionTemplate::New(iso, cb_epoch));
    proto->Set(iso, "vertexCount", FunctionTemplate::New(iso, cb_vertex_count));
    proto->Set(iso, "indexCount", FunctionTemplate::New(iso, cb_index_count));
    proto->Set(iso, "bounds", FunctionTemplate::New(iso, cb_bounds));
    proto->Set(iso, "transform", FunctionTemplate::New(iso, cb_transform));
    proto->Set(iso, "queue", FunctionTemplate::New(iso, cb_queue));
    proto->Set(iso, "buffers", FunctionTemplate::New(iso, cb_buffers));
    proto->Set(iso, "vertexBuffer", FunctionTemplate::New(iso, cb_vertex_buffer));
    proto->Set(iso, "indexBuffer", FunctionTemplate::New(iso, cb_index_buffer));
    proto->Set(iso, "allocate", FunctionTemplate::New(iso, cb_allocate));
    proto->Set(iso, "clone", FunctionTemplate::New(iso, cb_clone));
    proto->Set(iso, "isEmpty", FunctionTemplate::New(iso, cb_is_empty));

    global->Set(iso, "CommandBuffer", tpl);
    cb_tpl_table()[iso].Reset(iso, tpl);
  }

  Local<Object> make_command_buffer_object(Isolate* iso, Local<Context> ctx, command_buffer* cb) {
    auto tpl = cb_tpl_table()[iso].Get(iso);
    auto inst = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
    auto* fresh = new js_command_buffer(iso);
    if (cb)
      fresh->queue(*cb);
    install_owned_js_buffer(iso, ctx, inst, fresh);
    return inst;
  }
} // namespace fxe::js
