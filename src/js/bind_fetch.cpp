// fetch / Headers / Request / Response / AbortController bindings.
//
// Threading: the underlying http_client is single-threaded — its poll() drains
// libcurl-multi on whichever thread invokes it, which is always the V8 thread
// (called from bind_fetch::pump). We therefore avoid cross-thread machinery
// here entirely; completion callbacks fire synchronously inside pump().

#include "bind_fetch.hpp"

#include "../net/http_client.hpp"
#include "runtime/capabilities.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_HEADERS = 0x48454144u;  // 'HEAD'
    constexpr u32 TAG_REQUEST = 0x52455155u;  // 'REQU'
    constexpr u32 TAG_RESPONSE = 0x52455350u; // 'RESP'
    constexpr u32 TAG_ABORT = 0x41424F52u;    // 'ABOR'

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& headers_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& request_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& response_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& abort_ctrl_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& abort_signal_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void erase_for(std::unordered_map<Isolate*, TplGlobal>& t, Isolate* iso) {
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    void headers_reset_for_isolate(Isolate* iso) {
      erase_for(headers_tpl_table(), iso);
    }
    void request_reset_for_isolate(Isolate* iso) {
      erase_for(request_tpl_table(), iso);
    }
    void response_reset_for_isolate(Isolate* iso) {
      erase_for(response_tpl_table(), iso);
    }
    void abort_ctrl_reset_for_isolate(Isolate* iso) {
      erase_for(abort_ctrl_tpl_table(), iso);
    }
    void abort_signal_reset_for_isolate(Isolate* iso) {
      erase_for(abort_signal_tpl_table(), iso);
    }
    struct fetch_resetter_register {
      fetch_resetter_register() {
        register_template_resetter(&headers_reset_for_isolate);
        register_template_resetter(&request_reset_for_isolate);
        register_template_resetter(&response_reset_for_isolate);
        register_template_resetter(&abort_ctrl_reset_for_isolate);
        register_template_resetter(&abort_signal_reset_for_isolate);
      }
    };
    static fetch_resetter_register s_fetch_resetter_register;

    // ---------------- Helpers -----------------------------------------------

    Local<String> s8(Isolate* iso, const std::string& s) {
      return String::NewFromUtf8(iso, s.c_str(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }
    std::string to_str(Isolate* iso, Local<Value> v) {
      auto ctx = iso->GetCurrentContext();
      Local<String> s;
      if (!v->ToString(ctx).ToLocal(&s))
        return {};
      String::Utf8Value u(iso, s);
      return std::string(*u ? *u : "", *u ? u.length() : 0);
    }
    std::string ascii_lower(std::string s) {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }
    void throw_type(Isolate* iso, const char* m) {
      iso->ThrowException(Exception::TypeError(String::NewFromUtf8(iso, m).ToLocalChecked()));
    }

    Local<Value> make_permission_denied(Isolate* iso, std::string_view what) {
      std::string msg = "Permission denied: ";
      msg.append(what);
      auto err = Exception::Error(s8(iso, msg)).As<Object>();
      (void)err->Set(iso->GetCurrentContext(), s8(iso, "name"), s8(iso, "PermissionDenied"));
      return err;
    }

    std::string net_permission_message(std::string_view url) {
      std::string msg = "network access denied for '";
      msg.append(url);
      msg += "'";
      return msg;
    }

    bool guard_net(Isolate* iso, std::string_view url) {
      if (fxe::runtime::net_host_allowed(url))
        return true;
      iso->ThrowException(make_permission_denied(iso, net_permission_message(url)));
      return false;
    }

    // ---------------- Headers -----------------------------------------------

    struct headers_data {
      // case-insensitive: stored lower-cased
      std::vector<std::pair<std::string, std::string>> entries;
    };

    void headers_finalizer(const WeakCallbackInfo<headers_data>& info) {
      delete info.GetParameter();
    }

    headers_data* unwrap_headers(Local<Object> o) {
      return static_cast<headers_data*>(unwrap(o, TAG_HEADERS));
    }

    Local<Object> wrap_headers(Isolate* iso, Local<Context> ctx, std::unique_ptr<headers_data> d) {
      auto tpl = headers_tpl_table()[iso].Get(iso);
      Local<Object> obj = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      obj->SetInternalField(0, External::New(iso, d.get(), v8::kExternalPointerTypeTagDefault));
      obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_HEADERS));
      auto* persistent = new Global<Object>(iso, obj);
      persistent->SetWeak(d.release(), headers_finalizer, WeakCallbackType::kParameter);
      return obj;
    }

    void headers_populate_from_value(Isolate* iso, Local<Context> ctx, Local<Value> v,
                                     headers_data& d) {
      if (v.IsEmpty() || v->IsUndefined() || v->IsNull())
        return;
      if (v->IsArray()) {
        auto a = v.As<Array>();
        for (u32 i = 0; i < a->Length(); ++i) {
          Local<Value> entry;
          if (!a->Get(ctx, i).ToLocal(&entry) || !entry->IsArray())
            continue;
          auto e = entry.As<Array>();
          Local<Value> k, vv;
          if (e->Get(ctx, 0).ToLocal(&k) && e->Get(ctx, 1).ToLocal(&vv))
            d.entries.emplace_back(ascii_lower(to_str(iso, k)), to_str(iso, vv));
        }
      } else if (v->IsObject()) {
        // Could be another Headers — recover via tag.
        auto o = v.As<Object>();
        if (auto* other = unwrap_headers(o)) {
          for (auto& [k, vv] : other->entries)
            d.entries.emplace_back(k, vv);
          return;
        }
        Local<Array> names;
        if (!o->GetOwnPropertyNames(ctx).ToLocal(&names))
          return;
        for (u32 i = 0; i < names->Length(); ++i) {
          Local<Value> k;
          if (!names->Get(ctx, i).ToLocal(&k))
            continue;
          Local<Value> vv;
          if (!o->Get(ctx, k).ToLocal(&vv))
            continue;
          d.entries.emplace_back(ascii_lower(to_str(iso, k)), to_str(iso, vv));
        }
      }
    }

    void headers_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "Headers must be called with new");
        return;
      }
      auto d = std::make_unique<headers_data>();
      if (info.Length() >= 1)
        headers_populate_from_value(iso, ctx, info[0], *d);
      auto self = info.This();
      self->SetInternalField(0, External::New(iso, d.get(), v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_HEADERS));
      auto* persistent = new Global<Object>(iso, self);
      persistent->SetWeak(d.release(), headers_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(self);
    }

    void headers_get(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_headers(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = ascii_lower(to_str(iso, info[0]));
      std::string out;
      bool found = false;
      for (auto& [pk, pv] : d->entries)
        if (pk == k) {
          if (found)
            out += ", ";
          out += pv;
          found = true;
        }
      if (found)
        info.GetReturnValue().Set(s8(iso, out));
      else
        info.GetReturnValue().SetNull();
    }
    void headers_has(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_headers(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = ascii_lower(to_str(iso, info[0]));
      for (auto& [pk, pv] : d->entries)
        if (pk == k) {
          info.GetReturnValue().Set(true);
          return;
        }
      info.GetReturnValue().Set(false);
    }
    void headers_set(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_headers(info.This());
      if (!d || info.Length() < 2)
        return;
      std::string k = ascii_lower(to_str(iso, info[0]));
      std::string v = to_str(iso, info[1]);
      bool replaced = false;
      auto it = d->entries.begin();
      while (it != d->entries.end()) {
        if (it->first == k) {
          if (!replaced) {
            it->second = v;
            replaced = true;
            ++it;
          } else
            it = d->entries.erase(it);
        } else
          ++it;
      }
      if (!replaced)
        d->entries.emplace_back(std::move(k), std::move(v));
    }
    void headers_append(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_headers(info.This());
      if (!d || info.Length() < 2)
        return;
      d->entries.emplace_back(ascii_lower(to_str(iso, info[0])), to_str(iso, info[1]));
    }
    void headers_delete(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_headers(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = ascii_lower(to_str(iso, info[0]));
      auto it = d->entries.begin();
      while (it != d->entries.end()) {
        if (it->first == k)
          it = d->entries.erase(it);
        else
          ++it;
      }
    }
    void headers_for_each(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_headers(info.This());
      if (!d || info.Length() < 1 || !info[0]->IsFunction())
        return;
      auto fn = info[0].As<Function>();
      Local<Object> self = info.This();
      for (auto& [pk, pv] : d->entries) {
        Local<Value> argv[3] = {s8(iso, pv), s8(iso, pk), self};
        Local<Value> ignored;
        (void)fn->Call(ctx, Undefined(iso), 3, argv).ToLocal(&ignored);
      }
    }

    // ---------------- AbortController / AbortSignal -------------------------

    struct abort_signal_data {
      bool aborted = false;
      std::string reason;
      std::vector<Global<Function>> listeners; // 'abort' event listeners
    };

    void abort_signal_finalizer(const WeakCallbackInfo<abort_signal_data>& info) {
      delete info.GetParameter();
    }

    abort_signal_data* unwrap_abort_signal(Local<Object> o) {
      return static_cast<abort_signal_data*>(unwrap(o, TAG_ABORT));
    }

    Local<Object> make_abort_signal(Isolate* iso, Local<Context> ctx, abort_signal_data*& out_ptr) {
      auto tpl = abort_signal_tpl_table()[iso].Get(iso);
      Local<Object> obj = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      auto* d = new abort_signal_data();
      out_ptr = d;
      obj->SetInternalField(0, External::New(iso, d, v8::kExternalPointerTypeTagDefault));
      obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_ABORT));
      auto* persistent = new Global<Object>(iso, obj);
      persistent->SetWeak(d, abort_signal_finalizer, WeakCallbackType::kParameter);
      return obj;
    }

    void abort_signal_get_aborted(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* d = unwrap_abort_signal(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(d->aborted);
    }
    void abort_signal_get_reason(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_abort_signal(info.HolderV2());
      if (!d || !d->aborted) {
        info.GetReturnValue().SetUndefined();
        return;
      }
      info.GetReturnValue().Set(s8(iso, d->reason));
    }
    void abort_signal_add_listener(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsFunction())
        return;
      if (to_str(iso, info[0]) != "abort")
        return;
      auto* d = unwrap_abort_signal(info.This());
      if (!d)
        return;
      d->listeners.emplace_back(iso, info[1].As<Function>());
    }

    struct abort_controller_data {
      Global<Object> signal_obj;
      abort_signal_data* signal = nullptr;
    };
    void abort_ctrl_finalizer(const WeakCallbackInfo<abort_controller_data>& info) {
      delete info.GetParameter();
    }

    void abort_ctrl_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "AbortController must be called with new");
        return;
      }
      auto* d = new abort_controller_data();
      auto sig_obj = make_abort_signal(iso, ctx, d->signal);
      d->signal_obj.Reset(iso, sig_obj);
      auto self = info.This();
      self->SetInternalField(0, External::New(iso, d, v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_ABORT + 1));
      // Expose `signal` directly on the instance.
      self->Set(ctx, String::NewFromUtf8Literal(iso, "signal"), sig_obj).Check();
      auto* persistent = new Global<Object>(iso, self);
      persistent->SetWeak(d, abort_ctrl_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(self);
    }

    void abort_ctrl_abort(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto self = info.This();
      Local<Value> sig_v;
      if (!self->Get(ctx, String::NewFromUtf8Literal(iso, "signal")).ToLocal(&sig_v))
        return;
      if (!sig_v->IsObject())
        return;
      auto* sig = unwrap_abort_signal(sig_v.As<Object>());
      if (!sig || sig->aborted)
        return;
      sig->aborted = true;
      sig->reason = info.Length() >= 1 ? to_str(iso, info[0]) : std::string("aborted");
      // Fire listeners.
      for (auto& g : sig->listeners) {
        auto fn = g.Get(iso);
        Local<Value> ignored;
        (void)fn->Call(ctx, Undefined(iso), 0, nullptr).ToLocal(&ignored);
      }
      sig->listeners.clear();
    }

    // ---------------- In-flight tracking ------------------------------------

    struct in_flight_state {
      Global<Promise::Resolver> resolver;
      Global<Context> ctx;
      Global<Object> signal_obj; // optional
      net::http_request_id req_id = 0;
      bool aborted = false;
    };

    // No global registry — each in-flight state lives inside the libcurl
    // completion lambda (shared_ptr capture) and the optional abort listener.

    // ---------------- Response ----------------------------------------------

    struct response_data {
      long status = 200;
      std::string status_text;
      std::string url;
      std::string body; // raw bytes
      Global<Object> headers_obj;
      bool body_used = false;
    };
    void response_finalizer(const WeakCallbackInfo<response_data>& info) {
      delete info.GetParameter();
    }
    response_data* unwrap_response(Local<Object> o) {
      return static_cast<response_data*>(unwrap(o, TAG_RESPONSE));
    }

    Local<Object> wrap_response(Isolate* iso, Local<Context> ctx,
                                std::unique_ptr<response_data> d) {
      auto tpl = response_tpl_table()[iso].Get(iso);
      Local<Object> obj = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      obj->SetInternalField(0, External::New(iso, d.get(), v8::kExternalPointerTypeTagDefault));
      obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_RESPONSE));
      auto* persistent = new Global<Object>(iso, obj);
      persistent->SetWeak(d.release(), response_finalizer, WeakCallbackType::kParameter);
      return obj;
    }

    void resp_get_status(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* d = unwrap_response(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(static_cast<int32_t>(d->status));
    }
    void resp_get_status_text(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_response(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(s8(iso, d->status_text));
    }
    void resp_get_ok(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* d = unwrap_response(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(d->status >= 200 && d->status < 300);
    }
    void resp_get_url(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_response(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(s8(iso, d->url));
    }
    void resp_get_headers(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_response(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(d->headers_obj.Get(iso));
    }
    void resp_get_body_used(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* d = unwrap_response(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(d->body_used);
    }

    bool response_body_from_value(Isolate* iso, Local<Value> v, std::string& out) {
      if (v.IsEmpty() || v->IsUndefined() || v->IsNull()) {
        out.clear();
        return true;
      }
      if (v->IsString()) {
        out = to_str(iso, v);
        return true;
      }
      if (v->IsArrayBuffer()) {
        auto ab = v.As<ArrayBuffer>();
        auto bs = ab->GetBackingStore();
        out.assign(reinterpret_cast<const char*>(bs->Data()), bs->ByteLength());
        return true;
      }
      if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        auto ab = view->Buffer();
        auto bs = ab->GetBackingStore();
        auto* p = reinterpret_cast<const char*>(bs->Data()) + view->ByteOffset();
        out.assign(p, view->ByteLength());
        return true;
      }
      return false;
    }

    void resp_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_response(info.This());
      if (!d) {
        throw_type(iso, "Response.text: invalid this");
        return;
      }
      d->body_used = true;
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      auto str = String::NewFromUtf8(iso, d->body.c_str(), NewStringType::kNormal,
                                     static_cast<int>(d->body.size()))
                     .ToLocalChecked();
      resolver->Resolve(ctx, str).Check();
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    void resp_array_buffer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_response(info.This());
      if (!d) {
        throw_type(iso, "Response.arrayBuffer: invalid this");
        return;
      }
      d->body_used = true;
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      auto store = ArrayBuffer::NewBackingStore(iso, d->body.size());
      if (!d->body.empty())
        std::memcpy(store->Data(), d->body.data(), d->body.size());
      auto ab = ArrayBuffer::New(iso, std::move(store));
      resolver->Resolve(ctx, ab).Check();
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    void resp_json(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_response(info.This());
      if (!d) {
        throw_type(iso, "Response.json: invalid this");
        return;
      }
      d->body_used = true;
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      auto src = String::NewFromUtf8(iso, d->body.c_str(), NewStringType::kNormal,
                                     static_cast<int>(d->body.size()))
                     .ToLocalChecked();
      Local<Value> parsed;
      if (!JSON::Parse(ctx, src).ToLocal(&parsed)) {
        resolver
            ->Reject(ctx,
                     Exception::SyntaxError(String::NewFromUtf8Literal(iso, "JSON parse failed")))
            .Check();
      } else {
        resolver->Resolve(ctx, parsed).Check();
      }
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    void response_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "Response must be called with new");
        return;
      }

      auto d = std::make_unique<response_data>();
      if (info.Length() >= 1 && !response_body_from_value(iso, info[0], d->body)) {
        throw_type(iso, "Response: unsupported body type");
        return;
      }

      auto h_data = std::make_unique<headers_data>();
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto init = info[1].As<Object>();
        Local<Value> v;
        if (init->Get(ctx, String::NewFromUtf8Literal(iso, "status")).ToLocal(&v) &&
            !v->IsUndefined()) {
          auto status = v->Int32Value(ctx).FromMaybe(200);
          if (status < 200 || status > 599) {
            iso->ThrowException(Exception::RangeError(String::NewFromUtf8Literal(
                iso, "Response: status must be in the range 200 to 599")));
            return;
          }
          d->status = status;
        }
        if (init->Get(ctx, String::NewFromUtf8Literal(iso, "statusText")).ToLocal(&v) &&
            !v->IsUndefined())
          d->status_text = to_str(iso, v);
        if (init->Get(ctx, String::NewFromUtf8Literal(iso, "headers")).ToLocal(&v) &&
            !v->IsUndefined())
          headers_populate_from_value(iso, ctx, v, *h_data);
      }
      d->headers_obj.Reset(iso, wrap_headers(iso, ctx, std::move(h_data)));

      auto self = info.This();
      self->SetInternalField(0, External::New(iso, d.get(), v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_RESPONSE));
      auto* persistent = new Global<Object>(iso, self);
      persistent->SetWeak(d.release(), response_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(self);
    }

    // ---------------- Request -----------------------------------------------

    struct request_data {
      std::string url;
      std::string method = "GET";
      std::string body;
      Global<Object> headers_obj;
      Global<Object> signal_obj;
      std::string proxy;
      std::string range;
    };
    void request_finalizer(const WeakCallbackInfo<request_data>& info) {
      delete info.GetParameter();
    }
    request_data* unwrap_request(Local<Object> o) {
      return static_cast<request_data*>(unwrap(o, TAG_REQUEST));
    }

    bool extract_init(Isolate* iso, Local<Context> ctx, Local<Value> init_v, request_data& req,
                      std::string* body_text_or_null, bool* throw_stream) {
      *throw_stream = false;
      if (init_v.IsEmpty() || init_v->IsUndefined() || init_v->IsNull())
        return true;
      if (!init_v->IsObject())
        return true;
      auto init = init_v.As<Object>();
      Local<Value> v;
      if (init->Get(ctx, String::NewFromUtf8Literal(iso, "method")).ToLocal(&v) &&
          !v->IsUndefined())
        req.method = to_str(iso, v);
      if (init->Get(ctx, String::NewFromUtf8Literal(iso, "headers")).ToLocal(&v) &&
          !v->IsUndefined()) {
        auto h_data = std::make_unique<headers_data>();
        headers_populate_from_value(iso, ctx, v, *h_data);
        req.headers_obj.Reset(iso, wrap_headers(iso, ctx, std::move(h_data)));
      }
      if (init->Get(ctx, String::NewFromUtf8Literal(iso, "body")).ToLocal(&v) &&
          !v->IsUndefined() && !v->IsNull()) {
        if (v->IsString()) {
          req.body = to_str(iso, v);
        } else if (v->IsArrayBuffer()) {
          auto ab = v.As<ArrayBuffer>();
          auto bs = ab->GetBackingStore();
          req.body.assign(reinterpret_cast<const char*>(bs->Data()), bs->ByteLength());
        } else if (v->IsArrayBufferView()) {
          auto view = v.As<ArrayBufferView>();
          auto ab = view->Buffer();
          auto bs = ab->GetBackingStore();
          auto* p = reinterpret_cast<const char*>(bs->Data()) + view->ByteOffset();
          req.body.assign(p, view->ByteLength());
        } else if (v->IsObject()) {
          // Detect ReadableStream-shape: an object with `getReader` is a
          // streaming body, which v0 explicitly does not support.
          auto o = v.As<Object>();
          Local<Value> getter;
          if (o->Get(ctx, String::NewFromUtf8Literal(iso, "getReader")).ToLocal(&getter) &&
              getter->IsFunction()) {
            *throw_stream = true;
            return false;
          }
          // Best-effort: stringify objects that look like form data.
          req.body = to_str(iso, v);
        } else {
          req.body = to_str(iso, v);
        }
        if (body_text_or_null)
          *body_text_or_null = req.body;
      }
      if (init->Get(ctx, String::NewFromUtf8Literal(iso, "signal")).ToLocal(&v) && v->IsObject())
        req.signal_obj.Reset(iso, v.As<Object>());
      if (init->Get(ctx, String::NewFromUtf8Literal(iso, "proxy")).ToLocal(&v) &&
          !v->IsUndefined() && !v->IsNull())
        req.proxy = to_str(iso, v);
      if (init->Get(ctx, String::NewFromUtf8Literal(iso, "range")).ToLocal(&v) &&
          !v->IsUndefined() && !v->IsNull())
        req.range = to_str(iso, v);
      return true;
    }

    void request_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "Request must be called with new");
        return;
      }
      if (info.Length() < 1) {
        throw_type(iso, "Request: missing url");
        return;
      }
      auto* d = new request_data();
      d->url = to_str(iso, info[0]);
      bool throw_stream = false;
      if (info.Length() >= 2) {
        std::string ignored;
        if (!extract_init(iso, ctx, info[1], *d, &ignored, &throw_stream)) {
          if (throw_stream)
            throw_type(iso, "Request: ReadableStream body is not supported in v0");
          delete d;
          return;
        }
      }
      auto self = info.This();
      self->SetInternalField(0, External::New(iso, d, v8::kExternalPointerTypeTagDefault));
      self->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_REQUEST));
      auto* persistent = new Global<Object>(iso, self);
      persistent->SetWeak(d, request_finalizer, WeakCallbackType::kParameter);
      // Expose simple props as own properties so JS readers see them.
      self->Set(ctx, String::NewFromUtf8Literal(iso, "url"), s8(iso, d->url)).Check();
      self->Set(ctx, String::NewFromUtf8Literal(iso, "method"), s8(iso, d->method)).Check();
      info.GetReturnValue().Set(self);
    }

    // ---------------- fetch -------------------------------------------------

    void resolve_with_response(Isolate* iso, in_flight_state& st, net::http_response&& resp) {
      auto ctx = st.ctx.Get(iso);
      Context::Scope cs(ctx);
      HandleScope hs(iso);
      auto resolver = st.resolver.Get(iso);
      if (!resp.error.empty()) {
        std::string msg = "fetch failed: " + resp.error;
        resolver->Reject(ctx, Exception::Error(s8(iso, msg))).Check();
        return;
      }
      auto h_data = std::make_unique<headers_data>();
      for (auto& [k, v] : resp.headers)
        h_data->entries.emplace_back(ascii_lower(k), v);
      auto h_obj = wrap_headers(iso, ctx, std::move(h_data));
      auto rd = std::make_unique<response_data>();
      rd->status = resp.status;
      rd->status_text = std::move(resp.status_text);
      rd->url = std::move(resp.final_url);
      rd->body = std::move(resp.body);
      rd->headers_obj.Reset(iso, h_obj);
      auto resp_obj = wrap_response(iso, ctx, std::move(rd));
      resolver->Resolve(ctx, resp_obj).Check();
    }

    void cookie_jar_set(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 3) {
        throw_type(iso, "cookieJar.set: expected domain, name, value");
        return;
      }
      std::string path = "/";
      if (info.Length() >= 4 && !info[3]->IsUndefined() && !info[3]->IsNull())
        path = to_str(iso, info[3]);
      std::int64_t expires = 0;
      if (info.Length() >= 5 && !info[4]->IsUndefined() && !info[4]->IsNull())
        expires = info[4]->IntegerValue(ctx).FromMaybe(0);
      bool secure = false;
      if (info.Length() >= 6)
        secure = info[5]->BooleanValue(iso);
      bool http_only = false;
      if (info.Length() >= 7)
        http_only = info[6]->BooleanValue(iso);
      net::http_client::instance().cookies().set(to_str(iso, info[0]), to_str(iso, info[1]),
                                                 to_str(iso, info[2]), std::move(path), expires,
                                                 secure, http_only);
    }

    void cookie_jar_get(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1)
        return;
      auto out = net::http_client::instance().cookies().get(to_str(iso, info[0]));
      info.GetReturnValue().Set(s8(iso, out));
    }

    void cookie_jar_clear(const FunctionCallbackInfo<Value>& info) {
      net::http_client::instance().cookies().clear();
      (void)info;
    }

    void fetch_cookie_jar(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull())
        net::http_client::instance().set_cookie_file_path(to_str(iso, info[0]));
      auto obj = Object::New(iso);
      obj->Set(ctx, String::NewFromUtf8Literal(iso, "set"),
               Function::New(ctx, cookie_jar_set).ToLocalChecked())
          .Check();
      obj->Set(ctx, String::NewFromUtf8Literal(iso, "get"),
               Function::New(ctx, cookie_jar_get).ToLocalChecked())
          .Check();
      obj->Set(ctx, String::NewFromUtf8Literal(iso, "clear"),
               Function::New(ctx, cookie_jar_clear).ToLocalChecked())
          .Check();
      info.GetReturnValue().Set(obj);
    }

    void fetch_global(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      info.GetReturnValue().Set(resolver->GetPromise());
      if (info.Length() < 1) {
        resolver
            ->Reject(ctx,
                     Exception::TypeError(String::NewFromUtf8Literal(iso, "fetch: missing url")))
            .Check();
        return;
      }

      net::http_request hreq;
      Local<Object> signal_obj_local;
      bool have_signal = false;

      if (info[0]->IsObject() && unwrap_request(info[0].As<Object>())) {
        auto* rd = unwrap_request(info[0].As<Object>());
        hreq.url = rd->url;
        hreq.method = rd->method;
        hreq.body = rd->body;
        hreq.proxy = rd->proxy;
        hreq.range = rd->range;
        if (!rd->headers_obj.IsEmpty()) {
          auto* hd = unwrap_headers(rd->headers_obj.Get(iso));
          if (hd)
            for (auto& [k, v] : hd->entries)
              hreq.headers.emplace_back(k, v);
        }
        if (!rd->signal_obj.IsEmpty()) {
          signal_obj_local = rd->signal_obj.Get(iso);
          have_signal = true;
        }
      } else {
        hreq.url = to_str(iso, info[0]);
        hreq.method = "GET";
      }

      if (info.Length() >= 2 && info[1]->IsObject()) {
        request_data scratch;
        bool throw_stream = false;
        std::string body_str;
        if (!extract_init(iso, ctx, info[1], scratch, &body_str, &throw_stream)) {
          if (throw_stream) {
            resolver
                ->Reject(ctx, Exception::TypeError(String::NewFromUtf8Literal(
                                  iso, "fetch: ReadableStream body is not supported in v0")))
                .Check();
            return;
          }
        }
        if (!scratch.method.empty() && scratch.method != "GET")
          hreq.method = scratch.method;
        if (!scratch.body.empty())
          hreq.body = scratch.body;
        if (!scratch.headers_obj.IsEmpty()) {
          auto* hd = unwrap_headers(scratch.headers_obj.Get(iso));
          if (hd)
            for (auto& [k, v] : hd->entries)
              hreq.headers.emplace_back(k, v);
        }
        if (!scratch.proxy.empty())
          hreq.proxy = scratch.proxy;
        if (!scratch.range.empty())
          hreq.range = scratch.range;
        if (!scratch.signal_obj.IsEmpty()) {
          signal_obj_local = scratch.signal_obj.Get(iso);
          have_signal = true;
        }
      }

      if (hreq.url.empty()) {
        resolver
            ->Reject(ctx, Exception::TypeError(String::NewFromUtf8Literal(iso, "fetch: empty url")))
            .Check();
        return;
      }

      {
        TryCatch try_catch(iso);
        if (!guard_net(iso, hreq.url)) {
          auto err = try_catch.Exception();
          try_catch.Reset();
          resolver->Reject(ctx, err).Check();
          return;
        }
      }

      // Pre-aborted signal -> fast reject.
      if (have_signal) {
        auto* sig = unwrap_abort_signal(signal_obj_local);
        if (sig && sig->aborted) {
          resolver->Reject(ctx, Exception::Error(s8(iso, "aborted: " + sig->reason))).Check();
          return;
        }
      }

      auto state = std::make_shared<in_flight_state>();
      state->resolver.Reset(iso, resolver);
      state->ctx.Reset(iso, ctx);
      if (have_signal)
        state->signal_obj.Reset(iso, signal_obj_local);

      // Submit. Capture state by value so the lambda owns it. The completion
      // may run synchronously inside submit() when libcurl is unavailable,
      // so don't rely on any post-submit bookkeeping.
      auto cb_state = state;
      net::http_request_id id = net::http_client::instance().submit(
          std::move(hreq), [iso, cb_state](net::http_response resp) mutable {
            if (cb_state->aborted)
              return;
            HandleScope hs2(iso);
            resolve_with_response(iso, *cb_state, std::move(resp));
          });
      state->req_id = id;

      // If we have a signal, install an 'abort' listener that cancels the
      // in-flight curl handle and marks the state aborted.
      if (have_signal) {
        auto* sig = unwrap_abort_signal(signal_obj_local);
        if (sig) {
          struct abort_ctx {
            std::shared_ptr<in_flight_state> st;
          };
          auto* lctx = new abort_ctx{state};
          auto data = External::New(iso, lctx, v8::kExternalPointerTypeTagDefault);
          auto fn_maybe = Function::New(
              ctx,
              [](const FunctionCallbackInfo<Value>& i) {
                auto* a = static_cast<abort_ctx*>(
                    i.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
                if (!a || !a->st)
                  return;
                a->st->aborted = true;
                net::http_client::instance().abort(a->st->req_id);
              },
              data);
          if (!fn_maybe.IsEmpty())
            sig->listeners.emplace_back(iso, fn_maybe.ToLocalChecked());
        }
      }
    }

  } // namespace

  void install_fetch_globals(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);

    // Headers
    auto htpl = FunctionTemplate::New(iso, headers_ctor);
    htpl->SetClassName(String::NewFromUtf8Literal(iso, "Headers"));
    htpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto hproto = htpl->PrototypeTemplate();
    hproto->Set(iso, "get", FunctionTemplate::New(iso, headers_get));
    hproto->Set(iso, "has", FunctionTemplate::New(iso, headers_has));
    hproto->Set(iso, "set", FunctionTemplate::New(iso, headers_set));
    hproto->Set(iso, "append", FunctionTemplate::New(iso, headers_append));
    hproto->Set(iso, "delete", FunctionTemplate::New(iso, headers_delete));
    hproto->Set(iso, "forEach", FunctionTemplate::New(iso, headers_for_each));
    global->Set(iso, "Headers", htpl);
    headers_tpl_table()[iso].Reset(iso, htpl);

    // Request
    auto rtpl = FunctionTemplate::New(iso, request_ctor);
    rtpl->SetClassName(String::NewFromUtf8Literal(iso, "Request"));
    rtpl->InstanceTemplate()->SetInternalFieldCount(2);
    global->Set(iso, "Request", rtpl);
    request_tpl_table()[iso].Reset(iso, rtpl);

    // Response
    auto rsptpl = FunctionTemplate::New(iso, response_ctor);
    rsptpl->SetClassName(String::NewFromUtf8Literal(iso, "Response"));
    rsptpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto rspinst = rsptpl->InstanceTemplate();
    rspinst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "status"), resp_get_status);
    rspinst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "statusText"),
                                   resp_get_status_text);
    rspinst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "ok"), resp_get_ok);
    rspinst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "url"), resp_get_url);
    rspinst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "headers"), resp_get_headers);
    rspinst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "bodyUsed"), resp_get_body_used);
    auto rspproto = rsptpl->PrototypeTemplate();
    rspproto->Set(iso, "text", FunctionTemplate::New(iso, resp_text));
    rspproto->Set(iso, "arrayBuffer", FunctionTemplate::New(iso, resp_array_buffer));
    rspproto->Set(iso, "json", FunctionTemplate::New(iso, resp_json));
    global->Set(iso, "Response", rsptpl);
    response_tpl_table()[iso].Reset(iso, rsptpl);

    // AbortSignal
    auto sigtpl = FunctionTemplate::New(iso, [](const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      throw_type(iso, "AbortSignal cannot be constructed directly");
    });
    sigtpl->SetClassName(String::NewFromUtf8Literal(iso, "AbortSignal"));
    sigtpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto siginst = sigtpl->InstanceTemplate();
    siginst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "aborted"),
                                   abort_signal_get_aborted);
    siginst->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "reason"),
                                   abort_signal_get_reason);
    auto sigproto = sigtpl->PrototypeTemplate();
    sigproto->Set(iso, "addEventListener", FunctionTemplate::New(iso, abort_signal_add_listener));
    global->Set(iso, "AbortSignal", sigtpl);
    abort_signal_tpl_table()[iso].Reset(iso, sigtpl);

    // AbortController
    auto ctrltpl = FunctionTemplate::New(iso, abort_ctrl_ctor);
    ctrltpl->SetClassName(String::NewFromUtf8Literal(iso, "AbortController"));
    ctrltpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto ctrlproto = ctrltpl->PrototypeTemplate();
    ctrlproto->Set(iso, "abort", FunctionTemplate::New(iso, abort_ctrl_abort));
    global->Set(iso, "AbortController", ctrltpl);
    abort_ctrl_tpl_table()[iso].Reset(iso, ctrltpl);

    // fetch global
    auto fetch_tpl = FunctionTemplate::New(iso, fetch_global);
    fetch_tpl->Set(iso, "cookieJar", FunctionTemplate::New(iso, fetch_cookie_jar));
    global->Set(iso, "fetch", fetch_tpl);
  }

} // namespace fxe::js

namespace fxe::js::bind_fetch {
  void pump(v8::Isolate*) {
    fxe::net::http_client::instance().poll();
  }
} // namespace fxe::js::bind_fetch
