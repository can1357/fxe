import type { Theme } from '../theme/index.ts';
import type { HighlightTheme } from './incremental.ts';

export * from './incremental.ts';

export function defaultHighlightTheme(theme: Theme): HighlightTheme {
  return {
    attribute: { color: theme.colors.primary },
    comment: { color: theme.colors.mutedText, italic: true },
    constant: { color: 0x6c71c4ff },
    function: { color: theme.colors.primary },
    keyword: { color: theme.colors.primary },
    number: { color: 0xd33682ff },
    property: { color: theme.colors.text },
    string: { color: 0x2aa198ff },
    tag: { color: 0xcb4b16ff },
    type: { color: 0xb58900ff },
  };
}
