// JS WebSocket binding.
//
// Each `new WebSocket(url, protocols?)` spawns a worker thread inside
// fxe::net::websocket_client. Inbound events queue on the worker side and are
// drained on the V8 thread by `bind_websocket::pump(iso)`, which is invoked
// once per app_run_loop iteration. Sends are buffered through the same client
// and serialised by the worker.

#include "bind_websocket.hpp"
#include "bind_blob.hpp"

#include "../net/websocket_client.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_strings.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_WEBSOCKET = 0x57534F43u; // 'WSOC'

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& ws_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void cleanup_ws_holders_for_isolate(Isolate* iso);
    void ws_reset_for_isolate(Isolate* iso) {
      auto& t = ws_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
      cleanup_ws_holders_for_isolate(iso);
    }
    struct ws_resetter_register {
      ws_resetter_register() {
        register_template_resetter(&ws_reset_for_isolate);
      }
    };
    static ws_resetter_register s_ws_resetter_register;

    struct ws_handler_set {
      Global<Function> on_open;
      Global<Function> on_message;
      Global<Function> on_error;
      Global<Function> on_close;
      // addEventListener registry per-event.
      std::vector<Global<Function>> open_l;
      std::vector<Global<Function>> message_l;
      std::vector<Global<Function>> error_l;
      std::vector<Global<Function>> close_l;
    };

    struct ws_holder {
      std::unique_ptr<net::websocket_client> client;
      ws_handler_set h;
      std::string binary_type = "arraybuffer";
      Global<Object> self;
      Isolate* isolate = nullptr;
    };

    // All live holders, so pump can iterate.
    std::unordered_set<ws_holder*>& holders() {
      static thread_local std::unordered_set<ws_holder*> s;
      return s;
    }

    void destroy_ws_holder(ws_holder* h) {
      if (!h)
        return;
      holders().erase(h);
      h->self.Reset();
      delete h;
    }
    void cleanup_ws_holders_for_isolate(Isolate* iso) {
      std::vector<ws_holder*> stale;
      stale.reserve(holders().size());
      for (auto* h : holders()) {
        if (h->isolate == iso)
          stale.push_back(h);
      }
      for (auto* h : stale)
        destroy_ws_holder(h);
    }
    Local<String> s8(Isolate* iso, const std::string& s) {
      return String::NewFromUtf8(iso, s.c_str(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }
    std::string to_str(Isolate* iso, Local<Value> v) {
      auto ctx = iso->GetCurrentContext();
      Local<String> str;
      if (!v->ToString(ctx).ToLocal(&str))
        return {};
      String::Utf8Value u(iso, str);
      return std::string(*u ? *u : "", *u ? u.length() : 0);
    }
    void throw_type(Isolate* iso, const char* m) {
      iso->ThrowException(Exception::TypeError(String::NewFromUtf8(iso, m).ToLocalChecked()));
    }

    void ws_finalizer(const WeakCallbackInfo<ws_holder>& info) {
      destroy_ws_holder(info.GetParameter());
    }

    ws_holder* unwrap_ws(Local<Object> o) {
      return static_cast<ws_holder*>(unwrap(o, TAG_WEBSOCKET));
    }

    void ws_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "WebSocket must be called with new");
        return;
      }
      if (info.Length() < 1) {
        throw_type(iso, "WebSocket: missing url");
        return;
      }
      std::string url = to_str(iso, info[0]);
      std::vector<std::string> protocols;
      if (info.Length() >= 2 && !info[1]->IsUndefined() && !info[1]->IsNull()) {
        if (info[1]->IsArray()) {
          auto a = info[1].As<Array>();
          for (u32 i = 0; i < a->Length(); ++i) {
            Local<Value> v;
            if (a->Get(ctx, i).ToLocal(&v))
              protocols.push_back(to_str(iso, v));
          }
        } else {
          protocols.push_back(to_str(iso, info[1]));
        }
      }
      auto* h = new ws_holder();
      h->client = std::make_unique<net::websocket_client>();
      h->client->connect(url, protocols);

      auto self = info.This();
      self->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_WEBSOCKET));
      self->Set(ctx, "url"_v8(iso), s8(iso, url)).Check();
      h->isolate = iso;
      h->self.Reset(iso, self);
      h->self.SetWeak(h, ws_finalizer, WeakCallbackType::kParameter);
      holders().insert(h);
      info.GetReturnValue().Set(self);
    }

    // Accessors

    void ws_get_ready_state(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      info.GetReturnValue().Set(static_cast<int32_t>(h->client->ready_state()));
    }
    void ws_get_buffered_amount(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      info.GetReturnValue().Set(static_cast<double>(h->client->buffered_amount()));
    }
    void ws_get_protocol(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      info.GetReturnValue().Set(s8(iso, h->client->selected_protocol()));
    }
    void ws_get_extensions(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      (void)iso;
      info.GetReturnValue().Set(""_v8(info.GetIsolate()));
    }

    void ws_get_binary_type(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      info.GetReturnValue().Set(s8(iso, h->binary_type));
    }

    void ws_set_binary_type(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      const std::string value = to_str(iso, v);
      if (value == "arraybuffer" || value == "blob")
        h->binary_type = value;
    }

    // on{open,message,error,close} — store the latest assigned function.

    Global<Function>& field_for(ws_holder* h, const std::string& key) {
      static Global<Function> dummy;
      if (key == "onopen")
        return h->h.on_open;
      if (key == "onmessage")
        return h->h.on_message;
      if (key == "onerror")
        return h->h.on_error;
      if (key == "onclose")
        return h->h.on_close;
      return dummy;
    }

    void ws_set_handler(Local<Name> name, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      String::Utf8Value u(iso, name);
      std::string key(*u ? *u : "");
      auto& slot = field_for(h, key);
      if (v->IsFunction())
        slot.Reset(iso, v.As<Function>());
      else
        slot.Reset();
    }
    void ws_get_handler(Local<Name> name, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_ws(info.HolderV2());
      if (!h)
        return;
      String::Utf8Value u(iso, name);
      auto& slot = field_for(h, std::string(*u ? *u : ""));
      if (slot.IsEmpty())
        info.GetReturnValue().SetNull();
      else
        info.GetReturnValue().Set(slot.Get(iso));
    }

    // Methods

    void ws_send(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = unwrap_ws(info.This());
      if (!h || info.Length() < 1)
        return;
      auto v = info[0];
      if (v->IsString()) {
        h->client->send_text(to_str(iso, v));
      } else if (v->IsArrayBuffer()) {
        auto ab = v.As<ArrayBuffer>();
        auto bs = ab->GetBackingStore();
        std::vector<std::uint8_t> bytes(reinterpret_cast<std::uint8_t*>(bs->Data()),
                                        reinterpret_cast<std::uint8_t*>(bs->Data()) +
                                            bs->ByteLength());
        h->client->send_binary(std::move(bytes));
      } else if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        auto ab = view->Buffer();
        auto bs = ab->GetBackingStore();
        auto* p = reinterpret_cast<std::uint8_t*>(bs->Data()) + view->ByteOffset();
        std::vector<std::uint8_t> bytes(p, p + view->ByteLength());
        h->client->send_binary(std::move(bytes));
      } else {
        h->client->send_text(to_str(iso, v));
      }
    }

    void ws_close(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_ws(info.This());
      if (!h)
        return;
      std::uint16_t code = 1000;
      std::string reason;
      if (info.Length() >= 1 && info[0]->IsNumber())
        code = static_cast<std::uint16_t>(info[0]->Uint32Value(ctx).FromMaybe(1000u));
      if (info.Length() >= 2)
        reason = to_str(iso, info[1]);
      h->client->close(code, std::move(reason));
    }

    void ws_add_event_listener(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = unwrap_ws(info.This());
      if (!h || info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction())
        return;
      std::string evt = to_str(iso, info[0]);
      auto fn = info[1].As<Function>();
      if (evt == "open")
        h->h.open_l.emplace_back(iso, fn);
      else if (evt == "message")
        h->h.message_l.emplace_back(iso, fn);
      else if (evt == "error")
        h->h.error_l.emplace_back(iso, fn);
      else if (evt == "close")
        h->h.close_l.emplace_back(iso, fn);
    }

    void ws_remove_event_listener(const FunctionCallbackInfo<Value>& info) {
      // Functions don't compare structurally in V8 once stored in Global; we
      // do a pointer-equal sweep using SameValue.
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = unwrap_ws(info.This());
      if (!h || info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction())
        return;
      std::string evt = to_str(iso, info[0]);
      auto target = info[1].As<Function>();
      auto sweep = [&](std::vector<Global<Function>>& v) {
        auto it = v.begin();
        while (it != v.end()) {
          if (it->Get(iso)->StrictEquals(target))
            it = v.erase(it);
          else
            ++it;
        }
      };
      if (evt == "open")
        sweep(h->h.open_l);
      else if (evt == "message")
        sweep(h->h.message_l);
      else if (evt == "error")
        sweep(h->h.error_l);
      else if (evt == "close")
        sweep(h->h.close_l);
    }

    Local<Object> make_event_obj(Isolate* iso, Local<Context> ctx, const std::string& type) {
      auto o = Object::New(iso);
      o->Set(ctx, "type"_v8(iso), s8(iso, type)).Check();
      return o;
    }

    void dispatch(Isolate* iso, Local<Context> ctx, ws_holder* h, const std::string& type,
                  Local<Value> arg) {
      auto self = h->self.Get(iso);
      // on<type> handler
      Global<Function>* slot = nullptr;
      std::vector<Global<Function>>* lst = nullptr;
      if (type == "open") {
        slot = &h->h.on_open;
        lst = &h->h.open_l;
      } else if (type == "message") {
        slot = &h->h.on_message;
        lst = &h->h.message_l;
      } else if (type == "error") {
        slot = &h->h.on_error;
        lst = &h->h.error_l;
      } else if (type == "close") {
        slot = &h->h.on_close;
        lst = &h->h.close_l;
      } else {
        return;
      }
      Local<Value> argv[1] = {arg};
      if (slot && !slot->IsEmpty()) {
        auto fn = slot->Get(iso);
        Local<Value> ignored;
        (void)fn->Call(ctx, self, 1, argv).ToLocal(&ignored);
      }
      if (lst) {
        for (auto& g : *lst) {
          auto fn = g.Get(iso);
          Local<Value> ignored;
          (void)fn->Call(ctx, self, 1, argv).ToLocal(&ignored);
        }
      }
    }

  } // namespace

  void install_websocket_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, ws_ctor);
    tpl->SetClassName("WebSocket"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto inst = tpl->InstanceTemplate();
    inst->SetNativeDataProperty("readyState"_v8(iso), ws_get_ready_state,
                                nullptr);
    inst->SetNativeDataProperty("bufferedAmount"_v8(iso),
                                ws_get_buffered_amount, nullptr);
    inst->SetNativeDataProperty("protocol"_v8(iso), ws_get_protocol,
                                nullptr);
    inst->SetNativeDataProperty("extensions"_v8(iso), ws_get_extensions,
                                nullptr);
    inst->SetNativeDataProperty("binaryType"_v8(iso), ws_get_binary_type,
                                ws_set_binary_type);
    // Handler properties via NativeAccessor on Name
    auto add_handler = [&](Local<String> name) {
      inst->SetNativeDataProperty(
          name,
          // get
          [](Local<Name> n, const PropertyCallbackInfo<Value>& i) {
            ws_get_handler(n.As<Name>(), i);
          },
          [](Local<Name> n, Local<Value> v, const PropertyCallbackInfo<void>& i) {
            ws_set_handler(n.As<Name>(), v, i);
          });
    };
    add_handler("onopen"_v8(iso));
    add_handler("onmessage"_v8(iso));
    add_handler("onerror"_v8(iso));
    add_handler("onclose"_v8(iso));

    auto proto = tpl->PrototypeTemplate();
    proto->Set(iso, "send", FunctionTemplate::New(iso, ws_send));
    proto->Set(iso, "close", FunctionTemplate::New(iso, ws_close));
    proto->Set(iso, "addEventListener", FunctionTemplate::New(iso, ws_add_event_listener));
    proto->Set(iso, "removeEventListener", FunctionTemplate::New(iso, ws_remove_event_listener));

    // Class-level constants for readyState.
    auto attach_const = [&](Local<String> name, int v) {
      tpl->Set(name, Integer::New(iso, v),
               static_cast<PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
    };
    attach_const("CONNECTING"_v8(iso), 0);
    attach_const("OPEN"_v8(iso), 1);
    attach_const("CLOSING"_v8(iso), 2);
    attach_const("CLOSED"_v8(iso), 3);

    global->Set(iso, "WebSocket", tpl);
    ws_tpl_table()[iso].Reset(iso, tpl);
  }

} // namespace fxe::js

