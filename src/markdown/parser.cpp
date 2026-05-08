// fxe markdown parser — md4c SAX → fxe::markdown::node tree.
//
// The bridge keeps a stack of "currently open" containers. Block/span enter
// callbacks push a new node; leave callbacks pop. Text callbacks attach a
// leaf to the top of the stack. md4c guarantees correct nesting, so the
// stack never underflows for well-formed input.
#include <fxe/markdown.hpp>

#include <md4c.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace fxe::markdown {
  namespace {

    // Translate an md4c MD_ATTRIBUTE into a flat std::string. md4c may split
    // the attribute into several substrings (normal text vs. NULL-char vs.
    // entity). For our use cases — link/image hrefs, code-block info — the
    // raw bytes are correct: hrefs and code info are not entity-decoded by
    // md4c, and downstream consumers (browsers, our UI, tests) do their own
    // decoding when needed.
    std::string attr_to_string(const MD_ATTRIBUTE& attr) {
      if (attr.text == nullptr || attr.size == 0)
        return {};
      return std::string(attr.text, attr.size);
    }

    std::string lang_from_info(std::string_view info) {
      // Strip leading whitespace then take up to first whitespace.
      size_t i = 0;
      while (i < info.size() && std::isspace(static_cast<unsigned char>(info[i])))
        ++i;
      size_t j = i;
      while (j < info.size() && !std::isspace(static_cast<unsigned char>(info[j])))
        ++j;
      return std::string(info.substr(i, j - i));
    }

    struct builder {
      std::vector<node*> stack;
      std::unique_ptr<node> root;

      builder() {
        root = std::make_unique<node>();
        root->kind = node_kind::document;
        stack.push_back(root.get());
      }

      node* push(node_kind k) {
        auto child = std::make_unique<node>();
        child->kind = k;
        node* raw = child.get();
        stack.back()->children.push_back(std::move(child));
        stack.push_back(raw);
        return raw;
      }

      void pop() {
        if (stack.size() > 1)
          stack.pop_back();
      }

      void add_leaf(node_kind k, std::string text) {
        auto child = std::make_unique<node>();
        child->kind = k;
        child->text = std::move(text);
        stack.back()->children.push_back(std::move(child));
      }
    };

    builder& self(void* ud) {
      return *static_cast<builder*>(ud);
    }

    int on_enter_block(MD_BLOCKTYPE type, void* detail, void* ud) {
      auto& b = self(ud);
      switch (type) {
      case MD_BLOCK_DOC:
        // md4c emits a top-level DOC; we already created one. Just track it.
        return 0;
      case MD_BLOCK_QUOTE:
        b.push(node_kind::blockquote);
        return 0;
      case MD_BLOCK_UL: {
        auto* d = static_cast<MD_BLOCK_UL_DETAIL*>(detail);
        auto* n = b.push(node_kind::list);
        n->ordered = false;
        n->tight = d ? d->is_tight != 0 : true;
        return 0;
      }
      case MD_BLOCK_OL: {
        auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        auto* n = b.push(node_kind::list);
        n->ordered = true;
        n->list_start = d ? static_cast<int>(d->start) : 1;
        n->tight = d ? d->is_tight != 0 : true;
        return 0;
      }
      case MD_BLOCK_LI: {
        auto* d = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        auto* n = b.push(node_kind::list_item);
        if (d) {
          n->task = d->is_task != 0;
          n->checked = d->is_task && (d->task_mark == 'x' || d->task_mark == 'X');
        }
        return 0;
      }
      case MD_BLOCK_HR:
        b.push(node_kind::thematic_break);
        return 0;
      case MD_BLOCK_H: {
        auto* d = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        auto* n = b.push(node_kind::heading);
        n->heading_level = d ? static_cast<int>(d->level) : 1;
        return 0;
      }
      case MD_BLOCK_CODE: {
        auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        auto* n = b.push(node_kind::code_block);
        if (d) {
          n->info = attr_to_string(d->info);
          n->lang = !d->lang.text ? lang_from_info(n->info) : attr_to_string(d->lang);
        }
        return 0;
      }
      case MD_BLOCK_HTML:
        b.push(node_kind::html_block);
        return 0;
      case MD_BLOCK_P:
        b.push(node_kind::paragraph);
        return 0;
      case MD_BLOCK_TABLE:
        b.push(node_kind::table);
        return 0;
      case MD_BLOCK_THEAD:
        b.push(node_kind::table_head);
        return 0;
      case MD_BLOCK_TBODY:
        b.push(node_kind::table_body);
        return 0;
      case MD_BLOCK_TR:
        b.push(node_kind::table_row);
        return 0;
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        auto* d = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
        auto* n = b.push(node_kind::table_cell);
        if (d) {
          switch (d->align) {
          case MD_ALIGN_LEFT:
            n->align = "left";
            break;
          case MD_ALIGN_CENTER:
            n->align = "center";
            break;
          case MD_ALIGN_RIGHT:
            n->align = "right";
            break;
          default:
            break;
          }
        }
        return 0;
      }
      }
      return 0;
    }

    int on_leave_block(MD_BLOCKTYPE type, void* /*detail*/, void* ud) {
      auto& b = self(ud);
      if (type == MD_BLOCK_DOC)
        return 0;
      b.pop();
      return 0;
    }

    int on_enter_span(MD_SPANTYPE type, void* detail, void* ud) {
      auto& b = self(ud);
      switch (type) {
      case MD_SPAN_EM:
        b.push(node_kind::emph);
        return 0;
      case MD_SPAN_STRONG:
        b.push(node_kind::strong);
        return 0;
      case MD_SPAN_DEL:
        b.push(node_kind::strikethrough);
        return 0;
      case MD_SPAN_U:
        b.push(node_kind::underline);
        return 0;
      case MD_SPAN_CODE:
        b.push(node_kind::code_span);
        return 0;
      case MD_SPAN_A: {
        auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
        auto* n = b.push(node_kind::link);
        if (d) {
          n->href = attr_to_string(d->href);
          n->title = attr_to_string(d->title);
          n->autolink = d->is_autolink != 0;
        }
        return 0;
      }
      case MD_SPAN_IMG: {
        auto* d = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        auto* n = b.push(node_kind::image);
        if (d) {
          n->src = attr_to_string(d->src);
          n->title = attr_to_string(d->title);
        }
        return 0;
      }
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
        b.push(node_kind::latex_math);
        return 0;
      case MD_SPAN_WIKILINK: {
        auto* d = static_cast<MD_SPAN_WIKILINK_DETAIL*>(detail);
        auto* n = b.push(node_kind::wikilink);
        if (d)
          n->wikilink_target = attr_to_string(d->target);
        return 0;
      }
      }
      return 0;
    }

    int on_leave_span(MD_SPANTYPE /*type*/, void* /*detail*/, void* ud) {
      self(ud).pop();
      return 0;
    }

    int on_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
      auto& b = self(ud);
      std::string s(text, size);
      switch (type) {
      case MD_TEXT_NORMAL:
        b.add_leaf(node_kind::text, std::move(s));
        return 0;
      case MD_TEXT_NULLCHAR:
        b.add_leaf(node_kind::null_char, std::move(s));
        return 0;
      case MD_TEXT_BR:
        b.add_leaf(node_kind::hard_break, {});
        return 0;
      case MD_TEXT_SOFTBR:
        b.add_leaf(node_kind::soft_break, {});
        return 0;
      case MD_TEXT_ENTITY:
        b.add_leaf(node_kind::entity, std::move(s));
        return 0;
      case MD_TEXT_CODE:
        b.add_leaf(node_kind::text, std::move(s));
        return 0;
      case MD_TEXT_HTML:
        b.add_leaf(node_kind::raw_html, std::move(s));
        return 0;
      case MD_TEXT_LATEXMATH:
        b.add_leaf(node_kind::text, std::move(s));
        return 0;
      }
      return 0;
    }

    unsigned translate_flags(std::uint32_t in) {
      unsigned out = 0;
      auto bit = [&](std::uint32_t mask, unsigned md4c_flag) {
        if ((in & mask) != 0)
          out |= md4c_flag;
      };
      bit(flag_collapse_whitespace, MD_FLAG_COLLAPSEWHITESPACE);
      bit(flag_permissive_atx_headers, MD_FLAG_PERMISSIVEATXHEADERS);
      bit(flag_permissive_url_autolinks, MD_FLAG_PERMISSIVEURLAUTOLINKS);
      bit(flag_permissive_email_autolinks, MD_FLAG_PERMISSIVEEMAILAUTOLINKS);
      bit(flag_no_indented_code_blocks, MD_FLAG_NOINDENTEDCODEBLOCKS);
      bit(flag_no_html_blocks, MD_FLAG_NOHTMLBLOCKS);
      bit(flag_no_html_spans, MD_FLAG_NOHTMLSPANS);
      bit(flag_tables, MD_FLAG_TABLES);
      bit(flag_strikethrough, MD_FLAG_STRIKETHROUGH);
      bit(flag_permissive_www_autolinks, MD_FLAG_PERMISSIVEWWWAUTOLINKS);
      bit(flag_tasklists, MD_FLAG_TASKLISTS);
      bit(flag_latex_math_spans, MD_FLAG_LATEXMATHSPANS);
      bit(flag_wikilinks, MD_FLAG_WIKILINKS);
      bit(flag_underline, MD_FLAG_UNDERLINE);
      // hard_soft_breaks: md4c always emits softbr/br callbacks; this flag is
      // a no-op at parse time and is documented as a hint for renderers.
      return out;
    }

  } // namespace

  std::unique_ptr<node> parse(std::string_view source, const parse_options& opts) {
    builder b;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = translate_flags(opts.flags);
    parser.enter_block = &on_enter_block;
    parser.leave_block = &on_leave_block;
    parser.enter_span = &on_enter_span;
    parser.leave_span = &on_leave_span;
    parser.text = &on_text;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, &b);
    return std::move(b.root);
  }

  std::string_view kind_tag(node_kind kind) {
    switch (kind) {
    case node_kind::document:
      return "document";
    case node_kind::paragraph:
      return "paragraph";
    case node_kind::heading:
      return "heading";
    case node_kind::blockquote:
      return "blockquote";
    case node_kind::list:
      return "list";
    case node_kind::list_item:
      return "list_item";
    case node_kind::code_block:
      return "code_block";
    case node_kind::html_block:
      return "html_block";
    case node_kind::thematic_break:
      return "thematic_break";
    case node_kind::table:
      return "table";
    case node_kind::table_head:
      return "table_head";
    case node_kind::table_body:
      return "table_body";
    case node_kind::table_row:
      return "table_row";
    case node_kind::table_cell:
      return "table_cell";
    case node_kind::emph:
      return "emph";
    case node_kind::strong:
      return "strong";
    case node_kind::strikethrough:
      return "strikethrough";
    case node_kind::underline:
      return "underline";
    case node_kind::code_span:
      return "code_span";
    case node_kind::link:
      return "link";
    case node_kind::image:
      return "image";
    case node_kind::latex_math:
      return "latex_math";
    case node_kind::wikilink:
      return "wikilink";
    case node_kind::raw_html:
      return "raw_html";
    case node_kind::text:
      return "text";
    case node_kind::soft_break:
      return "soft_break";
    case node_kind::hard_break:
      return "hard_break";
    case node_kind::entity:
      return "entity";
    case node_kind::null_char:
      return "null_char";
    }
    return "unknown";
  }

} // namespace fxe::markdown
