// fxe::text_document — piece-array implementation. See header for design.

#include <fxe/text_document.hpp>

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace fxe {

  namespace {
    // UTF-8 → UTF-16 (BMP-aware, supplementary planes encoded as surrogate
    // pairs). Replaces malformed input with U+FFFD instead of throwing.
    std::u16string utf8_to_utf16(std::string_view in) {
      std::u16string out;
      out.reserve(in.size());
      const auto* p = reinterpret_cast<const u8*>(in.data());
      const auto* end = p + in.size();
      while (p < end) {
        char32_t cp;
        u8 c = *p;
        if (c < 0x80) {
          cp = static_cast<char32_t>(c);
          p += 1;
        } else if ((c & 0xE0) == 0xC0 && end - p >= 2 && (p[1] & 0xC0) == 0x80) {
          cp = static_cast<char32_t>(((c & 0x1Fu) << 6) | (p[1] & 0x3Fu));
          p += 2;
        } else if ((c & 0xF0) == 0xE0 && end - p >= 3 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
          cp = static_cast<char32_t>(((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu));
          p += 3;
        } else if ((c & 0xF8) == 0xF0 && end - p >= 4 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
          cp = static_cast<char32_t>(((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu));
          p += 4;
        } else {
          cp = 0xFFFDu;
          p += 1;
        }
        if (cp <= 0xFFFF) {
          out.push_back(static_cast<char16_t>(cp));
        } else {
          cp -= 0x10000u;
          out.push_back(static_cast<char16_t>(0xD800u | ((cp >> 10) & 0x3FFu)));
          out.push_back(static_cast<char16_t>(0xDC00u | (cp & 0x3FFu)));
        }
      }
      return out;
    }

    void emit_utf8(char32_t cp, std::string& out) {
      if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
      } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    }

    std::string utf16_to_utf8(std::u16string_view in) {
      std::string out;
      out.reserve(in.size());
      for (usize i = 0; i < in.size(); ++i) {
        char16_t c = in[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < in.size()) {
          char16_t lo = in[i + 1];
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            char32_t cp = static_cast<char32_t>(0x10000u + ((static_cast<u32>(c - 0xD800) << 10) | static_cast<u32>(lo - 0xDC00)));
            emit_utf8(cp, out);
            i += 1;
            continue;
          }
        }
        emit_utf8(static_cast<char32_t>(c), out);
      }
      return out;
    }

    inline char16_t fold_ascii(char16_t c) noexcept {
      return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + (u'a' - u'A')) : c;
    }
  } // namespace

  // ---------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------
  text_document::text_document(std::u16string_view initial) : original_(initial) {
    if (!original_.empty())
      pieces_.push_back(make_piece(buf::original, 0, static_cast<u32>(original_.size())));
    rebuild_cum();
  }

  text_document::text_document(std::string_view initial_utf8)
      : text_document(utf8_to_utf16(initial_utf8)) {}

  // ---------------------------------------------------------------------
  // Reads
  // ---------------------------------------------------------------------
  u32 text_document::length() const noexcept {
    return cum_len_.empty() ? 0u : cum_len_.back();
  }

  u32 text_document::line_count() const noexcept {
    return (cum_lines_.empty() ? 0u : cum_lines_.back()) + 1u;
  }

  std::u16string text_document::slice(u32 start, u32 end) const {
    const u32 len = length();
    if (start > len) start = len;
    if (end > len) end = len;
    if (start >= end) return {};
    std::u16string out;
    out.reserve(end - start);
    auto loc = piece_at(start);
    u32 remaining = end - start;
    u32 idx = loc.piece_index;
    u32 inner = loc.inner;
    while (remaining > 0 && idx < pieces_.size()) {
      const auto& p = pieces_[idx];
      const auto& src = buf_for(p.source);
      const u32 take = std::min(remaining, p.length - inner);
      out.append(src.data() + p.start + inner, take);
      remaining -= take;
      idx += 1;
      inner = 0;
    }
    return out;
  }

  std::string text_document::slice_utf8(u32 start, u32 end) const {
    return utf16_to_utf8(slice(start, end));
  }

  std::string text_document::text_utf8() const {
    return slice_utf8(0, length());
  }

  char16_t text_document::code_unit_at(u32 offset) const noexcept {
    if (offset >= length()) return 0;
    auto loc = piece_at(offset);
    const auto& p = pieces_[loc.piece_index];
    return buf_for(p.source)[p.start + loc.inner];
  }

  u32 text_document::line_to_offset(u32 line) const noexcept {
    if (line == 0) return 0;
    const u32 total_lines = line_count();
    if (line >= total_lines) return length();
    // Find smallest piece-index i such that cum_lines_[i+1] >= line.
    u32 lo = 0, hi = static_cast<u32>(pieces_.size());
    while (lo < hi) {
      u32 mid = (lo + hi) >> 1;
      if (cum_lines_[mid + 1] >= line) hi = mid; else lo = mid + 1;
    }
    const u32 piece_idx = lo;
    if (piece_idx >= pieces_.size()) return length();
    const auto& p = pieces_[piece_idx];
    const u32 lines_before = cum_lines_[piece_idx];
    const u32 wanted_within = line - lines_before - 1;
    if (wanted_within >= p.line_starts.size()) {
      // Edge: line falls at the boundary between two pieces.
      return cum_len_[piece_idx];
    }
    const u32 inner = p.line_starts[wanted_within] + 1;
    return cum_len_[piece_idx] + inner;
  }

  u32 text_document::offset_to_line(u32 offset) const noexcept {
    if (offset == 0) return 0;
    const u32 len = length();
    if (offset >= len) return line_count() - 1;
    auto loc = piece_at(offset);
    const auto& p = pieces_[loc.piece_index];
    const u32 lines_before = cum_lines_[loc.piece_index];
    // Newlines strictly before `loc.inner`.
    const auto& ls = p.line_starts;
    auto it = std::lower_bound(ls.begin(), ls.end(), loc.inner);
    return lines_before + static_cast<u32>(it - ls.begin());
  }

  text_line_range text_document::line_range(u32 line) const noexcept {
    const u32 total = line_count();
    if (line >= total) return {length(), length()};
    const u32 start = line_to_offset(line);
    const u32 next = (line + 1 >= total) ? length() : line_to_offset(line + 1);
    u32 end = next;
    if (end > start) {
      // Trim the trailing '\n' that terminates this line.
      if (code_unit_at(end - 1) == u'\n') end -= 1;
    }
    return {start, end};
  }

  std::u16string text_document::line_text(u32 line) const {
    auto r = line_range(line);
    return slice(r.start, r.end);
  }

  // ---------------------------------------------------------------------
  // Edits
  // ---------------------------------------------------------------------
  text_document_edit text_document::replace(u32 start, u32 end, std::u16string_view text) {
    auto edit = replace_internal(start, end, text);
    revision_ += 1;
    if (!listeners_.empty()) {
      const text_document_edit edits[1] = {edit};
      const auto listeners_copy = listeners_;
      for (const auto& [id, fn] : listeners_copy) (void)id, fn(edits);
    }
    return edit;
  }

  text_document_edit text_document::replace_utf8(u32 start, u32 end, std::string_view t) {
    return replace(start, end, utf8_to_utf16(t));
  }

  std::vector<text_document_edit>
  text_document::apply_batch(std::span<const text_document_edit> edits) {
    if (edits.empty()) return {};
    // Validate ordering & non-overlap.
    for (usize i = 1; i < edits.size(); ++i) {
      const auto& a = edits[i - 1];
      const auto& b = edits[i];
      if (b.start < a.start + a.removed)
        throw std::invalid_argument("text_document::apply_batch: overlapping edits");
    }
    std::vector<text_document_edit> applied;
    applied.resize(edits.size());
    // Apply right-to-left so left-side offsets stay valid.
    for (usize i = edits.size(); i-- > 0;) {
      const auto& e = edits[i];
      applied[i] = replace_internal(e.start, e.start + e.removed, e.inserted);
    }
    revision_ += 1;
    if (!listeners_.empty()) {
      const auto listeners_copy = listeners_;
      for (const auto& [id, fn] : listeners_copy) (void)id, fn(applied);
    }
    return applied;
  }

  text_document::listener_id text_document::subscribe(listener_fn fn) {
    const auto id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(fn));
    return id;
  }

  void text_document::unsubscribe(listener_id id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [id](const auto& kv) { return kv.first == id; }),
                     listeners_.end());
  }

  // ---------------------------------------------------------------------
  // Search
  // ---------------------------------------------------------------------
  std::vector<text_document::match>
  text_document::search_literal(std::u16string_view needle, u32 from, u32 limit,
                                bool case_insensitive) const {
    std::vector<match> out;
    if (needle.empty()) return out;
    const u32 len = length();
    if (from >= len) return out;
    // Materialise the slice once — for huge documents this is wasteful, but
    // for typical single-file editors (<10 MB) it's a single allocation
    // instead of N piece-stitching loops. v2 can scan piece-by-piece.
    std::u16string hay = slice(from, len);
    const usize n = needle.size();
    const usize m = hay.size();
    if (n > m) return out;
    auto eq = [case_insensitive](char16_t a, char16_t b) {
      return case_insensitive ? fold_ascii(a) == fold_ascii(b) : a == b;
    };
    usize i = 0;
    while (i + n <= m && out.size() < limit) {
      bool match = true;
      for (usize k = 0; k < n; ++k) {
        if (!eq(hay[i + k], needle[k])) { match = false; break; }
      }
      if (match) {
        out.push_back({static_cast<u32>(from + i), static_cast<u32>(from + i + n)});
        i += n;
      } else {
        i += 1;
      }
    }
    return out;
  }

  std::vector<text_document::match>
  text_document::search_literal_utf8(std::string_view needle, u32 from, u32 limit,
                                     bool case_insensitive) const {
    return search_literal(utf8_to_utf16(needle), from, limit, case_insensitive);
  }

  // ---------------------------------------------------------------------
  // Internals
  // ---------------------------------------------------------------------
  const std::u16string& text_document::buf_for(buf b) const noexcept {
    return b == buf::original ? original_ : add_buffer_;
  }

  text_document::piece text_document::make_piece(buf source, u32 start, u32 length) const {
    piece p;
    p.source = source;
    p.start = start;
    p.length = length;
    const auto& src = buf_for(source);
    const u32 end = start + length;
    for (u32 i = start; i < end; ++i) {
      if (src[i] == u'\n') p.line_starts.push_back(i - start);
    }
    return p;
  }

  text_document::locate text_document::piece_at(u32 offset) const noexcept {
    if (pieces_.empty()) return {0u, 0u};
    // Largest i with cum_len_[i] <= offset.
    u32 lo = 0;
    u32 hi = static_cast<u32>(pieces_.size());
    while (lo < hi) {
      u32 mid = (lo + hi) >> 1;
      if (cum_len_[mid + 1] <= offset) lo = mid + 1; else hi = mid;
    }
    const u32 idx = std::min<u32>(lo, static_cast<u32>(pieces_.size()) - 1);
    return {idx, offset - cum_len_[idx]};
  }

  void text_document::split_at(u32 offset) {
    if (pieces_.empty()) return;
    if (offset == 0 || offset >= length()) return;
    auto loc = piece_at(offset);
    if (loc.inner == 0) return;
    const piece p = pieces_[loc.piece_index];
    piece left = make_piece(p.source, p.start, loc.inner);
    piece right = make_piece(p.source, p.start + loc.inner, p.length - loc.inner);
    pieces_[loc.piece_index] = std::move(left);
    pieces_.insert(pieces_.begin() + loc.piece_index + 1, std::move(right));
    rebuild_cum();
  }

  u32 text_document::piece_starting_at(u32 offset) const noexcept {
    if (offset >= length()) return static_cast<u32>(pieces_.size());
    u32 lo = 0;
    u32 hi = static_cast<u32>(pieces_.size());
    while (lo < hi) {
      u32 mid = (lo + hi) >> 1;
      if (cum_len_[mid] < offset) lo = mid + 1; else hi = mid;
    }
    return lo;
  }

  text_document_edit text_document::replace_internal(u32 start, u32 end,
                                                     std::u16string_view text) {
    const u32 len = length();
    if (start > len) start = len;
    if (end > len) end = len;
    if (start > end) std::swap(start, end);

    text_document_edit edit;
    edit.start = start;
    edit.removed = end - start;
    edit.inserted.assign(text);
    if (edit.removed > 0) edit.deleted = slice(start, end);
    if (edit.removed == 0 && text.empty()) return edit;

    split_at(end);
    split_at(start);
    const u32 first = piece_starting_at(start);
    const u32 last = piece_starting_at(end);
    std::vector<piece> replacement;
    if (!text.empty()) {
      const u32 add_start = static_cast<u32>(add_buffer_.size());
      add_buffer_.append(text);
      replacement.push_back(make_piece(buf::add, add_start, static_cast<u32>(text.size())));
    }
    pieces_.erase(pieces_.begin() + first, pieces_.begin() + last);
    pieces_.insert(pieces_.begin() + first, replacement.begin(), replacement.end());
    rebuild_cum();
    return edit;
  }

  void text_document::rebuild_cum() noexcept {
    const usize n = pieces_.size();
    cum_len_.assign(n + 1, 0u);
    cum_lines_.assign(n + 1, 0u);
    for (usize i = 0; i < n; ++i) {
      cum_len_[i + 1] = cum_len_[i] + pieces_[i].length;
      cum_lines_[i + 1] =
          cum_lines_[i] + static_cast<u32>(pieces_[i].line_starts.size());
    }
  }

} // namespace fxe
