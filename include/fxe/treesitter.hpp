#pragma once

// fxe::treesitter — thin C++ wrapper over the Tree-sitter runtime + bundled
// grammars. The runtime is pure C; the wrapper handles RAII lifetimes,
// translates between TSPoint/TSNode and the rest of fxe, and exposes a
// small subset of the runtime API the editor actually needs:
//   - Parser   : owns a reusable TSParser, switches grammars by name.
//   - Tree     : owns a TSTree, supports incremental edit + reparse.
//   - Node     : value-type wrapping TSNode.
//   - Query    : compiled query + helper iterator over matches.
//   - Language registry: looked up by short name (`"typescript"`, `"tsx"`, …).

#include <fxe/types.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct TSLanguage;
struct TSParser;
struct TSTree;
struct TSQuery;

namespace fxe::treesitter {

  /** Look up a baked-in language by name. Returns nullptr for unknown. */
  [[nodiscard]] const TSLanguage* language_by_name(std::string_view name);

  /** Names of all baked-in grammars (stable order). */
  [[nodiscard]] std::vector<std::string_view> available_languages();

  struct point {
    u32 row = 0;
    u32 column = 0;
  };

  struct edit_descriptor {
    u32 start_byte = 0;
    u32 old_end_byte = 0;
    u32 new_end_byte = 0;
    point start_point{};
    point old_end_point{};
    point new_end_point{};
  };

  class node {
  public:
    node() noexcept = default;
    /** Implementation detail: constructed from a TSNode by the wrappers. */
    explicit node(const void* tsnode_ptr_storage[4]) noexcept;

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_named() const noexcept;
    [[nodiscard]] std::string_view kind() const noexcept;
    [[nodiscard]] u16 kind_id() const noexcept;
    [[nodiscard]] u32 start_byte() const noexcept;
    [[nodiscard]] u32 end_byte() const noexcept;
    [[nodiscard]] point start_point() const noexcept;
    [[nodiscard]] point end_point() const noexcept;
    [[nodiscard]] u32 child_count() const noexcept;
    [[nodiscard]] node child(u32 index) const noexcept;
    [[nodiscard]] u32 named_child_count() const noexcept;
    [[nodiscard]] node named_child(u32 index) const noexcept;
    [[nodiscard]] node parent() const noexcept;
    [[nodiscard]] node next_sibling() const noexcept;
    [[nodiscard]] node prev_sibling() const noexcept;

    /** Opaque storage for the underlying TSNode. */
    [[nodiscard]] const void* const* raw() const noexcept {
      return storage_;
    }

  private:
    const void* storage_[4]{};
  };

  class tree {
  public:
    tree() = default;
    tree(const tree&) = delete;
    tree& operator=(const tree&) = delete;
    tree(tree&& other) noexcept;
    tree& operator=(tree&& other) noexcept;
    ~tree();

    [[nodiscard]] bool is_valid() const noexcept {
      return ts_tree_ != nullptr;
    }
    [[nodiscard]] node root() const noexcept;
    void edit(const edit_descriptor& d) noexcept;

    /** Implementation detail. */
    explicit tree(TSTree* raw) noexcept : ts_tree_(raw) {}
    TSTree* raw() noexcept {
      return ts_tree_;
    }
    const TSTree* raw() const noexcept {
      return ts_tree_;
    }

  private:
    TSTree* ts_tree_ = nullptr;
  };

  class parser {
  public:
    parser();
    parser(const parser&) = delete;
    parser& operator=(const parser&) = delete;
    parser(parser&&) noexcept;
    parser& operator=(parser&&) noexcept;
    ~parser();

    /** Switch to a baked-in grammar. Returns false if name is unknown. */
    bool set_language_by_name(std::string_view name);
    bool set_language(const TSLanguage* lang);
    [[nodiscard]] const TSLanguage* language() const noexcept {
      return language_;
    }

    /** Parse a string. `previous` enables incremental reparse — pass the
     *  prior tree (after `edit()`-ing it) to reuse unchanged subtrees. */
    [[nodiscard]] tree parse(std::string_view text, const tree* previous = nullptr) const;

  private:
    TSParser* ts_parser_ = nullptr;
    const TSLanguage* language_ = nullptr;
  };

  class query {
  public:
    /** Compile `source` against `lang`. Throws std::invalid_argument with a
     *  human-readable message when the query is malformed. */
    query(const TSLanguage* lang, std::string_view source);
    query(const query&) = delete;
    query& operator=(const query&) = delete;
    query(query&&) noexcept;
    query& operator=(query&&) noexcept;
    ~query();

    /** A capture inside a match — name + the matched node. */
    struct capture {
      std::string_view name;
      node n;
    };

    /** Run the query against `root`, restricted to byte range [start, end).
     *  Pass {0, UINT32_MAX} to scan the whole tree. The callback runs once
     *  per capture; return false from it to stop iteration. */
    void run(const node& root, u32 start_byte, u32 end_byte,
             const std::function<bool(const capture&)>& on_capture) const;

    [[nodiscard]] const TSQuery* raw() const noexcept {
      return ts_query_;
    }

  private:
    TSQuery* ts_query_ = nullptr;
    const TSLanguage* language_ = nullptr;
    std::vector<std::string> capture_names_;
  };

} // namespace fxe::treesitter
