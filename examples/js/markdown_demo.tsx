/** @jsxImportSource fxe-ui */

// Markdown rendering demo.
//
// fxe-ui ships a `Markdown` component that walks the `Markdown.parse` AST
// into themed primitives (headings, lists, fenced code with tree-sitter
// highlighting, tables, blockquotes, links). This demo just supplies a
// sample document and three themes; the toolbar swaps the active theme
// through `ThemeProvider`, and the renderer reflows in place.

import { App, Window } from 'fxe';
import {
  Button,
  Markdown,
  type MarkdownTheme,
  mount,
  ScrollView,
  StyleSheet,
  Text,
  ThemeProvider,
  type Node as UiNode,
  useState,
  View,
} from 'fxe-ui';

// ---------------------------------------------------------------------------
// Themes — three fully-styled palettes covering background → border → accent.
// ---------------------------------------------------------------------------

const baseSpacing = { xs: 4, sm: 8, md: 12, lg: 16, xl: 24 };
const baseRadii = { sm: 4, md: 8, lg: 14, pill: 999 };
const baseSizes = { sm: 12, md: 15, lg: 18, xl: 24 };

const darkTheme: MarkdownTheme = {
  colors: {
    background: 0x0b0f17ff,
    surface: 0x131826ff,
    primary: 0x6aa3ffff,
    primaryText: 0xffffffff,
    text: 0xe6ebf5ff,
    mutedText: 0x9aa3b8ff,
    border: 0x2a3145ff,
    danger: 0xff6b81ff,
    code: 0xf6c177ff,
    codeBg: 0x1c2233ff,
    quote: 0xb8c0d4ff,
    link: 0x7cc6ffff,
    headingRule: 0x2a3145ff,
    tableHeaderBg: 0x1c2233ff,
  },
  spacing: baseSpacing,
  radii: baseRadii,
  fontSizes: baseSizes,
  fonts: { body: 'system-ui', mono: 'Menlo' },
  syntax: {
    comment: 0x6c7793ff,
    string: 0xa3d9a5ff,
    number: 0xf6c177ff,
    constant: 0xc599ffff,
    keyword: 0xff8fb1ff,
    type: 0x7cc6ffff,
    function: 0xffd479ff,
    property: 0xb8c0d4ff,
    tag: 0xff8fb1ff,
    attribute: 0x7cc6ffff,
  },
};

const lightTheme: MarkdownTheme = {
  colors: {
    background: 0xfafbfcff,
    surface: 0xffffffff,
    primary: 0x0b66ffff,
    primaryText: 0xffffffff,
    text: 0x1d2433ff,
    mutedText: 0x57606aff,
    border: 0xd0d7deff,
    danger: 0xcf222eff,
    code: 0xb12a8eff,
    codeBg: 0xf3f4f6ff,
    quote: 0x57606aff,
    link: 0x0969daff,
    headingRule: 0xd8dee4ff,
    tableHeaderBg: 0xf3f4f6ff,
  },
  spacing: baseSpacing,
  radii: baseRadii,
  fontSizes: baseSizes,
  fonts: { body: 'system-ui', mono: 'Menlo' },
  syntax: {
    comment: 0x6e7781ff,
    string: 0x0a3069ff,
    number: 0x953800ff,
    constant: 0x8250dfff,
    keyword: 0xcf222eff,
    type: 0x0550aeff,
    function: 0x6639baff,
    property: 0x24292fff,
    tag: 0x116329ff,
    attribute: 0x0550aeff,
  },
};

const solarizedTheme: MarkdownTheme = {
  colors: {
    background: 0x002b36ff,
    surface: 0x073642ff,
    primary: 0x268bd2ff,
    primaryText: 0xfdf6e3ff,
    text: 0xeee8d5ff,
    mutedText: 0x93a1a1ff,
    border: 0x0f4655ff,
    danger: 0xdc322fff,
    code: 0xb58900ff,
    codeBg: 0x073642ff,
    quote: 0x93a1a1ff,
    link: 0x2aa198ff,
    headingRule: 0x0f4655ff,
    tableHeaderBg: 0x0a4a59ff,
  },
  spacing: baseSpacing,
  radii: baseRadii,
  fontSizes: baseSizes,
  fonts: { body: 'system-ui', mono: 'Menlo' },
  syntax: {
    comment: 0x586e75ff,
    string: 0x2aa198ff,
    number: 0xd33682ff,
    constant: 0x6c71c4ff,
    keyword: 0x859900ff,
    type: 0xb58900ff,
    function: 0x268bd2ff,
    property: 0x93a1a1ff,
    tag: 0xcb4b16ff,
    attribute: 0x268bd2ff,
  },
};

const themes = {
  dark: darkTheme,
  light: lightTheme,
  solarized: solarizedTheme,
} as const;

