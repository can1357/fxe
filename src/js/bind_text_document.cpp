// JS binding for fxe::text_document — exposes a `TextDocument` class on the
// global. Internal field layout:
//   0 = v8::External pointing at the C++ td_holder (owns the doc + V8 self ref)
//   1 = v8::Uint32 type tag (TAG_TEXT_DOCUMENT)
//
// Construction allocates a heap text_document. Listener callbacks are V8
// functions held as Globals; we drop them when the document is finalised.

#include "bind_text_document.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/text_document.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <unordered_map>
#include <utility>
#include <v8.h>
#include <vector>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_TEXT_DOCUMENT = 0x54584F44u; // 'TXOD'

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& td_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void td_reset_for_isolate(Isolate* iso) {
      auto& t = td_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct td_resetter_register {
      td_resetter_register() {
        register_template_resetter(&td_reset_for_isolate);
      }
    };
    static td_resetter_register s_td_resetter_register;

    struct js_listener {
      Isolate* iso = nullptr;
      Global<Function> fn;
      text_document::listener_id id = 0;
    };

    struct td_holder {
      text_document* doc = nullptr;
      Global<Object>* self = nullptr;
      Isolate* iso = nullptr;
      // Listeners keyed by document subscription id.
      std::vector<std::unique_ptr<js_listener>> listeners;
    };

    void td_finalizer(const WeakCallbackInfo<td_holder>& info) {
      auto* h = info.GetParameter();
      if (!h)
        return;
      // Drop V8 listener handles before destroying doc.
      for (auto& l : h->listeners)
        l->fn.Reset();
      h->listeners.clear();
      if (h->self) {
        h->self->Reset();
        delete h->self;
      }
      delete h->doc;
      delete h;
    }

    text_document* unwrap_doc(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      auto o = v.As<Object>();
      if (auto* h = static_cast<td_holder*>(unwrap(o, TAG_TEXT_DOCUMENT)))
        return h->doc;
      return nullptr;
    }

    td_holder* unwrap_holder(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      return static_cast<td_holder*>(unwrap(v.As<Object>(), TAG_TEXT_DOCUMENT));
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    Local<String> u16_to_v8(Isolate* iso, std::u16string_view s) {
      // V8 String::NewFromTwoByte accepts uint16_t*; char16_t is layout-
      // compatible.
      return String::NewFromTwoByte(iso, reinterpret_cast<const uint16_t*>(s.data()),
                                    NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::u16string v8_to_u16(Isolate* iso, Local<Value> v) {
      // Round-trip via UTF-8 (V8's String::Write was removed; UTF-8 path
      // also handles non-string coercions safely via Utf8Value).
      if (v.IsEmpty())
        return {};
      String::Utf8Value u(iso, v);
      std::u16string out;
      const char* p = *u;
      if (!p)
        return out;
      const char* end = p + u.length();
      out.reserve(static_cast<usize>(u.length()));
      while (p < end) {
        char32_t cp;
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) {
          cp = c;
          ++p;
        } else if ((c & 0xE0) == 0xC0 && end - p >= 2) {
          cp = static_cast<char32_t>(((c & 0x1Fu) << 6) |
                                     (static_cast<unsigned char>(p[1]) & 0x3Fu));
          p += 2;
        } else if ((c & 0xF0) == 0xE0 && end - p >= 3) {
          cp = static_cast<char32_t>(((c & 0x0Fu) << 12) |
                                     ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 6) |
                                     (static_cast<unsigned char>(p[2]) & 0x3Fu));
          p += 3;
        } else if ((c & 0xF8) == 0xF0 && end - p >= 4) {
          cp = static_cast<char32_t>(((c & 0x07u) << 18) |
                                     ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 12) |
                                     ((static_cast<unsigned char>(p[2]) & 0x3Fu) << 6) |
                                     (static_cast<unsigned char>(p[3]) & 0x3Fu));
          p += 4;
        } else {
          cp = 0xFFFDu;
          ++p;
        }
        if (cp <= 0xFFFFu) {
          out.push_back(static_cast<char16_t>(cp));
        } else {
          cp -= 0x10000u;
          out.push_back(static_cast<char16_t>(0xD800u | ((cp >> 10) & 0x3FFu)));
          out.push_back(static_cast<char16_t>(0xDC00u | (cp & 0x3FFu)));
        }
      }
      return out;
    }

    u32 to_u32(Local<Context> ctx, Local<Value> v, u32 def = 0) {
      return v->Uint32Value(ctx).FromMaybe(def);
    }

    Local<Object> make_edit_object(Isolate* iso, Local<Context> ctx, const text_document_edit& e) {
      auto o = Object::New(iso);
      (void)o->Set(ctx, "start"_v8(iso), Integer::NewFromUnsigned(iso, e.start));
      (void)o->Set(ctx, "removed"_v8(iso), Integer::NewFromUnsigned(iso, e.removed));
      (void)o->Set(ctx, "inserted"_v8(iso), u16_to_v8(iso, e.inserted));
      (void)o->Set(ctx, "deleted"_v8(iso), u16_to_v8(iso, e.deleted));
      return o;
    }

    // ---------------- Methods ----------------

    void td_constructor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "TextDocument must be invoked with new");
        return;
      }
      HandleScope hs(iso);
      auto self = info.This();
      text_document* doc;
      if (info.Length() >= 1 && info[0]->IsString()) {
        std::string init = utf8(iso, info[0]);
        doc = new text_document(std::string_view{init});
      } else {
        doc = new text_document();
      }
      auto* h = new td_holder();
      h->doc = doc;
      h->iso = iso;
      set_native(iso, self, h, TAG_TEXT_DOCUMENT);
      auto* persistent = new Global<Object>(iso, self);
      h->self = persistent;
      persistent->SetWeak(h, td_finalizer, WeakCallbackType::kParameter);
    }

    void td_length(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_doc(info.This());
      if (!d) {
        (void)throw_type_error(iso, "invalid TextDocument");
        return;
      }
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->length()));
    }
    void td_line_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->line_count()));
    }
    void td_revision(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->revision()));
    }
    void td_piece_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->piece_count()));
    }

    void td_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      auto s = d->slice(0, d->length());
      info.GetReturnValue().Set(u16_to_v8(iso, s));
    }
    void td_slice(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 a = to_u32(ctx, info.Length() >= 1 ? info[0] : Undefined(iso).As<Value>(), 0);
      const u32 b =
          to_u32(ctx, info.Length() >= 2 ? info[1] : Undefined(iso).As<Value>(), d->length());
      info.GetReturnValue().Set(u16_to_v8(iso, d->slice(a, b)));
    }
    void td_char_code_at(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 off = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->code_unit_at(off)));
    }

    void td_line_to_offset(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 line = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->line_to_offset(line)));
    }
    void td_offset_to_line(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 off = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, d->offset_to_line(off)));
    }
    void td_line_range(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 line = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      auto r = d->line_range(line);
      auto o = Object::New(iso);
      (void)o->Set(ctx, "start"_v8(iso), Integer::NewFromUnsigned(iso, r.start));
      (void)o->Set(ctx, "end"_v8(iso), Integer::NewFromUnsigned(iso, r.end));
      info.GetReturnValue().Set(o);
    }
    void td_line_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 line = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      info.GetReturnValue().Set(u16_to_v8(iso, d->line_text(line)));
    }
    void td_offset_to_line_col(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 off = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      const u32 line = d->offset_to_line(off);
      const u32 lstart = d->line_to_offset(line);
      auto o = Object::New(iso);
      (void)o->Set(ctx, "line"_v8(iso), Integer::NewFromUnsigned(iso, line));
      (void)o->Set(ctx, "col"_v8(iso), Integer::NewFromUnsigned(iso, off - lstart));
      info.GetReturnValue().Set(o);
    }
    void td_line_col_to_offset(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      const u32 line = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      const u32 col = info.Length() >= 2 ? to_u32(ctx, info[1]) : 0;
      const u32 lstart = d->line_to_offset(line);
      const auto r = d->line_range(line);
      const u32 off = std::min(lstart + col, r.end);
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, off));
    }

    // replace(start, end, text) → { start, removed, inserted, deleted }
    void td_replace(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      if (info.Length() < 3 || !info[2]->IsString()) {
        (void)throw_type_error(iso, "replace: expected (start, end, text)");
        return;
      }
      const u32 a = to_u32(ctx, info[0]);
      const u32 b = to_u32(ctx, info[1]);
      auto t = v8_to_u16(iso, info[2]);
      auto e = d->replace(a, b, t);
      info.GetReturnValue().Set(make_edit_object(iso, ctx, e));
    }

    // applyBatch(edits: Array<{ start, removed, inserted }>)
    //   → Array<{ start, removed, inserted, deleted }>
    void td_apply_batch(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      if (info.Length() < 1 || !info[0]->IsArray()) {
        (void)throw_type_error(iso, "applyBatch: expected (edits[])");
        return;
      }
      auto arr = info[0].As<Array>();
      const u32 n = arr->Length();
      std::vector<text_document_edit> in;
      in.reserve(n);
      auto k_start = "start"_v8(iso);
      auto k_removed = "removed"_v8(iso);
      auto k_inserted = "inserted"_v8(iso);
      for (u32 i = 0; i < n; ++i) {
        Local<Value> ev;
        if (!arr->Get(ctx, i).ToLocal(&ev) || !ev->IsObject())
          continue;
        auto o = ev.As<Object>();
        text_document_edit e;
        Local<Value> f;
        if (o->Get(ctx, k_start).ToLocal(&f))
          e.start = to_u32(ctx, f);
        if (o->Get(ctx, k_removed).ToLocal(&f))
          e.removed = to_u32(ctx, f);
        if (o->Get(ctx, k_inserted).ToLocal(&f) && f->IsString())
          e.inserted = v8_to_u16(iso, f);
        in.push_back(std::move(e));
      }
      try {
        auto applied = d->apply_batch(in);
        auto out = Array::New(iso, static_cast<int>(applied.size()));
        for (u32 i = 0; i < applied.size(); ++i)
          (void)out->Set(ctx, i, make_edit_object(iso, ctx, applied[i]));
        info.GetReturnValue().Set(out);
      } catch (const std::exception& ex) {
        (void)throw_range_error(iso, ex.what());
      }
    }

    void td_subscribe(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* h = unwrap_holder(info.This());
      if (!h)
        return;
      if (info.Length() < 1 || !info[0]->IsFunction()) {
        (void)throw_type_error(iso, "subscribe: expected (fn)");
        return;
      }
      auto fn = info[0].As<Function>();
      auto listener = std::make_unique<js_listener>();
      listener->iso = iso;
      listener->fn.Reset(iso, fn);
      js_listener* raw = listener.get();
      raw->id = h->doc->subscribe([iso, raw, h](std::span<const text_document_edit> edits) {
        HandleScope hs2(iso);
        auto ctx = iso->GetCurrentContext();
        Local<Function> cb = raw->fn.Get(iso);
        auto arr = Array::New(iso, static_cast<int>(edits.size()));
        for (u32 i = 0; i < edits.size(); ++i)
          (void)arr->Set(ctx, i, make_edit_object(iso, ctx, edits[i]));
        Local<Value> argv[1] = {arr};
        Local<Object> recv = h->self ? h->self->Get(iso) : ctx->Global();
        TryCatch try_catch(iso);
        (void)cb->Call(ctx, recv, 1, argv);
      });
      const auto id = raw->id;
      h->listeners.push_back(std::move(listener));
      info.GetReturnValue().Set(BigInt::NewFromUnsigned(iso, id));
    }

    void td_unsubscribe(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_holder(info.This());
      if (!h)
        return;
      if (info.Length() < 1)
        return;
      u64 id = 0;
      if (info[0]->IsBigInt()) {
        id = info[0]->ToBigInt(ctx).ToLocalChecked()->Uint64Value();
      } else {
        id = static_cast<u64>(info[0]->NumberValue(ctx).FromMaybe(0));
      }
      h->doc->unsubscribe(id);
      h->listeners.erase(std::remove_if(h->listeners.begin(), h->listeners.end(),
                                        [id](const auto& up) {
                                          if (up->id == id) {
                                            up->fn.Reset();
                                            return true;
                                          }
                                          return false;
                                        }),
                         h->listeners.end());
    }

    // searchLiteral(needle, opts?: { from, limit, caseInsensitive }) → Array<{start,end}>
    void td_search_literal(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_doc(info.This());
      if (!d)
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "searchLiteral: expected (needle, opts?)");
        return;
      }
      auto needle = v8_to_u16(iso, info[0]);
      u32 from = 0;
      u32 limit = 0xFFFFFFFFu;
      bool ci = false;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto o = info[1].As<Object>();
        Local<Value> f;
        if (o->Get(ctx, "from"_v8(iso)).ToLocal(&f))
          from = to_u32(ctx, f);
        if (o->Get(ctx, "limit"_v8(iso)).ToLocal(&f))
          limit = to_u32(ctx, f, 0xFFFFFFFFu);
        if (o->Get(ctx, "caseInsensitive"_v8(iso)).ToLocal(&f))
          ci = f->BooleanValue(iso);
      }
      auto matches = d->search_literal(needle, from, limit, ci);
      auto out = Array::New(iso, static_cast<int>(matches.size()));
      for (u32 i = 0; i < matches.size(); ++i) {
        auto o = Object::New(iso);
        (void)o->Set(ctx, "start"_v8(iso), Integer::NewFromUnsigned(iso, matches[i].start));
        (void)o->Set(ctx, "end"_v8(iso), Integer::NewFromUnsigned(iso, matches[i].end));
        (void)out->Set(ctx, i, o);
      }
      info.GetReturnValue().Set(out);
    }

