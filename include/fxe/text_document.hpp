#pragma once

// fxe::text_document — native rope-style backing for editor-grade buffers.
//
// Implementation: piece array over an immutable `original` buffer (seed
// text) plus an append-only `add_buffer`. Each piece holds a buffer id,
// start/length, and cached newline offsets within the slice. Cumulative
// length and cumulative newline-count prefix arrays are kept in sync after
// every edit so offset→piece, line→offset, and offset→line are O(log P).
//
// Memory model:
//   - All character data is stored as UTF-16 code units (`char16_t`) so
//     offsets line up 1:1 with V8 string indices on the binding boundary.
//     Editors care about code units, not bytes; UTF-8 conversion happens
//     only at the V8 ↔ C++ edge.
//   - Edits never touch existing buffers. Inserting `text` appends to
//     `add_buffer` and pushes a piece that references that slice; deletes
//     splice pieces out. Original characters are immutable until the
//     document is destroyed.

#include <fxe/types.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fxe {

  struct text_document_edit {
    u32 start = 0;       // pre-edit offset where the change began (UTF-16 code units)
    u32 removed = 0;     // # code units removed
    std::u16string deleted; // captured for undo / event payload
    std::u16string inserted;
  };

  struct text_line_range {
    u32 start = 0; // inclusive (UTF-16 code units)
    u32 end = 0;   // exclusive of trailing '\n'
  };

  class text_document {
  public:
    text_document() = default;
    explicit text_document(std::u16string_view initial);
    explicit text_document(std::string_view initial_utf8);

    // ----- Reads -----
    [[nodiscard]] u32 length() const noexcept;
    [[nodiscard]] u32 line_count() const noexcept;
    [[nodiscard]] u32 revision() const noexcept { return revision_; }
    [[nodiscard]] u32 piece_count() const noexcept { return static_cast<u32>(pieces_.size()); }

    [[nodiscard]] std::u16string slice(u32 start, u32 end) const;
    /** UTF-8 export of [start, end). */
    [[nodiscard]] std::string slice_utf8(u32 start, u32 end) const;
    /** Whole-document UTF-8 export. */
    [[nodiscard]] std::string text_utf8() const;

    [[nodiscard]] char16_t code_unit_at(u32 offset) const noexcept;

    [[nodiscard]] u32 line_to_offset(u32 line) const noexcept;
    [[nodiscard]] u32 offset_to_line(u32 offset) const noexcept;
    [[nodiscard]] text_line_range line_range(u32 line) const noexcept;
    [[nodiscard]] std::u16string line_text(u32 line) const;

    // ----- Edits -----
    /** Replace [start, end) with `text`. Updates revision & runs listeners. */
    text_document_edit replace(u32 start, u32 end, std::u16string_view text);
    text_document_edit replace_utf8(u32 start, u32 end, std::string_view text_utf8);

    /** Apply N replaces in one revision. Edits MUST be sorted ascending by
     *  `start` and non-overlapping (overlap throws). Run right-to-left so
     *  earlier offsets stay valid. Returns the captured edits in original
     *  ascending order. */
    std::vector<text_document_edit> apply_batch(std::span<const text_document_edit> edits);

    // ----- Listeners -----
    using listener_id = u64;
    using listener_fn = std::function<void(std::span<const text_document_edit>)>;
    listener_id subscribe(listener_fn fn);
    void unsubscribe(listener_id id);

    // ----- Search -----
    /** Literal substring scan. Stops at `limit` matches. Set
     *  `case_insensitive` for ASCII-folded search. Pass `from = 0` to scan
     *  the whole document. */
    struct match { u32 start; u32 end; };
    [[nodiscard]] std::vector<match>
    search_literal(std::u16string_view needle, u32 from = 0,
                   u32 limit = 0xFFFFFFFFu, bool case_insensitive = false) const;

    [[nodiscard]] std::vector<match>
    search_literal_utf8(std::string_view needle, u32 from = 0, u32 limit = 0xFFFFFFFFu,
                        bool case_insensitive = false) const;

  private:
    enum class buf : u8 { original = 0, add = 1 };
    struct piece {
      buf source;
      u32 start;
      u32 length;
      std::vector<u32> line_starts; // newline offsets relative to piece start
    };

    std::u16string original_;
    std::u16string add_buffer_;
    std::vector<piece> pieces_;
    std::vector<u32> cum_len_;   // size = pieces_.size() + 1
    std::vector<u32> cum_lines_; // size = pieces_.size() + 1
    u32 revision_ = 0;
    listener_id next_listener_id_ = 1;
    std::vector<std::pair<listener_id, listener_fn>> listeners_;

    void rebuild_cum() noexcept;
    [[nodiscard]] const std::u16string& buf_for(buf b) const noexcept;
    [[nodiscard]] piece make_piece(buf source, u32 start, u32 length) const;
    /** Find piece containing the absolute offset (clamped to [0, length()]). */
    struct locate { u32 piece_index; u32 inner; };
    [[nodiscard]] locate piece_at(u32 offset) const noexcept;
    void split_at(u32 offset);
    [[nodiscard]] u32 piece_starting_at(u32 offset) const noexcept;
    text_document_edit replace_internal(u32 start, u32 end, std::u16string_view text);
  };

} // namespace fxe
