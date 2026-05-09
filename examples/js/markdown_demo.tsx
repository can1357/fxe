/** @jsxImportSource fxe-ui */

// Markdown rendering demo.
//
// Walks the AST produced by `Markdown.parse` into fxe-ui primitives. The
// renderer is theme-aware: every visual decision (color, font size, spacing)
// reads from `useTheme()`, so swapping the theme via the toolbar buttons
// re-renders the whole document in place.

import { App, Primitives, Window } from 'fxe';
import {
  Button,
  type Color,
  Layer,
  memo,
  mount,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  type Theme,
  ThemeProvider,
  type Node as UiNode,
  useMemo,
  useState,
  useTheme,
  View,
} from 'fxe-ui';

// ---------------------------------------------------------------------------
// Themes — three fully-styled palettes covering background → border → accent.
// ---------------------------------------------------------------------------

interface SyntaxPalette {
  comment: number;
  string: number;
  number: number;
  constant: number;
  keyword: number;
  type: number;
  function: number;
  property: number;
  tag: number;
  attribute: number;
}

interface MarkdownTheme extends Theme {
  colors: Theme['colors'] & {
    code: number;
    codeBg: number;
    quote: number;
    link: number;
    headingRule: number;
    tableHeaderBg: number;
  };
  fonts: { body: string; mono: string };
  syntax: SyntaxPalette;
}

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
const themeIds = Object.keys(themes) as ThemeId[];

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
// Renderer.
// ---------------------------------------------------------------------------

type MdNode = FXEMarkdown.Node;

const inlineKinds: ReadonlyArray<MdNode['type']> = [
  'text',
  'emph',
  'strong',
  'strikethrough',
  'underline',
  'code_span',
  'link',
  'image',
  'soft_break',
  'hard_break',
  'entity',
  'raw_html',
  'wikilink',
];

function isInline(n: MdNode): boolean {
  return inlineKinds.includes(n.type);
}

interface Inline {
  text: string;
  bold?: boolean;
  italic?: boolean;
  strike?: boolean;
  underline?: boolean;
  code?: boolean;
  link?: string;
}

// Flatten a span subtree to a list of styled text runs. fxe-ui's <Text>
// children become a single line, so we pre-merge adjacent runs that share
// formatting and emit one <Text> per run with the appropriate style.
function flatten(node: MdNode, acc: Inline[], style: Inline = { text: '' }): void {
  switch (node.type) {
    case 'text':
    case 'entity':
      acc.push({ ...style, text: node.text });
      return;
    case 'soft_break':
      acc.push({ ...style, text: ' ' });
      return;
    case 'hard_break':
      acc.push({ ...style, text: '\n' });
      return;
    case 'raw_html':
      // Render literal HTML as plain text — no HTML rendering in v0.
      acc.push({ ...style, text: node.text });
      return;
    case 'emph':
      for (const c of node.children) flatten(c, acc, { ...style, italic: true });
      return;
    case 'strong':
      for (const c of node.children) flatten(c, acc, { ...style, bold: true });
      return;
    case 'strikethrough':
      for (const c of node.children) flatten(c, acc, { ...style, strike: true });
      return;
    case 'underline':
      for (const c of node.children) flatten(c, acc, { ...style, underline: true });
      return;
    case 'code_span': {
      const txt = node.children.map((c) => c.text).join('');
      acc.push({ ...style, text: txt, code: true });
      return;
    }
    case 'link': {
      for (const c of node.children) flatten(c, acc, { ...style, link: node.href });
      return;
    }
    case 'image': {
      // Show "[alt]" for images; native image fetching is out of scope here.
      const alt: Inline[] = [];
      for (const c of node.children) flatten(c, alt, style);
      const merged = alt.map((r) => r.text).join('');
      acc.push({ ...style, text: `🖼 ${merged || node.src}`, italic: true });
      return;
    }
    case 'wikilink': {
      const inner: Inline[] = [];
      for (const c of node.children) flatten(c, inner, style);
      const txt = inner.map((r) => r.text).join('') || node.target;
      acc.push({ ...style, text: txt, link: node.target });
      return;
    }
    default:
      // Unknown inline kind — ignore.
      return;
  }
}

function inlineText(children: ReadonlyArray<MdNode>): string {
  const runs: Inline[] = [];
  for (const c of children) flatten(c, runs);
  return runs.map((r) => r.text).join('');
}

