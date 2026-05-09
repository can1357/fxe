// fxe::treesitter — wrapper implementations.

#include <fxe/treesitter.hpp>
#include <fxe/types.hpp>

#include <cstring>
#include <stdexcept>
#include <string>
#include <tree_sitter/api.h>
#include <utility>

namespace fxe::treesitter {

  // ---------------------------------------------------------------------
  // node
  // ---------------------------------------------------------------------
  static_assert(sizeof(TSNode) == sizeof(const void*) * 4,
                "fxe::treesitter::node assumes TSNode is 4 pointers wide");

  static TSNode to_ts_node(const void* const* storage) noexcept {
    TSNode n;
    std::memcpy(&n, storage, sizeof(TSNode));
    return n;
  }

  static node from_ts_node(TSNode n) noexcept {
    const void* storage[4];
    std::memcpy(storage, &n, sizeof(TSNode));
    return node{storage};
  }

  node::node(const void* tsnode_ptr_storage[4]) noexcept {
    std::memcpy(storage_, tsnode_ptr_storage, sizeof(storage_));
  }

  bool node::is_null() const noexcept {
    return ts_node_is_null(to_ts_node(storage_));
  }
  bool node::is_named() const noexcept {
    return ts_node_is_named(to_ts_node(storage_));
  }
  std::string_view node::kind() const noexcept {
    const TSNode n = to_ts_node(storage_);
    if (ts_node_is_null(n))
      return {};
    const char* k = ts_node_type(n);
    return k ? std::string_view{k} : std::string_view{};
  }
  u16 node::kind_id() const noexcept {
    return ts_node_symbol(to_ts_node(storage_));
  }
  u32 node::start_byte() const noexcept {
    return ts_node_start_byte(to_ts_node(storage_));
  }
  u32 node::end_byte() const noexcept {
    return ts_node_end_byte(to_ts_node(storage_));
  }
  point node::start_point() const noexcept {
    auto p = ts_node_start_point(to_ts_node(storage_));
    return {p.row, p.column};
  }
  point node::end_point() const noexcept {
    auto p = ts_node_end_point(to_ts_node(storage_));
    return {p.row, p.column};
  }
  u32 node::child_count() const noexcept {
    return ts_node_child_count(to_ts_node(storage_));
  }
  node node::child(u32 i) const noexcept {
    return from_ts_node(ts_node_child(to_ts_node(storage_), i));
  }
  u32 node::named_child_count() const noexcept {
    return ts_node_named_child_count(to_ts_node(storage_));
  }
  node node::named_child(u32 i) const noexcept {
    return from_ts_node(ts_node_named_child(to_ts_node(storage_), i));
  }
  node node::parent() const noexcept {
    return from_ts_node(ts_node_parent(to_ts_node(storage_)));
  }
  node node::next_sibling() const noexcept {
    return from_ts_node(ts_node_next_sibling(to_ts_node(storage_)));
  }
  node node::prev_sibling() const noexcept {
    return from_ts_node(ts_node_prev_sibling(to_ts_node(storage_)));
  }

  // ---------------------------------------------------------------------
  // tree
  // ---------------------------------------------------------------------
  tree::tree(tree&& other) noexcept : ts_tree_(other.ts_tree_) {
    other.ts_tree_ = nullptr;
  }
  tree& tree::operator=(tree&& other) noexcept {
    if (this != &other) {
      if (ts_tree_)
        ts_tree_delete(ts_tree_);
      ts_tree_ = other.ts_tree_;
      other.ts_tree_ = nullptr;
    }
    return *this;
  }
  tree::~tree() {
    if (ts_tree_)
      ts_tree_delete(ts_tree_);
  }

  node tree::root() const noexcept {
    if (!ts_tree_)
      return {};
    return from_ts_node(ts_tree_root_node(ts_tree_));
  }

  void tree::edit(const edit_descriptor& d) noexcept {
    if (!ts_tree_)
      return;
    TSInputEdit e{};
    e.start_byte = d.start_byte;
    e.old_end_byte = d.old_end_byte;
    e.new_end_byte = d.new_end_byte;
    e.start_point = TSPoint{d.start_point.row, d.start_point.column};
    e.old_end_point = TSPoint{d.old_end_point.row, d.old_end_point.column};
    e.new_end_point = TSPoint{d.new_end_point.row, d.new_end_point.column};
    ts_tree_edit(ts_tree_, &e);
  }

