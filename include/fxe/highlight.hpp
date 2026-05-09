#pragma once

// fxe::highlight — syntax highlighting on top of fxe::treesitter.
//
// Given a source string and a language name, runs a built-in tree-sitter
// highlights query and returns a flat, non-overlapping list of tokens that
// the markdown renderer (and any other consumer) can colorize via a theme.
//
// Language names mirror fxe::treesitter::available_languages(), plus a few
// common aliases (`ts` → `typescript`, `js` → `typescript`, `jsx` → `tsx`).
// Languages without a baked-in query — or builds without tree-sitter at all —
// surface as std::nullopt and the caller falls back to plain text.

#include <fxe/types.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::highlight {

  /** A non-overlapping byte range tagged with a tree-sitter capture name
   *  (e.g. "keyword", "string", "type"). The caller maps the name to a
   *  theme color. Bytes between adjacent tokens, or before/after the
   *  first/last token, are unhighlighted plain text. */
  struct token {
    u32 start = 0;
    u32 end = 0;
    std::string name;
  };

  struct result {
    std::string language;        // canonical grammar name actually used
    std::vector<token> tokens;   // sorted by `start`, non-overlapping
  };

  /** Highlight `source` using the built-in query for `language`. Returns
   *  std::nullopt for unknown / unsupported languages, including when the
   *  build was configured without tree-sitter. */
  std::optional<result> tokenize(std::string_view source, std::string_view language);

  /** Languages with a built-in highlights query. Stable ordering. */
  std::vector<std::string_view> supported_languages();

} // namespace fxe::highlight