interface InlineRunsProps {
  key?: string | number;
  children: ReadonlyArray<MdNode>;
  baseSize?: number;
  baseColor?: Color;
  bold?: boolean;
}
function inlineRuns(props: InlineRunsProps): UiNode {
  const t = useTheme() as MarkdownTheme;
  const runs = useMemo(() => {
    const acc: Inline[] = [];
    for (const c of props.children) flatten(c, acc);
    return acc;
  }, [props.children]);
  const baseSize = props.baseSize ?? t.fontSizes.md;
  const baseColor = props.baseColor ?? t.colors.text;
  // Render: each run becomes its own <Text>; siblings inside one <Text>
  // box flow horizontally because fxe-ui Text concatenates string children.
  // We wrap the run sequence in a row View that wraps via line breaks
  // embedded in the text content (hard_break = '\n').
  return (
    <View
      style={{
        flexDirection: 'row',
        flexWrap: 'wrap',
        alignItems: 'baseline',
      }}
    >
      {runs.map((r, i) => {
        const style = {
          color: r.code ? t.colors.code : r.link ? t.colors.link : baseColor,
          fontSize: baseSize,
          fontFamily: r.code ? t.fonts.mono : t.fonts.body,
          fontWeight: props.bold || r.bold ? 700 : 400,
          // fxe-ui doesn't expose strike/underline as text props yet — render
          // as suffix marks so the user can see the intent. (~~~ for strike,
          // _ for underline). When proper decorations land, swap these in.
        } as const;
        const decorated = r.strike
          ? `\u0336${r.text.split('').join('\u0336')}` // combining strike
          : r.underline
            ? r.text
                .split('')
                .map((ch) => `${ch}\u0332`)
                .join('')
            : r.text;
        if (r.link) {
          return (
            <Pressable
              key={`run-${i}`}
              onPress={() => {
                try {
                  // fxe:shell is gated on the `shell` capability; demo Window
                  // grants it via `permissions: { shell: true }`.
                  shell.openExternal(r.link as string);
                } catch {
                  // Capability denied — silently ignore in the demo.
                }
              }}
            >
              <Text style={{ ...style }}>{decorated}</Text>
            </Pressable>
          );
        }
        if (r.code) {
          return (
            <View
              key={`run-${i}`}
              style={{
                paddingX: 4,
                paddingY: 1,
                borderRadius: t.radii.sm,
                backgroundColor: t.colors.codeBg,
              }}
            >
              <Text style={style}>{decorated}</Text>
            </View>
          );
        }
        return (
          <Text key={`run-${i}`} style={style}>
            {decorated}
          </Text>
        );
      })}
    </View>
  );
}
const InlineRuns = memo(inlineRuns);

function mdHeading(props: { key?: string | number; node: FXEMarkdown.HeadingNode }): UiNode {
  const t = useTheme() as MarkdownTheme;
  const level = props.node.level;
  const sizeMap: Record<1 | 2 | 3 | 4 | 5 | 6, number> = {
    1: 30,
    2: 24,
    3: 20,
    4: 17,
    5: 15,
    6: 14,
  };
  return (
    <View
      style={{
        gap: t.spacing.xs,
        marginTop: level <= 2 ? t.spacing.lg : t.spacing.md,
        marginBottom: t.spacing.sm,
        borderBottomWidth: level <= 2 ? 1 : 0,
        borderBottomColor: t.colors.headingRule,
        paddingBottom: level <= 2 ? t.spacing.xs : 0,
      }}
    >
      <InlineRuns baseSize={sizeMap[level]} baseColor={t.colors.text} bold>
        {props.node.children}
      </InlineRuns>
    </View>
  );
}
const MdHeading = memo(mdHeading);

function mdParagraph(props: { key?: string | number; node: FXEMarkdown.ParagraphNode }): UiNode {
  const t = useTheme() as MarkdownTheme;
  return (
    <View style={{ marginBottom: t.spacing.sm }}>
      <InlineRuns>{props.node.children}</InlineRuns>
    </View>
  );
}
const MdParagraph = memo(mdParagraph);

function mdBlockquote(props: { key?: string | number; node: FXEMarkdown.BlockquoteNode }): UiNode {
  const t = useTheme() as MarkdownTheme;
  return (
    <View
      style={{
        borderLeftWidth: 3,
        borderLeftColor: t.colors.primary,
        paddingLeft: t.spacing.md,
        paddingY: t.spacing.xs,
        marginY: t.spacing.sm,
        gap: t.spacing.xs,
      }}
    >
      {props.node.children.map((c, i) => (
        <MdBlock key={`bq-${i}`} node={c} />
      ))}
    </View>
  );
}
const MdBlockquote = memo(mdBlockquote);