  // ---------------------------------------------------------------------
  // parser
  // ---------------------------------------------------------------------
  parser::parser() : ts_parser_(ts_parser_new()) {}
  parser::parser(parser&& other) noexcept
      : ts_parser_(other.ts_parser_), language_(other.language_) {
    other.ts_parser_ = nullptr;
    other.language_ = nullptr;
  }
  parser& parser::operator=(parser&& other) noexcept {
    if (this != &other) {
      if (ts_parser_)
        ts_parser_delete(ts_parser_);
      ts_parser_ = other.ts_parser_;
      language_ = other.language_;
      other.ts_parser_ = nullptr;
      other.language_ = nullptr;
    }
    return *this;
  }
  parser::~parser() {
    if (ts_parser_)
      ts_parser_delete(ts_parser_);
  }

  bool parser::set_language_by_name(std::string_view name) {
    const TSLanguage* lang = language_by_name(name);
    if (!lang)
      return false;
    return set_language(lang);
  }
  bool parser::set_language(const TSLanguage* lang) {
    if (!lang || !ts_parser_)
      return false;
    if (!ts_parser_set_language(ts_parser_, lang))
      return false;
    language_ = lang;
    return true;
  }

  tree parser::parse(std::string_view text, const tree* previous) const {
    if (!ts_parser_ || !language_)
      return {};
    TSTree* old = previous ? const_cast<TSTree*>(previous->raw()) : nullptr;
    TSTree* t = ts_parser_parse_string(ts_parser_, old, text.data(), static_cast<u32>(text.size()));
    return tree{t};
  }

  // ---------------------------------------------------------------------
  // query
  // ---------------------------------------------------------------------
  query::query(const TSLanguage* lang, std::string_view source) : language_(lang) {
    u32 error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    ts_query_ = ts_query_new(lang, source.data(), static_cast<u32>(source.size()), &error_offset,
                             &error_type);
    if (!ts_query_) {
      const char* kind = "unknown";
      switch (error_type) {
      case TSQueryErrorSyntax:
        kind = "syntax";
        break;
      case TSQueryErrorNodeType:
        kind = "node type";
        break;
      case TSQueryErrorField:
        kind = "field";
        break;
      case TSQueryErrorCapture:
        kind = "capture";
        break;
      case TSQueryErrorStructure:
        kind = "structure";
        break;
      case TSQueryErrorLanguage:
        kind = "language";
        break;
      default:
        break;
      }
      throw std::invalid_argument(std::string("tree-sitter query ") + kind + " error at byte " +
                                  std::to_string(error_offset));
    }
    const u32 n = ts_query_capture_count(ts_query_);
    capture_names_.reserve(n);
    for (u32 i = 0; i < n; ++i) {
      u32 len = 0;
      const char* nm = ts_query_capture_name_for_id(ts_query_, i, &len);
      capture_names_.emplace_back(nm, len);
    }
  }

  query::query(query&& other) noexcept
      : ts_query_(other.ts_query_), language_(other.language_),
        capture_names_(std::move(other.capture_names_)) {
    other.ts_query_ = nullptr;
    other.language_ = nullptr;
  }
  query& query::operator=(query&& other) noexcept {
    if (this != &other) {
      if (ts_query_)
        ts_query_delete(ts_query_);
      ts_query_ = other.ts_query_;
      language_ = other.language_;
      capture_names_ = std::move(other.capture_names_);
      other.ts_query_ = nullptr;
      other.language_ = nullptr;
    }
    return *this;
  }
  query::~query() {
    if (ts_query_)
      ts_query_delete(ts_query_);
  }

  void query::run(const node& root, u32 start_byte, u32 end_byte,
                  const std::function<bool(const capture&)>& on_capture) const {
    if (!ts_query_)
      return;
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_set_byte_range(cursor, start_byte, end_byte);
    ts_query_cursor_exec(cursor, ts_query_, to_ts_node(root.raw()));
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
      for (u16 i = 0; i < match.capture_count; ++i) {
        const auto& cap = match.captures[i];
        capture c;
        c.n = from_ts_node(cap.node);
        if (cap.index < capture_names_.size()) {
          c.name = capture_names_[cap.index];
        }
        if (!on_capture(c)) {
          ts_query_cursor_delete(cursor);
          return;
        }
      }
    }
    ts_query_cursor_delete(cursor);
  }

} // namespace fxe::treesitter
