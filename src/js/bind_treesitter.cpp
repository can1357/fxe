// V8 bindings for fxe::treesitter — exposes a `Treesitter` namespace global
// with three constructors (`Parser`, `Tree`, `Query`), an opaque `Node`
// value-type (read-only methods), and an `availableLanguages()` helper.
//
// Usage from JS:
//
//   const p = new Treesitter.Parser('typescript');
//   const tree = p.parse(doc.text());
//   const root = tree.rootNode();
//   const q = new Treesitter.Query('typescript',
//     '(identifier) @name (string) @str');
//   for (const cap of q.captures(root)) {
//     console.log(cap.name, cap.startByte, cap.endByte);
//   }
//
// Each Tree owns the underlying TSTree; GC finalisers free it. Nodes hold a
// borrowed reference to their tree's storage and become invalid if the tree
// is finalised — usual editor-tool tradeoff.

#ifdef FXE_HAS_TREESITTER

#include "bind_treesitter.hpp"
#include <fxe/js_bindings.hpp>
#include <fxe/treesitter.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_strings.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_TS_PARSER = 0x54534150u; // 'TSAP'
    constexpr u32 TAG_TS_TREE = 0x54535452u;   // 'TSTR'
    constexpr u32 TAG_TS_NODE = 0x54534E44u;   // 'TSND'
    constexpr u32 TAG_TS_QUERY = 0x5453514Eu;  // 'TSQN'

    using TplGlobal = Global<FunctionTemplate>;
    template <int Tag> std::unordered_map<Isolate*, TplGlobal>& tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void reset_for_isolate(Isolate* iso) {
      auto reset = [iso](auto& tbl) {
        auto it = tbl.find(iso);
        if (it != tbl.end()) {
          it->second.Reset();
          tbl.erase(it);
        }
      };
      reset(tpl_table<0>());
      reset(tpl_table<1>());
      reset(tpl_table<2>());
      reset(tpl_table<3>());
    }
    struct ts_resetter_register {
      ts_resetter_register() {
        register_template_resetter(&reset_for_isolate);
      }
    };
    static ts_resetter_register s_resetter;

    // ---------------- Holders ----------------
    struct parser_holder {
      treesitter::parser p;
      Global<Object> self;
    };
    struct tree_holder {
      treesitter::tree t;
      Global<Object> self;
      std::string text; // copy so node text() can return slices
    };
    struct query_holder {
      std::unique_ptr<treesitter::query> q;
      Global<Object> self;
      const TSLanguage* lang = nullptr;
    };
    struct node_holder {
      treesitter::node n;
      Global<Object> tree_ref; // keeps the tree alive
      Global<Object> self;
    };

    void parser_finalizer(const WeakCallbackInfo<parser_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }
    void tree_finalizer(const WeakCallbackInfo<tree_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }
    void query_finalizer(const WeakCallbackInfo<query_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }
    void node_finalizer(const WeakCallbackInfo<node_holder>& info) {
      auto* h = info.GetParameter();
      h->tree_ref.Reset();
      h->self.Reset();
      delete h;
    }

    parser_holder* unwrap_parser(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      return static_cast<parser_holder*>(unwrap(v.As<Object>(), TAG_TS_PARSER));
    }
    tree_holder* unwrap_tree(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      return static_cast<tree_holder*>(unwrap(v.As<Object>(), TAG_TS_TREE));
    }
    query_holder* unwrap_query(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      return static_cast<query_holder*>(unwrap(v.As<Object>(), TAG_TS_QUERY));
    }
    node_holder* unwrap_node(Local<Value> v) {
      if (!v->IsObject())
        return nullptr;
      return static_cast<node_holder*>(unwrap(v.As<Object>(), TAG_TS_NODE));
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }
    u32 to_u32(Local<Context> ctx, Local<Value> v, u32 def = 0) {
      return v->Uint32Value(ctx).FromMaybe(def);
    }

    Local<Object> wrap_node(Isolate* iso, Local<Context> ctx, treesitter::node n,
                            Local<Object> tree_ref);

    // ---------------- Parser ----------------
    void parser_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Treesitter.Parser must be invoked with new");
        return;
      }
      HandleScope hs(iso);
      auto self = info.This();
      auto* h = new parser_holder();
      if (info.Length() >= 1 && info[0]->IsString()) {
        std::string lang = utf8(iso, info[0]);
        if (!h->p.set_language_by_name(lang)) {
          delete h;
          (void)throw_range_error(iso, "Treesitter.Parser: unknown language '{}'", lang);
          return;
        }
      }
      set_native(iso, self, h, TAG_TS_PARSER);
      h->self.Reset(iso, self);
      h->self.SetWeak(h, parser_finalizer, WeakCallbackType::kParameter);
    }

    void parser_set_language(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_parser(info.This());
      if (!h)
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "setLanguage: expected (name)");
        return;
      }
      std::string lang = utf8(iso, info[0]);
      info.GetReturnValue().Set(h->p.set_language_by_name(lang));
    }

    void parser_parse(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_parser(info.This());
      if (!h)
        return;
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "parse: expected (text, prevTree?)");
        return;
      }
      std::string text = utf8(iso, info[0]);
      const treesitter::tree* prev = nullptr;
      if (info.Length() >= 2 && !info[1]->IsNullOrUndefined()) {
        auto* th = unwrap_tree(info[1]);
        if (!th) {
          (void)throw_type_error(iso, "parse: prevTree must be a Tree");
          return;
        }
        prev = &th->t;
      }
      treesitter::tree t = h->p.parse(text, prev);
      if (!t.is_valid()) {
        info.GetReturnValue().SetNull();
        return;
      }
      // Wrap.
      auto& tbl = tpl_table<1>();
      auto tpl = tbl[iso].Get(iso);
      auto inst = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      auto* th = new tree_holder();
      th->t = std::move(t);
      th->text = std::move(text);
      set_native(iso, inst, th, TAG_TS_TREE);
      th->self.Reset(iso, inst);
      th->self.SetWeak(th, tree_finalizer, WeakCallbackType::kParameter);
      info.GetReturnValue().Set(inst);
    }

    // ---------------- Tree ----------------
    void tree_ctor(const FunctionCallbackInfo<Value>& info) {
      (void)throw_type_error(info.GetIsolate(), "Treesitter.Tree is not directly constructible");
    }

    void tree_root(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_tree(info.This());
      if (!h)
        return;
      info.GetReturnValue().Set(wrap_node(iso, ctx, h->t.root(), info.This().As<Object>()));
    }

    void tree_edit(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_tree(info.This());
      if (!h)
        return;
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "edit: expected ({startByte, oldEndByte, newEndByte, "
                                    "startPoint, oldEndPoint, newEndPoint})");
        return;
      }
      auto o = info[0].As<Object>();
      auto get = [&](Local<Name> k) {
        Local<Value> v;
        return o->Get(ctx, k).ToLocal(&v) ? v : Local<Value>(Local<Value>::Cast(Undefined(iso)));
      };
      auto get_point = [&](Local<Name> k) -> treesitter::point {
        Local<Value> v;
        if (!o->Get(ctx, k).ToLocal(&v) || !v->IsObject())
          return {};
        auto p = v.As<Object>();
        Local<Value> r;
        Local<Value> c;
        u32 row = p->Get(ctx, "row"_v8(iso)).ToLocal(&r) ? to_u32(ctx, r) : 0;
        u32 col = p->Get(ctx, "column"_v8(iso)).ToLocal(&c) ? to_u32(ctx, c) : 0;
        return {row, col};
      };
      treesitter::edit_descriptor d;
      d.start_byte = to_u32(ctx, get("startByte"_v8(iso)));
      d.old_end_byte = to_u32(ctx, get("oldEndByte"_v8(iso)));
      d.new_end_byte = to_u32(ctx, get("newEndByte"_v8(iso)));
      d.start_point = get_point("startPoint"_v8(iso));
      d.old_end_point = get_point("oldEndPoint"_v8(iso));
      d.new_end_point = get_point("newEndPoint"_v8(iso));
      h->t.edit(d);
    }

    // ---------------- Node ----------------
    Local<Object> wrap_node(Isolate* iso, Local<Context> ctx, treesitter::node n,
                            Local<Object> tree_ref) {
      auto& tbl = tpl_table<2>();
      auto tpl = tbl[iso].Get(iso);
      auto inst = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      auto* h = new node_holder();
      h->n = n;
      h->tree_ref.Reset(iso, tree_ref);
      set_native(iso, inst, h, TAG_TS_NODE);
      h->self.Reset(iso, inst);
      h->self.SetWeak(h, node_finalizer, WeakCallbackType::kParameter);
      return inst;
    }

    void node_ctor(const FunctionCallbackInfo<Value>& info) {
      (void)throw_type_error(info.GetIsolate(), "Treesitter.Node is not directly constructible");
    }

    void node_kind(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      auto k = h->n.kind();
      info.GetReturnValue().Set(
          String::NewFromUtf8(iso, k.data(), NewStringType::kNormal, static_cast<int>(k.size()))
              .ToLocalChecked());
    }
    void node_is_named(const FunctionCallbackInfo<Value>& info) {
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      info.GetReturnValue().Set(h->n.is_named());
    }
    void node_start_byte(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h->n.start_byte()));
    }
    void node_end_byte(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h->n.end_byte()));
    }
    void node_point(Isolate* iso, Local<Context> ctx, Local<Object>& dst, treesitter::point p) {
      auto o = Object::New(iso);
      (void)o->Set(ctx, "row"_v8(iso), Integer::NewFromUnsigned(iso, p.row));
      (void)o->Set(ctx, "column"_v8(iso), Integer::NewFromUnsigned(iso, p.column));
      dst = o;
    }
    void node_start_point(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      Local<Object> o;
      node_point(iso, ctx, o, h->n.start_point());
      info.GetReturnValue().Set(o);
    }
    void node_end_point(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      Local<Object> o;
      node_point(iso, ctx, o, h->n.end_point());
      info.GetReturnValue().Set(o);
    }
    void node_child_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h->n.child_count()));
    }
    void node_child(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      const u32 i = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      auto child = h->n.child(i);
      if (child.is_null()) {
        info.GetReturnValue().SetNull();
        return;
      }
      Local<Object> tree_ref = h->tree_ref.Get(iso);
      info.GetReturnValue().Set(wrap_node(iso, ctx, child, tree_ref));
    }
    void node_named_child_count(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, h->n.named_child_count()));
    }
    void node_named_child(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      const u32 i = info.Length() >= 1 ? to_u32(ctx, info[0]) : 0;
      auto child = h->n.named_child(i);
      if (child.is_null()) {
        info.GetReturnValue().SetNull();
        return;
      }
      Local<Object> tree_ref = h->tree_ref.Get(iso);
      info.GetReturnValue().Set(wrap_node(iso, ctx, child, tree_ref));
    }
    void node_parent(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_node(info.This());
      if (!h)
        return;
      auto p = h->n.parent();
      if (p.is_null()) {
        info.GetReturnValue().SetNull();
        return;
      }
      Local<Object> tree_ref = h->tree_ref.Get(iso);
      info.GetReturnValue().Set(wrap_node(iso, ctx, p, tree_ref));
    }

    // ---------------- Query ----------------
    void query_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        (void)throw_type_error(iso, "Treesitter.Query must be invoked with new");
        return;
      }
      HandleScope hs(iso);
      auto self = info.This();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "Treesitter.Query: expected (language, source)");
        return;
      }
      std::string lang_name = utf8(iso, info[0]);
      const TSLanguage* lang = treesitter::language_by_name(lang_name);
      if (!lang) {
        (void)throw_range_error(iso, "Treesitter.Query: unknown language '{}'", lang_name);
        return;
      }
      std::string source = utf8(iso, info[1]);
      auto* h = new query_holder();
      h->lang = lang;
      try {
        h->q = std::make_unique<treesitter::query>(lang, source);
      } catch (const std::exception& ex) {
        delete h;
        (void)throw_range_error(iso, ex.what());
        return;
      }
      set_native(iso, self, h, TAG_TS_QUERY);
      h->self.Reset(iso, self);
      h->self.SetWeak(h, query_finalizer, WeakCallbackType::kParameter);
    }

    // captures(node, opts?: { startByte, endByte, limit })
    //   → Array<{ name, kind, startByte, endByte, startPoint, endPoint, node }>
    void query_captures(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_query(info.This());
      if (!h)
        return;
      if (info.Length() < 1 || !info[0]->IsObject()) {
        (void)throw_type_error(iso, "captures: expected (node, opts?)");
        return;
      }
      auto* nh = unwrap_node(info[0]);
      if (!nh) {
        (void)throw_type_error(iso, "captures: first arg must be a Node");
        return;
      }
      u32 start_byte = 0;
      u32 end_byte = 0xFFFFFFFFu;
      u32 limit = 0xFFFFFFFFu;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto o = info[1].As<Object>();
        Local<Value> f;
        if (o->Get(ctx, "startByte"_v8(iso)).ToLocal(&f))
          start_byte = to_u32(ctx, f);
        if (o->Get(ctx, "endByte"_v8(iso)).ToLocal(&f))
          end_byte = to_u32(ctx, f, 0xFFFFFFFFu);
        if (o->Get(ctx, "limit"_v8(iso)).ToLocal(&f))
          limit = to_u32(ctx, f, 0xFFFFFFFFu);
      }
      auto out = Array::New(iso);
      u32 idx = 0;
      Local<Object> tree_ref = nh->tree_ref.Get(iso);
      h->q->run(nh->n, start_byte, end_byte, [&](const treesitter::query::capture& cap) {
        auto entry = Object::New(iso);
        (void)entry->Set(ctx, "name"_v8(iso),
                         String::NewFromUtf8(iso, cap.name.data(), NewStringType::kNormal,
                                             static_cast<int>(cap.name.size()))
                             .ToLocalChecked());
        auto kind = cap.n.kind();
        (void)entry->Set(ctx, "kind"_v8(iso),
                         String::NewFromUtf8(iso, kind.data(), NewStringType::kNormal,
                                             static_cast<int>(kind.size()))
                             .ToLocalChecked());
        (void)entry->Set(ctx, "startByte"_v8(iso),
                         Integer::NewFromUnsigned(iso, cap.n.start_byte()));
        (void)entry->Set(ctx, "endByte"_v8(iso), Integer::NewFromUnsigned(iso, cap.n.end_byte()));
        Local<Object> sp;
        Local<Object> ep;
        node_point(iso, ctx, sp, cap.n.start_point());
        node_point(iso, ctx, ep, cap.n.end_point());
        (void)entry->Set(ctx, "startPoint"_v8(iso), sp);
        (void)entry->Set(ctx, "endPoint"_v8(iso), ep);
        (void)entry->Set(ctx, "node"_v8(iso), wrap_node(iso, ctx, cap.n, tree_ref));
        (void)out->Set(ctx, idx++, entry);
        return idx < limit;
      });
      info.GetReturnValue().Set(out);
    }

    // ---------------- Namespace setup ----------------

    void ns_available_languages(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto langs = treesitter::available_languages();
      auto out = Array::New(iso, static_cast<int>(langs.size()));
      for (u32 i = 0; i < langs.size(); ++i) {
        (void)out->Set(ctx, i,
                       String::NewFromUtf8(iso, langs[i].data(), NewStringType::kNormal,
                                           static_cast<int>(langs[i].size()))
                           .ToLocalChecked());
      }
      info.GetReturnValue().Set(out);
    }
  } // namespace

  void install_treesitter_namespace(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    // Parser class
    auto parser_tpl = FunctionTemplate::New(iso, parser_ctor);
    parser_tpl->SetClassName("Parser"_v8(iso));
    parser_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto parser_proto = parser_tpl->PrototypeTemplate();
    parser_proto->Set(iso, "setLanguage", FunctionTemplate::New(iso, parser_set_language));
    parser_proto->Set(iso, "parse", FunctionTemplate::New(iso, parser_parse));
    tpl_table<0>()[iso].Reset(iso, parser_tpl);

    // Tree class (no public ctor — throws on `new`)
    auto tree_tpl = FunctionTemplate::New(iso, tree_ctor);
    tree_tpl->SetClassName("Tree"_v8(iso));
    tree_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto tree_proto = tree_tpl->PrototypeTemplate();
    tree_proto->Set(iso, "rootNode", FunctionTemplate::New(iso, tree_root));
    tree_proto->Set(iso, "edit", FunctionTemplate::New(iso, tree_edit));
    tpl_table<1>()[iso].Reset(iso, tree_tpl);

    // Node class
    auto node_tpl = FunctionTemplate::New(iso, node_ctor);
    node_tpl->SetClassName("Node"_v8(iso));
    node_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto node_proto = node_tpl->PrototypeTemplate();
    node_proto->Set(iso, "kind", FunctionTemplate::New(iso, node_kind));
    node_proto->Set(iso, "isNamed", FunctionTemplate::New(iso, node_is_named));
    node_proto->Set(iso, "startByte", FunctionTemplate::New(iso, node_start_byte));
    node_proto->Set(iso, "endByte", FunctionTemplate::New(iso, node_end_byte));
    node_proto->Set(iso, "startPoint", FunctionTemplate::New(iso, node_start_point));
    node_proto->Set(iso, "endPoint", FunctionTemplate::New(iso, node_end_point));
    node_proto->Set(iso, "childCount", FunctionTemplate::New(iso, node_child_count));
    node_proto->Set(iso, "child", FunctionTemplate::New(iso, node_child));
    node_proto->Set(iso, "namedChildCount", FunctionTemplate::New(iso, node_named_child_count));
    node_proto->Set(iso, "namedChild", FunctionTemplate::New(iso, node_named_child));
    node_proto->Set(iso, "parent", FunctionTemplate::New(iso, node_parent));
    tpl_table<2>()[iso].Reset(iso, node_tpl);

    // Query class
    auto query_tpl = FunctionTemplate::New(iso, query_ctor);
    query_tpl->SetClassName("Query"_v8(iso));
    query_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto query_proto = query_tpl->PrototypeTemplate();
    query_proto->Set(iso, "captures", FunctionTemplate::New(iso, query_captures));
    tpl_table<3>()[iso].Reset(iso, query_tpl);

    // Treesitter namespace object
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "Parser", parser_tpl);
    ns->Set(iso, "Tree", tree_tpl);
    ns->Set(iso, "Node", node_tpl);
    ns->Set(iso, "Query", query_tpl);
    ns->Set(iso, "availableLanguages", FunctionTemplate::New(iso, ns_available_languages));
    global->Set(iso, "Treesitter", ns);
  }
} // namespace fxe::js

#endif // FXE_HAS_TREESITTER