function mdList(props: {
  key?: string | number;
  node: FXEMarkdown.ListNode;
  depth: number;
}): UiNode {
  const t = useTheme() as MarkdownTheme;
  const start = props.node.ordered ? (props.node.start ?? 1) : 1;
  return (
    <View
      style={{
        marginY: t.spacing.xs,
        gap: props.node.tight ? 2 : t.spacing.xs,
        paddingLeft: props.depth === 0 ? 0 : t.spacing.md,
      }}
    >
      {props.node.children.map((item, i) => (
        <MdListItem
          key={`li-${i}`}
          node={item}
          ordered={props.node.ordered}
          marker={props.node.ordered ? `${start + i}.` : '•'}
          depth={props.depth}
        />
      ))}
    </View>
  );
}
const MdList = memo(mdList);

function mdListItem(props: {
  key?: string | number;
  node: FXEMarkdown.ListItemNode;
  ordered: boolean;
  marker: string;
  depth: number;
}): UiNode {
  const t = useTheme() as MarkdownTheme;
  const checkbox = props.node.task ? (props.node.checked ? '☑' : '☐') : null;
  return (
    <View style={{ flexDirection: 'row', gap: t.spacing.sm, alignItems: 'flex-start' }}>
      <Text
        style={{
          width: 24,
          color: t.colors.mutedText,
          fontSize: t.fontSizes.md,
          textAlign: 'right',
        }}
      >
        {checkbox ?? props.marker}
      </Text>
      <View style={{ flex: 1, gap: t.spacing.xs }}>
        {(() => {
          // Group consecutive inline siblings into one InlineRuns so they
          // wrap as a single line. A bare list item often emits inline
          // children directly (tight list); loose lists wrap in paragraphs.
          const out: UiNode[] = [];
          let inlineGroup: MdNode[] = [];
          const flush = () => {
            if (inlineGroup.length) {
              out.push(<InlineRuns key={`il-${out.length}`}>{inlineGroup}</InlineRuns>);
              inlineGroup = [];
            }
          };
          for (const c of props.node.children) {
            if (isInline(c)) inlineGroup.push(c);
            else {
              flush();
              out.push(<MdBlock key={`b-${out.length}`} node={c} depth={props.depth + 1} />);
            }
          }
          flush();
          return out;
        })()}
      </View>
    </View>
  );
}
const MdListItem = memo(mdListItem);

// Walk the syntax palette to color a tree-sitter capture name. The
// queries we ship emit names without dotted refinements (`comment`,
// `string`, …); if a future grammar emits e.g. `string.escape`, fall
// back to the prefix.
function syntaxColor(palette: SyntaxPalette, name: string, fallback: Color): Color {
  const direct = (palette as unknown as Record<string, number | undefined>)[name];
  if (typeof direct === 'number') return direct;
  const dot = name.indexOf('.');
  if (dot > 0) {
    const head = name.slice(0, dot);
    const prefix = (palette as unknown as Record<string, number | undefined>)[head];
    if (typeof prefix === 'number') return prefix;
  }
  return fallback;
}

interface CodeSpan {
  text: string;
  name?: string;
}

// Slice `source` into a flat list of spans where every code-unit index is
// covered: token ranges carry the capture name, gaps carry no name.
function buildSpans(source: string, tokens: FXEMarkdown.HighlightToken[]): CodeSpan[] {
  if (!tokens.length) return [{ text: source }];
  const out: CodeSpan[] = [];
  let cursor = 0;
  for (const tok of tokens) {
    if (tok.start > cursor) out.push({ text: source.slice(cursor, tok.start) });
    if (tok.end > tok.start) out.push({ text: source.slice(tok.start, tok.end), name: tok.name });
    cursor = tok.end;
  }
  if (cursor < source.length) out.push({ text: source.slice(cursor) });
  return out;
}

// Split spans on '\n' so each line lays out as its own row. Empty trailing
// lines (final newline) become an empty row so the gutter still shows.
function splitLines(spans: CodeSpan[]): CodeSpan[][] {
  const lines: CodeSpan[][] = [[]];
  for (const span of spans) {
    const parts = span.text.split('\n');
    for (let i = 0; i < parts.length; i++) {
      if (i > 0) lines.push([]);
      if (parts[i].length > 0) {
        lines[lines.length - 1].push({ text: parts[i], name: span.name });
      }
    }
  }
  return lines;
}

