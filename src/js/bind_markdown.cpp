// JS binding for fxe::markdown — exposes a `Markdown` global namespace with:
//
//   Markdown.parse(source, opts?) → DocumentNode
//
// `opts` is `{ dialect?: 'commonmark' | 'github', flags?: number }`. The
// returned tree is a plain JS object graph: each node has `{ type, children?,
// ...kindSpecificFields }`. Text nodes are leaves carrying `text`. The shape
// mirrors `fxe::markdown::node` 1:1 — see `include/fxe/markdown.hpp` and
// `types/fxe.d.ts`.
#include "bind_markdown.hpp"

#include <fxe/markdown.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_strings.hpp>

#include <string>
#include <string_view>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;
    namespace md = fxe::markdown;

    Local<String> v8_str(Isolate* iso, std::string_view s) {
      return String::NewFromUtf8(iso, s.data(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    // Build a JS object for `n`, recursing into children. Container kinds
    // include a `children` array; leaf text kinds carry their `text` field.
    Local<Object> node_to_v8(Isolate* iso, Local<Context> ctx, const md::node& n) {
      EscapableHandleScope hs(iso);
      auto obj = Object::New(iso);
      auto set = [&](Local<String> key, Local<Value> val) { (void)obj->Set(ctx, key, val); };

      set("type"_v8(iso), v8_str(iso, md::kind_tag(n.kind)));

      switch (n.kind) {
      case md::node_kind::heading:
        set("level"_v8(iso), Integer::New(iso, n.heading_level));
        break;
      case md::node_kind::list:
        set("ordered"_v8(iso), Boolean::New(iso, n.ordered));
        set("tight"_v8(iso), Boolean::New(iso, n.tight));
        if (n.ordered)
          set("start"_v8(iso), Integer::New(iso, n.list_start));
        break;
      case md::node_kind::list_item:
        if (n.task) {
          set("task"_v8(iso), Boolean::New(iso, true));
          set("checked"_v8(iso), Boolean::New(iso, n.checked));
        }
        break;
      case md::node_kind::code_block:
        set("info"_v8(iso), v8_str(iso, n.info));
        set("lang"_v8(iso), v8_str(iso, n.lang));
        break;
      case md::node_kind::link:
        set("href"_v8(iso), v8_str(iso, n.href));
        if (!n.title.empty())
          set("title"_v8(iso), v8_str(iso, n.title));
        if (n.autolink)
          set("autolink"_v8(iso), Boolean::New(iso, true));
        break;
      case md::node_kind::image:
        set("src"_v8(iso), v8_str(iso, n.src));
        if (!n.title.empty())
          set("title"_v8(iso), v8_str(iso, n.title));
        break;
      case md::node_kind::wikilink:
        set("target"_v8(iso), v8_str(iso, n.wikilink_target));
        break;
      case md::node_kind::table_cell:
        if (!n.align.empty())
          set("align"_v8(iso), v8_str(iso, n.align));
        break;
      case md::node_kind::text:
      case md::node_kind::entity:
      case md::node_kind::raw_html:
      case md::node_kind::html_block:
      case md::node_kind::null_char:
        set("text"_v8(iso), v8_str(iso, n.text));
        break;
      default:
        break;
      }

      // Container kinds get a children array. Pure-leaf text kinds skip it
      // for ergonomics on the JS side (a renderer can `if (n.children)`).
      const bool is_leaf =
          n.kind == md::node_kind::text || n.kind == md::node_kind::entity ||
          n.kind == md::node_kind::raw_html || n.kind == md::node_kind::null_char ||
          n.kind == md::node_kind::soft_break || n.kind == md::node_kind::hard_break ||
          n.kind == md::node_kind::thematic_break || n.kind == md::node_kind::html_block ||
          n.kind == md::node_kind::latex_math;
      if (!is_leaf || !n.children.empty()) {
        auto arr = Array::New(iso, static_cast<int>(n.children.size()));
        for (uint32_t i = 0; i < n.children.size(); ++i) {
          (void)arr->Set(ctx, i, node_to_v8(iso, ctx, *n.children[i]));
        }
        set("children"_v8(iso), arr);
      }

      return hs.Escape(obj);
    }

    bool read_opts(Isolate* iso, Local<Context> ctx, Local<Value> v, md::parse_options& out) {
      if (v.IsEmpty() || v->IsUndefined() || v->IsNull())
        return true;
      if (!v->IsObject()) {
        return throw_type_error(iso, "Markdown.parse: opts must be an object");
      }
      auto o = v.As<Object>();
      Local<Value> dialect_v;
      if (o->Get(ctx, "dialect"_v8(iso)).ToLocal(&dialect_v) && !dialect_v->IsUndefined() &&
          !dialect_v->IsNull()) {
        if (!dialect_v->IsString())
          return throw_type_error(iso, "Markdown.parse: opts.dialect must be a string");
        String::Utf8Value u(iso, dialect_v);
        std::string_view sv(*u ? *u : "", *u ? static_cast<size_t>(u.length()) : 0);
        if (sv == "commonmark") {
          out.flags = md::dialect_commonmark;
        } else if (sv == "github" || sv == "gfm") {
          out.flags = md::dialect_github;
        } else {
          return throw_type_error(iso,
                                  "Markdown.parse: opts.dialect must be 'commonmark' or 'github'");
        }
      }
      Local<Value> flags_v;
      if (o->Get(ctx, "flags"_v8(iso)).ToLocal(&flags_v) && !flags_v->IsUndefined() &&
          !flags_v->IsNull()) {
        // Explicit flags override the dialect baseline.
        out.flags = flags_v->Uint32Value(ctx).FromMaybe(out.flags);
      }
      return true;
    }

    void md_parse(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "Markdown.parse(source: string, opts?)");
        return;
      }
      String::Utf8Value src(iso, info[0]);
      std::string_view source(*src ? *src : "", *src ? static_cast<size_t>(src.length()) : 0);
      md::parse_options opts;
      if (info.Length() >= 2 && !read_opts(iso, ctx, info[1], opts))
        return;

      auto tree = md::parse(source, opts);
      if (!tree) {
        (void)throw_error(iso, "Markdown.parse: parser returned null");
        return;
      }
      info.GetReturnValue().Set(node_to_v8(iso, ctx, *tree));
    }

    // Markdown.FLAG_* mirrors fxe::markdown::parse_flags so JS callers can
    // assemble a custom flag set without depending on magic numbers.
    void install_flag_constants(Isolate* iso, Local<ObjectTemplate> ns) {
      auto add = [&](const char* name, uint32_t value) {
        ns->Set(iso, name, Integer::NewFromUnsigned(iso, value));
      };
      add("FLAG_COLLAPSE_WHITESPACE", md::flag_collapse_whitespace);
      add("FLAG_PERMISSIVE_ATX_HEADERS", md::flag_permissive_atx_headers);
      add("FLAG_PERMISSIVE_URL_AUTOLINKS", md::flag_permissive_url_autolinks);
      add("FLAG_PERMISSIVE_EMAIL_AUTOLINKS", md::flag_permissive_email_autolinks);
      add("FLAG_NO_INDENTED_CODE_BLOCKS", md::flag_no_indented_code_blocks);
      add("FLAG_NO_HTML_BLOCKS", md::flag_no_html_blocks);
      add("FLAG_NO_HTML_SPANS", md::flag_no_html_spans);
      add("FLAG_TABLES", md::flag_tables);
      add("FLAG_STRIKETHROUGH", md::flag_strikethrough);
      add("FLAG_PERMISSIVE_WWW_AUTOLINKS", md::flag_permissive_www_autolinks);
      add("FLAG_TASKLISTS", md::flag_tasklists);
      add("FLAG_LATEX_MATH_SPANS", md::flag_latex_math_spans);
      add("FLAG_WIKILINKS", md::flag_wikilinks);
      add("FLAG_UNDERLINE", md::flag_underline);
      add("DIALECT_COMMONMARK", md::dialect_commonmark);
      add("DIALECT_GITHUB", md::dialect_github);
    }

  } // namespace

  void install_markdown_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "parse", FunctionTemplate::New(iso, md_parse));
    install_flag_constants(iso, ns);
    global->Set(iso, "Markdown", ns);
  }

} // namespace fxe::js
