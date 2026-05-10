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
#include <fxe/v8_literals.hpp>
#if FXE_HAS_TREESITTER
#include <fxe/highlight.hpp>
#include <fxe/types.hpp>
#endif

#include <algorithm>
#include <string>
#include <string_view>
#include <v8.h>
#include <vector>

namespace fxe::js {
  namespace {
    using namespace v8;
    namespace md = fxe::markdown;

#if FXE_HAS_TREESITTER
    std::vector<u32> utf8_byte_to_utf16_units(std::string_view s) {
      std::vector<u32> out(s.size() + 1, 0);
      u32 units = 0;
      usize i = 0;
      while (i < s.size()) {
        out[i] = units;
        const auto c0 = static_cast<unsigned char>(s[i]);
        usize width = 1;
        u32 cp = c0;
        if ((c0 & 0xe0u) == 0xc0u && i + 1 < s.size()) {
          width = 2;
          cp = ((c0 & 0x1fu) << 6u) | (static_cast<unsigned char>(s[i + 1]) & 0x3fu);
        } else if ((c0 & 0xf0u) == 0xe0u && i + 2 < s.size()) {
          width = 3;
          cp = ((c0 & 0x0fu) << 12u) | ((static_cast<unsigned char>(s[i + 1]) & 0x3fu) << 6u) |
               (static_cast<unsigned char>(s[i + 2]) & 0x3fu);
        } else if ((c0 & 0xf8u) == 0xf0u && i + 3 < s.size()) {
          width = 4;
          cp = ((c0 & 0x07u) << 18u) | ((static_cast<unsigned char>(s[i + 1]) & 0x3fu) << 12u) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3fu) << 6u) |
               (static_cast<unsigned char>(s[i + 3]) & 0x3fu);
        }
        for (usize j = 1; j < width && i + j < s.size(); ++j)
          out[i + j] = units;
        i += width;
        units += cp > 0xffffu ? 2u : 1u;
      }
      out[s.size()] = units;
      return out;
    }