const TAB_COLUMNS = 4;

function expandCodeLineTabs(line: CodeSpan[]): CodeSpan[] {
  const out: CodeSpan[] = [];
  let column = 0;
  for (const span of line) {
    let text = '';
    for (const ch of span.text) {
      if (ch === '\t') {
        const spaces = TAB_COLUMNS - (column % TAB_COLUMNS);
        text += ' '.repeat(spaces);
        column += spaces;
      } else {
        text += ch;
        column += 1;
      }
    }
    if (text.length > 0) out.push({ text, name: span.name });
  }
  return out;
}

function codeLineText(line: CodeSpan[]): string {
  return line.map((span) => span.text).join('');
}

function codeLineWidth(line: CodeSpan[], fontSize: number): number {
  const text = codeLineText(line);
  return text.length === 0 ? 0 : Primitives.calcText(text, fontSize)[0];
}

function renderCodeSpans(
  line: CodeSpan[],
  palette: SyntaxPalette,
  fallback: Color,
  monoFamily: string,
  fontSize: number,
  lineHeight: number,
): UiNode[] {
  return line.map((span, si) => (
    <Text
      key={`s-${si}`}
      style={{
        height: lineHeight,
        color: span.name ? syntaxColor(palette, span.name, fallback) : fallback,
        fontFamily: monoFamily,
        fontSize,
        lineHeight,
      }}
    >
      {span.text}
    </Text>
  ));
}

function mdCodeBlock(props: { key?: string | number; node: FXEMarkdown.CodeBlockNode }): UiNode {
  const t = useTheme() as MarkdownTheme;
  const node = props.node;
  const lang = node.lang;
  const { lines, hasHighlighter } = useMemo(() => {
    const text = node.children.map((c) => c.text).join('');
    const hl = lang ? Markdown.highlight(text, lang) : null;
    const spans = hl ? buildSpans(text, hl.tokens) : [{ text }];
    return { lines: splitLines(spans).map(expandCodeLineTabs), hasHighlighter: !!hl };
  }, [node, lang]);
  const codeFontSize = t.fontSizes.sm + 1;
  const codeLineHeight = Primitives.calcText('M', codeFontSize)[1];
  const fixedLineHeight = Math.max(codeFontSize + 4, Math.ceil(codeLineHeight));
  const codeWidth = Math.ceil(
    Math.max(1, ...lines.map((line) => codeLineWidth(line, codeFontSize))),
  );
  return (
    <View
      style={{
        backgroundColor: t.colors.codeBg,
        borderRadius: t.radii.md,
        borderWidth: 1,
        borderColor: t.colors.border,
        padding: t.spacing.md,
        marginY: t.spacing.sm,
        gap: 0,
      }}
    >
      {lang ? (
        <Text
          style={{
            marginBottom: t.spacing.xs,
            color: t.colors.mutedText,
            fontSize: t.fontSizes.sm,
            lineHeight: fixedLineHeight,
          }}
        >
          {hasHighlighter ? lang : `${lang} (no highlighter)`}
        </Text>
      ) : null}
      {lines.map((line, li) => (
        <View
          key={`code-line-${li}`}
          style={{
            flexDirection: 'row',
            alignItems: 'flex-start',
            width: codeWidth,
            height: fixedLineHeight,
          }}
        >
          {renderCodeSpans(
            line,
            t.syntax,
            t.colors.text,
            t.fonts.mono,
            codeFontSize,
            fixedLineHeight,
          )}
        </View>
      ))}
    </View>
  );
}
const MdCodeBlock = memo(mdCodeBlock);

function mdThematicBreak(): UiNode {
  const t = useTheme() as MarkdownTheme;
  return (
    <View
      style={{
        height: 1,
        backgroundColor: t.colors.border,
        marginY: t.spacing.md,
      }}
    />
  );
}
const MdThematicBreak = memo(mdThematicBreak);

