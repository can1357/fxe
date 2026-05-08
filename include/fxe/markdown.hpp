// Public C++ surface for the fxe Markdown parser. Wraps md4c into a small
// AST so callers (V8 bindings, UI components, native consumers) can walk
// blocks/spans/text without touching SAX callbacks directly.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::markdown {

  enum class node_kind : std::uint8_t {
    // Block kinds.
    document,
    paragraph,
    heading,
    blockquote,
    list,
    list_item,
    code_block,
    html_block,
    thematic_break,
    table,
    table_head,
    table_body,
    table_row,
    table_cell,
    // Span kinds.
    emph,
    strong,
    strikethrough,
    underline,
    code_span,
    link,
    image,
    latex_math,
    wikilink,
    raw_html,
    // Text kinds.
    text,
    soft_break,
    hard_break,
    entity,
    null_char,
  };

  // One AST node. Most fields are kind-specific (see comments) and default to
  // empty/zero. Children are only populated for block/span containers.
  struct node {
    node_kind kind = node_kind::document;

    // Plain literal payload — populated for text/code_span/code_block/raw_html
    // /entity/html_block. For attribute-bearing kinds (link href, image src,
    // …) the payload lives in dedicated fields below.
    std::string text;

    // Heading: 1..6.
    int heading_level = 0;

    // List.
    bool ordered = false;
    int list_start = 1;
    bool tight = true;

    // List item.
    bool task = false;
    bool checked = false;

    // Code block.
    std::string info; // raw info string (e.g. "ts highlight")
    std::string lang; // first token of info (e.g. "ts")

    // Link / image.
    std::string href; // link target
    std::string src;  // image source
    std::string title;
    bool autolink = false;

    // Wikilink target.
    std::string wikilink_target;

    // Table cell alignment: "", "left", "right", "center".
    std::string align;

    std::vector<std::unique_ptr<node>> children;
  };

  // Bitmask. Mirrors the most useful md4c MD_FLAG_* values without forcing
  // callers to depend on md4c.h. Default `dialect_github` is a sensible UI
  // default; pass `commonmark` for stricter behavior.
  enum parse_flags : std::uint32_t {
    flag_collapse_whitespace = 1u << 0,
    flag_permissive_atx_headers = 1u << 1,
    flag_permissive_url_autolinks = 1u << 2,
    flag_permissive_email_autolinks = 1u << 3,
    flag_no_indented_code_blocks = 1u << 4,
    flag_no_html_blocks = 1u << 5,
    flag_no_html_spans = 1u << 6,
    flag_tables = 1u << 7,
    flag_strikethrough = 1u << 8,
    flag_permissive_www_autolinks = 1u << 9,
    flag_tasklists = 1u << 10,
    flag_latex_math_spans = 1u << 11,
    flag_wikilinks = 1u << 12,
    flag_underline = 1u << 13,
    flag_hard_soft_breaks = 1u << 14,
  };

  inline constexpr std::uint32_t dialect_commonmark = 0;
  inline constexpr std::uint32_t dialect_github =
      flag_permissive_url_autolinks | flag_permissive_email_autolinks |
      flag_permissive_www_autolinks | flag_tables | flag_strikethrough | flag_tasklists;

  struct parse_options {
    std::uint32_t flags = dialect_github;
  };

  // Parse `source` and return a tree rooted at a `document` node. On parse
  // failure (md4c returns non-zero — extremely rare for valid UTF-8), returns
  // a `document` node with whatever blocks were emitted before the failure.
  // Never returns nullptr.
  std::unique_ptr<node> parse(std::string_view source, const parse_options& opts = {});

  // Convert a kind enum into the lowercase tag used by the JS surface and
  // tests (e.g. `node_kind::code_block` → "code_block").
  std::string_view kind_tag(node_kind kind);

} // namespace fxe::markdown