#endif

    // Build a JS object for `n`, recursing into children. Container kinds
    // include a `children` array; leaf text kinds carry their `text` field.
    Local<Object> node_to_v8(Isolate* iso, Local<Context> ctx, const md::node& n) {
      EscapableHandleScope hs(iso);
      auto obj = Object::New(iso);
      set_prop(ctx, obj, "type"_v8, md::kind_tag(n.kind));

      switch (n.kind) {
      case md::node_kind::heading:
        set_prop(ctx, obj, "level"_v8, n.heading_level);
        break;
      case md::node_kind::list:
        set_prop(ctx, obj, "ordered"_v8, n.ordered);
        set_prop(ctx, obj, "tight"_v8, n.tight);
        if (n.ordered)
          set_prop(ctx, obj, "start"_v8, n.list_start);
        break;
      case md::node_kind::list_item:
        if (n.task) {
          set_prop(ctx, obj, "task"_v8, true);
          set_prop(ctx, obj, "checked"_v8, n.checked);
        }
        break;
      case md::node_kind::code_block:
        set_prop(ctx, obj, "info"_v8, n.info);
        set_prop(ctx, obj, "lang"_v8, n.lang);
        break;
      case md::node_kind::link:
        set_prop(ctx, obj, "href"_v8, n.href);
        if (!n.title.empty())
          set_prop(ctx, obj, "title"_v8, n.title);
        if (n.autolink)
          set_prop(ctx, obj, "autolink"_v8, true);
        break;
      case md::node_kind::image:
        set_prop(ctx, obj, "src"_v8, n.src);
        if (!n.title.empty())
          set_prop(ctx, obj, "title"_v8, n.title);
        break;
      case md::node_kind::wikilink:
        set_prop(ctx, obj, "target"_v8, n.wikilink_target);
        break;
      case md::node_kind::table_cell:
        if (!n.align.empty())
          set_prop(ctx, obj, "align"_v8, n.align);
        break;
      case md::node_kind::text:
      case md::node_kind::entity:
      case md::node_kind::raw_html:
      case md::node_kind::html_block:
      case md::node_kind::null_char:
        set_prop(ctx, obj, "text"_v8, n.text);
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
        for (uint32_t i = 0; i < n.children.size(); ++i)
          set_index(ctx, arr, i, node_to_v8(iso, ctx, *n.children[i]));
        set_prop(ctx, obj, "children"_v8, arr);
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
      if (auto dialect_v = get_prop<Local<Value>>(ctx, o, "dialect"_v8(iso));
          dialect_v && !(*dialect_v)->IsUndefined() && !(*dialect_v)->IsNull()) {
        if (!(*dialect_v)->IsString())
          return throw_type_error(iso, "Markdown.parse: opts.dialect must be a string");
        const std::string sv = to_std_string(iso, *dialect_v);
        if (sv == "commonmark") {
          out.flags = md::dialect_commonmark;
        } else if (sv == "github" || sv == "gfm") {
          out.flags = md::dialect_github;
        } else {
          return throw_type_error(iso,
                                  "Markdown.parse: opts.dialect must be 'commonmark' or 'github'");
        }
      }
      if (auto flags_v = get_prop<Local<Value>>(ctx, o, "flags"_v8(iso));
          flags_v && !(*flags_v)->IsUndefined() && !(*flags_v)->IsNull()) {
        // Explicit flags override the dialect baseline.
        out.flags = (*flags_v)->Uint32Value(ctx).FromMaybe(out.flags);
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
      const std::string source_storage = to_std_string(iso, info[0]);
      std::string_view source = source_storage;
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
    void install_flag_constants(Isolate* iso, Local<Context> ctx, Local<Object> ns) {
      auto add = [&](const char* name, u32 value) {
        (void)ns->Set(ctx, to_v8_string(iso, name), to_v8(iso, value));
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

    // Markdown.highlight(source, language) — returns a non-overlapping list
    // of `{start, end, name}` token spans produced by the built-in
    // tree-sitter highlight query for `language`. Returned offsets are
    // JavaScript UTF-16 code-unit indices, so callers can use `slice()` on the
    // original string even when source contains non-ASCII text before a token.
    // Capture names are tree-sitter style ("comment", "string", "number",
    // "constant", "keyword", "type", "function", "property"); the renderer
    // maps these to theme colors.
    void md_highlight(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString()) {
        (void)throw_type_error(iso, "Markdown.highlight(source: string, language: string)");
        return;
      }
      const std::string src_storage = to_std_string(iso, info[0]);
      const std::string lang_storage = to_std_string(iso, info[1]);
#if FXE_HAS_TREESITTER
      std::string_view src_view = src_storage;
      std::string_view lang_view = lang_storage;
      auto r = fxe::highlight::tokenize(src_view, lang_view);
      if (!r) {
        info.GetReturnValue().SetNull();
        return;
      }
      const auto byte_to_utf16 = utf8_byte_to_utf16_units(src_view);
      auto out = Object::New(iso);
      set_prop(ctx, out, "language"_v8, r->language);
      auto arr = Array::New(iso, static_cast<int>(r->tokens.size()));
      for (uint32_t i = 0; i < r->tokens.size(); ++i) {
        auto t = Object::New(iso);
        const auto start = byte_to_utf16[std::min<usize>(r->tokens[i].start, src_view.size())];
        const auto end = byte_to_utf16[std::min<usize>(r->tokens[i].end, src_view.size())];
        set_prop(ctx, t, "start"_v8, start);
        set_prop(ctx, t, "end"_v8, end);
        set_prop(ctx, t, "name"_v8, r->tokens[i].name);
        set_index(ctx, arr, i, t);
      }
      set_prop(ctx, out, "tokens"_v8, arr);
      info.GetReturnValue().Set(out);
#else
      (void)src_storage;
      (void)lang_storage;
      info.GetReturnValue().SetNull();
#endif
    }

    void md_highlight_languages(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
#if FXE_HAS_TREESITTER
      auto langs = fxe::highlight::supported_languages();
      auto arr = Array::New(iso, static_cast<int>(langs.size()));
      for (uint32_t i = 0; i < langs.size(); ++i)
        set_index(ctx, arr, i, langs[i]);
      info.GetReturnValue().Set(arr);
#else
      (void)ctx;
      info.GetReturnValue().Set(Array::New(iso, 0));
#endif
    }

    void markdown_namespace_getter(Local<Name> /*name*/, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto ns = Object::New(iso);
      (void)ns->Set(ctx, "parse"_v8(iso), Function::New(ctx, md_parse).ToLocalChecked());
      (void)ns->Set(ctx, "highlight"_v8(iso), Function::New(ctx, md_highlight).ToLocalChecked());
      (void)ns->Set(ctx, "highlightLanguages"_v8(iso),
                    Function::New(ctx, md_highlight_languages).ToLocalChecked());
      install_flag_constants(iso, ctx, ns);
      info.GetReturnValue().Set(ns);
    }
  } // namespace

  void install_markdown_global(Isolate* iso, Local<ObjectTemplate> global) {
    global->SetLazyDataProperty("Markdown"_v8(iso), markdown_namespace_getter);
  }

} // namespace fxe::js