function mdTable(props: { key?: string | number; node: FXEMarkdown.TableNode }): UiNode {
  const t = useTheme() as MarkdownTheme;
  const rows: Array<{ row: FXEMarkdown.TableRowNode; head: boolean }> = [];
  for (const sec of props.node.children) {
    if (sec.type === 'table_head') {
      for (const r of sec.children) rows.push({ row: r, head: true });
    } else if (sec.type === 'table_body') {
      for (const r of sec.children) rows.push({ row: r, head: false });
    }
  }
  return (
    <View
      style={{
        borderWidth: 1,
        borderColor: t.colors.border,
        borderRadius: t.radii.sm,
        marginY: t.spacing.sm,
      }}
    >
      {rows.map((r, ri) => (
        <View
          key={`tr-${ri}`}
          style={{
            flexDirection: 'row',
            backgroundColor: r.head ? t.colors.tableHeaderBg : undefined,
            borderTopWidth: ri === 0 ? 0 : 1,
            borderTopColor: t.colors.border,
          }}
        >
          {r.row.children.map((cell, ci) => (
            <View
              key={`td-${ci}`}
              style={{
                flex: 1,
                padding: t.spacing.sm,
                borderLeftWidth: ci === 0 ? 0 : 1,
                borderLeftColor: t.colors.border,
              }}
            >
              <Text
                style={{
                  color: t.colors.text,
                  fontSize: t.fontSizes.md,
                  fontWeight: r.head ? 700 : 400,
                  textAlign:
                    cell.align === 'center' ? 'center' : cell.align === 'right' ? 'right' : 'left',
                }}
              >
                {inlineText(cell.children)}
              </Text>
            </View>
          ))}
        </View>
      ))}
    </View>
  );
}
const MdTable = memo(mdTable);

function mdHtmlBlock(props: { key?: string | number; node: FXEMarkdown.HtmlBlockNode }): UiNode {
  const t = useTheme() as MarkdownTheme;
  return (
    <View
      style={{
        backgroundColor: t.colors.codeBg,
        padding: t.spacing.sm,
        borderRadius: t.radii.sm,
        marginY: t.spacing.xs,
      }}
    >
      <Text style={{ color: t.colors.mutedText, fontFamily: t.fonts.mono, fontSize: 13 }}>
        {props.node.text}
      </Text>
    </View>
  );
}
const MdHtmlBlock = memo(mdHtmlBlock);

function mdBlock(props: { key?: string | number; node: MdNode; depth?: number }): UiNode {
  const depth = props.depth ?? 0;
  switch (props.node.type) {
    case 'paragraph':
      return <MdParagraph node={props.node} />;
    case 'heading':
      return <MdHeading node={props.node} />;
    case 'blockquote':
      return <MdBlockquote node={props.node} />;
    case 'list':
      return <MdList node={props.node} depth={depth} />;
    case 'list_item':
      // Should not be hit directly — lists render their own items.
      return null as unknown as UiNode;
    case 'code_block':
      return <MdCodeBlock node={props.node} />;
    case 'thematic_break':
      return <MdThematicBreak />;
    case 'table':
      return <MdTable node={props.node} />;
    case 'html_block':
      return <MdHtmlBlock node={props.node} />;
    default:
      // Inline node at block scope — wrap as a paragraph.
      if (isInline(props.node)) {
        return <InlineRuns>{[props.node]}</InlineRuns>;
      }
      return null as unknown as UiNode;
  }
}
const MdBlock = memo(mdBlock);

function markdownRenderer(props: { key?: string | number; source: string }): UiNode {
  const t = useTheme() as MarkdownTheme;
  const doc = useMemo(() => Markdown.parse(props.source), [props.source]);
  return (
    <View
      style={{
        padding: t.spacing.lg,
        backgroundColor: t.colors.surface,
        gap: 0,
      }}
    >
      {doc.children.map((c, i) => (
        <MdBlock key={`block-${i}`} node={c} />
      ))}
    </View>
  );
}
const MarkdownRenderer = memo(markdownRenderer);

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
  const toolbarDeps = useMemo(() => [themeId, 'toolbar'], [themeId]);
  const documentDeps = useMemo(() => [themeId, 'document'], [themeId]);
  const toolbar = Layer({
    key: 'toolbar-layer',
    deps: toolbarDeps,
    children: [
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
        {themeIds.map((id) => (
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
      </View>,
    ],
  });
  const document = Layer({
    key: 'document-layer',
    deps: documentDeps,
    children: [
      <View style={{ width: WINDOW_WIDTH, padding: theme.spacing.lg }}>
        <MarkdownRenderer source={SAMPLE_MD} />
      </View>,
    ],
  });
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
        {toolbar}
        <ScrollView
          style={{
            width: WINDOW_WIDTH,
            height: WINDOW_HEIGHT - TOOLBAR_HEIGHT,
            backgroundColor: theme.colors.background,
          }}
        >
          {document}
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