type ThemeId = keyof typeof themes;

// ---------------------------------------------------------------------------
// Sample document — exercises every kind the renderer supports.
// ---------------------------------------------------------------------------

const SAMPLE_MD = `# fxe Markdown

A native Markdown renderer built on **md4c**, themed via \`fxe-ui\`.
This single document exercises every block and span the parser emits.

## Inline emphasis

Text supports *emphasis*, **strong emphasis**, ***both***, ~~strikethrough~~,
and inline code like \`Markdown.parse(src)\`. Links resolve through the
shell: visit [the FXE docs](https://example.com "FXE docs") or just paste
https://example.com — autolinks work too.

## Lists

- Bulleted, three items
- Items can hold *inline* formatting and \`code\`
- And nested content
  - second level

1. Ordered lists track their start
2. ...so this stays at 2
3. ...and this at 3

### Task list

- [x] Add md4c as a FetchContent dep
- [x] Implement the parser bridge
- [x] Implement \`Markdown.parse\` JS binding
- [ ] Ship the UI component

## Quotes & rules

> "Markdown should travel through the same pipeline as everything else
> in fxe — layout, theming, hit testing, the lot."

---

## Code blocks

Tree-sitter colorizes fenced blocks per their language tag. Theme palettes
map every capture (\`keyword\`, \`string\`, \`type\`, …) to a color, so the
same source recolors when you swap themes.

\`\`\`ts
import { mount, View, Text } from 'fxe-ui';

// Greet the user — the comment uses the syntax.comment color.
function greet(name: string): string {
  return \`hello, \${name}!\`;
}

mount(<View><Text>{greet('world')}</Text></View>, win);
\`\`\`

\`\`\`tsx
function Counter({ start }: { start: number }) {
  const [n, setN] = useState(start);
  return <Button title="bump" onPress={() => setN(n + 1)} />;
}
\`\`\`

\`\`\`json
{
  "name": "fxe-markdown",
  "version": "0.1.0",
  "private": true,
  "highlights": ["typescript", "tsx", "json"]
}
\`\`\`

## Table

| Feature       | Status   | Notes                       |
| :------------ | :------: | --------------------------: |
| Headings      | ok       | levels 1..6                 |
| Lists         | ok       | tight + tasks               |
| Tables        | ok       | with cell alignment         |
| Images        | partial  | renders alt text only       |
`;

// ---------------------------------------------------------------------------
// Demo shell.
// ---------------------------------------------------------------------------

const TOOLBAR_HEIGHT = 48;
const WINDOW_WIDTH = 920;
const WINDOW_HEIGHT = 720;

const s = StyleSheet.create({
  toolbar: {
    flexDirection: 'row',
    gap: 8,
    paddingX: 16,
    height: TOOLBAR_HEIGHT,
    alignItems: 'center',
  },
  toolbarLabel: { fontSize: 13, height: 18 },
  themeButton: { width: 96, height: 30 },
});

function Demo(): UiNode {
  const [themeId, setThemeId] = useState<ThemeId>('dark');
  const theme = themes[themeId];
  return (
    <ThemeProvider value={theme}>
      <View
        style={{
          width: WINDOW_WIDTH,
          height: WINDOW_HEIGHT,
          backgroundColor: theme.colors.background,
          alignItems: 'stretch',
        }}
      >
        <View
          style={{
            ...s.toolbar,
            width: WINDOW_WIDTH,
            borderBottomWidth: 1,
            borderBottomColor: theme.colors.border,
          }}
        >
          <Text style={{ ...s.toolbarLabel, width: 56, color: theme.colors.mutedText }}>
            Theme:
          </Text>
          {(Object.keys(themes) as ThemeId[]).map((id) => (
            <View key={`t-${id}`} style={s.themeButton}>
              <Button title={id} onPress={() => setThemeId(id)} />
            </View>
          ))}
          <View style={{ flex: 1 }} />
          <Text
            style={{
              ...s.toolbarLabel,
              width: 200,
              textAlign: 'right',
              color: theme.colors.mutedText,
            }}
          >
            md4c + fxe-ui
          </Text>
        </View>
        <ScrollView
          style={{
            width: WINDOW_WIDTH,
            height: WINDOW_HEIGHT - TOOLBAR_HEIGHT,
            backgroundColor: theme.colors.background,
          }}
        >
          <Markdown
            source={SAMPLE_MD}
            style={{
              width: WINDOW_WIDTH,
              padding: theme.spacing.lg,
              backgroundColor: theme.colors.background,
            }}
          />
        </ScrollView>
      </View>
    </ThemeProvider>
  );
}

const win = new Window({
  width: 920,
  height: 720,
  title: 'fxe markdown demo',
  permissions: { shell: true },
});

mount(<Demo />, win);
App.run();