#define TD_GETTER(name, fn) proto->Set(iso, name, FunctionTemplate::New(iso, fn))

  } // namespace

  void install_text_document_template(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto tpl = FunctionTemplate::New(iso, td_constructor);
    tpl->SetClassName("TextDocument"_v8(iso));
    tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto proto = tpl->PrototypeTemplate();
    TD_GETTER("length", td_length);
    TD_GETTER("lineCount", td_line_count);
    TD_GETTER("revision", td_revision);
    TD_GETTER("pieceCount", td_piece_count);
    TD_GETTER("text", td_text);
    TD_GETTER("slice", td_slice);
    TD_GETTER("charCodeAt", td_char_code_at);
    TD_GETTER("lineToOffset", td_line_to_offset);
    TD_GETTER("offsetToLine", td_offset_to_line);
    TD_GETTER("lineRange", td_line_range);
    TD_GETTER("lineText", td_line_text);
    TD_GETTER("offsetToLineCol", td_offset_to_line_col);
    TD_GETTER("lineColToOffset", td_line_col_to_offset);
    TD_GETTER("replace", td_replace);
    TD_GETTER("applyBatch", td_apply_batch);
    TD_GETTER("subscribe", td_subscribe);
    TD_GETTER("unsubscribe", td_unsubscribe);
    TD_GETTER("searchLiteral", td_search_literal);
    global->Set(iso, "TextDocument", tpl);
    td_tpl_table()[iso].Reset(iso, tpl);
  }
#undef TD_GETTER

} // namespace fxe::js