namespace fxe::js::bind_websocket {
  void pump(v8::Isolate* iso) {
    using namespace v8;
    HandleScope hs(iso);
    auto ctx = iso->GetCurrentContext();
    if (ctx.IsEmpty())
      return;
    Context::Scope cs(ctx);
    // Snapshot to allow JS handlers to mutate the holders set safely.
    std::vector<fxe::js::ws_holder*> snapshot;
    snapshot.reserve(fxe::js::holders().size());
    for (auto* h : fxe::js::holders())
      snapshot.push_back(h);
    for (auto* h : snapshot) {
      auto events = h->client->drain_events();
      for (auto& ev : events) {
        switch (ev.kind) {
        case fxe::net::ws_event_kind::open: {
          auto eo = fxe::js::make_event_obj(iso, ctx, std::string("open"));
          fxe::js::dispatch(iso, ctx, h, "open", eo);
        } break;
        case fxe::net::ws_event_kind::message_text: {
          auto eo = fxe::js::make_event_obj(iso, ctx, std::string("message"));
          eo->Set(ctx, "data"_v8(iso), fxe::js::s8(iso, ev.text)).Check();
          fxe::js::dispatch(iso, ctx, h, "message", eo);
        } break;
        case fxe::net::ws_event_kind::message_binary: {
          auto eo = fxe::js::make_event_obj(iso, ctx, std::string("message"));
          if (h->binary_type == "blob") {
            auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(ev.binary));
            eo->Set(ctx, "data"_v8(iso),
                    fxe::js::make_blob_object(iso, ctx, std::move(bytes)))
                .Check();
          } else {
            auto store = ArrayBuffer::NewBackingStore(iso, ev.binary.size());
            if (!ev.binary.empty())
              std::memcpy(store->Data(), ev.binary.data(), ev.binary.size());
            auto ab = ArrayBuffer::New(iso, std::move(store));
            eo->Set(ctx, "data"_v8(iso), ab).Check();
          }
          fxe::js::dispatch(iso, ctx, h, "message", eo);
        } break;
        case fxe::net::ws_event_kind::error_: {
          auto eo = fxe::js::make_event_obj(iso, ctx, std::string("error"));
          eo->Set(ctx, "message"_v8(iso), fxe::js::s8(iso, ev.text))
              .Check();
          fxe::js::dispatch(iso, ctx, h, "error", eo);
        } break;
        case fxe::net::ws_event_kind::close: {
          auto eo = fxe::js::make_event_obj(iso, ctx, std::string("close"));
          eo->Set(ctx, "code"_v8(iso), Integer::New(iso, ev.code)).Check();
          eo->Set(ctx, "reason"_v8(iso), fxe::js::s8(iso, ev.reason))
              .Check();
          eo->Set(ctx, "wasClean"_v8(iso),
                  v8::Boolean::New(iso, ev.was_clean))
              .Check();
          fxe::js::dispatch(iso, ctx, h, "close", eo);
        } break;
        }
      }
    }
  }
} // namespace fxe::js::bind_websocket
