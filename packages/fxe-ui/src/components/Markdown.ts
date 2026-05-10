/**
 * Markdown renderer for fxe-ui.
 *
 * `useTheme()` only guarantees the base `Theme` surface, so this component
 * derives markdown-specific colors/fonts/syntax from that base before merging
 * `props.theme` on top. Callers can pass a plain app theme and still get a
 * complete, renderable markdown palette.
 */
import { Primitives } from 'fxe';
import {
  Component,
  createContext,
  memo,
  type Node,
  useContext,
  useMemo,
} from '../reconciler/fiber.ts';
import type { Color, StyleValue } from '../style/types.ts';
import { type Theme, ThemeProvider, useTheme } from '../theme/index.ts';
import { Pressable } from './Pressable.ts';
import { Text } from './Text.ts';
import { View } from './View.ts';

export interface SyntaxPalette {
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

export interface MarkdownTheme extends Theme {
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

export interface MarkdownProps {
  key?: string;
  source: string;
  style?: StyleValue;
  theme?: Partial<MarkdownTheme>;
  onLinkPress?(href: string): void;
  onWikilinkPress?(target: string): void;
}

type MdNode = FXEMarkdown.Node;
type ThemeOverride = Partial<MarkdownTheme> & {
  colors?: Partial<MarkdownTheme['colors']>;
  fonts?: Partial<MarkdownTheme['fonts']>;
  syntax?: Partial<SyntaxPalette>;
};

type MarkdownHandlers = {
  onLinkPress: (href: string) => void;
  onWikilinkPress: (target: string) => void;
};

type Inline = {
  text: string;
  bold?: boolean;
  italic?: boolean;
  strike?: boolean;
  underline?: boolean;
  code?: boolean;
  link?: string;
  wikilink?: boolean;
};

type CodeSpan = {
  text: string;
  name?: string;
};

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

const emptyDocument: FXEMarkdown.DocumentNode = { type: 'document', children: [] };
const TAB_COLUMNS = 4;

export const defaultSyntaxPalette: SyntaxPalette = {
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
};

const MarkdownHandlersContext = createContext<MarkdownHandlers>({
  onLinkPress(href: string): void {
    try {
      shell.openExternal(href);
    } catch {}
  },
  onWikilinkPress(target: string): void {
    try {
      shell.openExternal(target);
    } catch {}
  },
});

function useMarkdownHandlers(): MarkdownHandlers {
  return useContext(MarkdownHandlersContext);
}

function isInline(node: MdNode): boolean {
  return inlineKinds.includes(node.type);
}

function flatten(node: MdNode, acc: Inline[], style: Inline = { text: '' }): void {
  switch (node.type) {
    case 'text':
    case 'entity':
    case 'null_char':
      acc.push({ ...style, text: node.text });
      return;
    case 'soft_break':
      acc.push({ ...style, text: ' ' });
      return;
    case 'hard_break':
      acc.push({ ...style, text: '\n' });
      return;
    case 'raw_html':
      acc.push({ ...style, text: node.text });
      return;
    case 'emph':
      for (const child of node.children) flatten(child, acc, { ...style, italic: true });
      return;
    case 'strong':
      for (const child of node.children) flatten(child, acc, { ...style, bold: true });
      return;
    case 'strikethrough':
      for (const child of node.children) flatten(child, acc, { ...style, strike: true });
      return;
    case 'underline':
      for (const child of node.children) flatten(child, acc, { ...style, underline: true });
      return;
    case 'code_span': {
      const text = node.children.map((child) => child.text).join('');
      acc.push({ ...style, text, code: true });
      return;
    }
    case 'link':
      for (const child of node.children) flatten(child, acc, { ...style, link: node.href });
      return;
    case 'image': {
      const alt: Inline[] = [];
      for (const child of node.children) flatten(child, alt, style);
      const merged = alt.map((run) => run.text).join('');
      acc.push({ ...style, text: `🖼 ${merged || node.src}`, italic: true });
      return;
    }
    case 'wikilink': {
      const inner: Inline[] = [];
      for (const child of node.children) flatten(child, inner, style);
      const text = inner.map((run) => run.text).join('') || node.target;
      acc.push({ ...style, text, link: node.target, wikilink: true });
      return;
    }
    default:
      return;
  }
}

function inlineText(children: ReadonlyArray<MdNode>): string {
  const runs: Inline[] = [];
  for (const child of children) flatten(child, runs);
  return runs.map((run) => run.text).join('');
}

function syntaxColor(palette: SyntaxPalette, name: string, fallback: Color): Color {
  const direct = (palette as Record<string, number | undefined>)[name];
  if (typeof direct === 'number') return direct;
  const dot = name.indexOf('.');
  if (dot > 0) {
    const prefix = (palette as Record<string, number | undefined>)[name.slice(0, dot)];
    if (typeof prefix === 'number') return prefix;
  }
  return fallback;
}

function buildSpans(source: string, tokens: FXEMarkdown.HighlightToken[]): CodeSpan[] {
  if (tokens.length === 0) return [{ text: source }];
  const spans: CodeSpan[] = [];
  let cursor = 0;
  for (const token of tokens) {
    if (token.start > cursor) spans.push({ text: source.slice(cursor, token.start) });
    if (token.end > token.start) {
      spans.push({ text: source.slice(token.start, token.end), name: token.name });
    }
    cursor = token.end;
  }
  if (cursor < source.length) spans.push({ text: source.slice(cursor) });
  return spans;
}

function splitLines(spans: CodeSpan[]): CodeSpan[][] {
  const lines: CodeSpan[][] = [[]];
  for (const span of spans) {
    const parts = span.text.split('\n');
    for (let i = 0; i < parts.length; ++i) {
      if (i > 0) lines.push([]);
      if (parts[i].length > 0) lines[lines.length - 1]?.push({ text: parts[i], name: span.name });
    }
  }
  return lines;
}

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
): Node[] {
  return line.map((span, index) =>
    Text({
      key: `code-span-${index}`,
      style: {
        height: lineHeight,
        color: span.name ? syntaxColor(palette, span.name, fallback) : fallback,
        fontFamily: monoFamily,
        fontSize,
        lineHeight,
      },
      children: span.text,
    }),
  );
}

function resolveMarkdownTheme(baseTheme: Theme, override?: Partial<MarkdownTheme>): MarkdownTheme {
  const themeOverride = (override ?? {}) as ThemeOverride;
  const baseColors = baseTheme.colors;
  const overrideColors = themeOverride.colors ?? {};
  const baseFonts = (baseTheme as Partial<MarkdownTheme>).fonts;
  const overrideFonts = themeOverride.fonts ?? {};
  const overrideSyntax = themeOverride.syntax ?? {};

  return {
    ...baseTheme,
    ...themeOverride,
    colors: {
      ...baseColors,
      code: baseColors.primary,
      codeBg: baseColors.surface,
      quote: baseColors.mutedText,
      link: baseColors.primary,
      headingRule: baseColors.border,
      tableHeaderBg: baseColors.surface,
      ...overrideColors,
    },
    fonts: {
      body: baseFonts?.body ?? 'system-ui',
      mono: baseFonts?.mono ?? 'Menlo',
      ...overrideFonts,
    },
    syntax: {
      ...defaultSyntaxPalette,
      ...(baseTheme as Partial<MarkdownTheme>).syntax,
      ...overrideSyntax,
    },
  };
}

interface InlineRunsProps {
  key?: string;
  children: ReadonlyArray<MdNode>;
  baseSize?: number;
  baseColor?: Color;
  bold?: boolean;
}

const InlineRuns = memo(
  Component((props: InlineRunsProps): Node => {
    const theme = useTheme() as MarkdownTheme;
    const handlers = useMarkdownHandlers();
    const runs = useMemo(() => {
      const acc: Inline[] = [];
      for (const child of props.children) flatten(child, acc);
      return acc;
    }, [props.children]);
    const baseSize = props.baseSize ?? theme.fontSizes.md;
    const baseColor = props.baseColor ?? theme.colors.text;

    return View({
      style: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'baseline' },
      children: runs.map((run, index) => {
        const style = {
          color: run.code ? theme.colors.code : run.link ? theme.colors.link : baseColor,
          fontSize: baseSize,
          fontFamily: run.code ? theme.fonts.mono : theme.fonts.body,
          fontWeight: props.bold || run.bold ? 700 : 400,
        } as const;
        const decorated = run.strike
          ? `\u0336${run.text.split('').join('\u0336')}`
          : run.underline
            ? run.text
                .split('')
                .map((ch) => `${ch}\u0332`)
                .join('')
            : run.text;

        if (run.link) {
          return Pressable({
            key: `inline-link-${index}`,
            onPress: () => {
              if (run.wikilink) handlers.onWikilinkPress(run.link as string);
              else handlers.onLinkPress(run.link as string);
            },
            children: Text({ style, children: decorated }),
          });
        }

        if (run.code) {
          return View({
            key: `inline-code-${index}`,
            style: {
              paddingX: 4,
              paddingY: 1,
              borderRadius: theme.radii.sm,
              backgroundColor: theme.colors.codeBg,
            },
            children: Text({ style, children: decorated }),
          });
        }

        return Text({ key: `inline-text-${index}`, style, children: decorated });
      }),
    });
  }, 'InlineRuns'),
);

const MdHeading = memo(
  Component((props: { key?: string; node: FXEMarkdown.HeadingNode }): Node => {
    const theme = useTheme() as MarkdownTheme;
    const level = props.node.level;
    const sizeMap: Record<1 | 2 | 3 | 4 | 5 | 6, number> = {
      1: 30,
      2: 24,
      3: 20,
      4: 17,
      5: 15,
      6: 14,
    };
    return View({
      style: {
        gap: theme.spacing.xs,
        marginTop: level <= 2 ? theme.spacing.lg : theme.spacing.md,
        marginBottom: theme.spacing.sm,
        borderBottomWidth: level <= 2 ? 1 : 0,
        borderBottomColor: theme.colors.headingRule,
        paddingBottom: level <= 2 ? theme.spacing.xs : 0,
      },
      children: InlineRuns({
        baseSize: sizeMap[level],
        baseColor: theme.colors.text,
        bold: true,
        children: props.node.children,
      }),
    });
  }, 'MdHeading'),
);

const MdParagraph = memo(
  Component((props: { key?: string; node: FXEMarkdown.ParagraphNode }): Node => {
    const theme = useTheme() as MarkdownTheme;
    return View({
      style: { marginBottom: theme.spacing.sm },
      children: InlineRuns({ children: props.node.children }),
    });
  }, 'MdParagraph'),
);

const MdBlockquote = memo(
  Component((props: { key?: string; node: FXEMarkdown.BlockquoteNode }): Node => {
    const theme = useTheme() as MarkdownTheme;
    return View({
      style: {
        borderLeftWidth: 3,
        borderLeftColor: theme.colors.primary,
        paddingLeft: theme.spacing.md,
        paddingY: theme.spacing.xs,
        marginY: theme.spacing.sm,
        gap: theme.spacing.xs,
      },
      children: props.node.children.map((child, index) =>
        MdBlock({ key: `blockquote-${index}`, node: child }),
      ),
    });
  }, 'MdBlockquote'),
);

const MdList = memo(
  Component((props: { key?: string; node: FXEMarkdown.ListNode; depth: number }): Node => {
    const theme = useTheme() as MarkdownTheme;
    const start = props.node.ordered ? (props.node.start ?? 1) : 1;
    return View({
      style: {
        marginY: theme.spacing.xs,
        gap: props.node.tight ? 2 : theme.spacing.xs,
        paddingLeft: props.depth === 0 ? 0 : theme.spacing.md,
      },
      children: props.node.children.map((item, index) =>
        MdListItem({
          key: `list-item-${index}`,
          node: item,
          ordered: props.node.ordered,
          marker: props.node.ordered ? `${start + index}.` : '•',
          depth: props.depth,
        }),
      ),
    });
  }, 'MdList'),
);

const MdListItem = memo(
  Component(
    (props: {
      key?: string;
      node: FXEMarkdown.ListItemNode;
      ordered: boolean;
      marker: string;
      depth: number;
    }): Node => {
      const theme = useTheme() as MarkdownTheme;
      const checkbox = props.node.task ? (props.node.checked ? '☑' : '☐') : null;
      const out: Node[] = [];
      let inlineGroup: MdNode[] = [];
      const flush = (): void => {
        if (inlineGroup.length === 0) return;
        out.push(InlineRuns({ key: `inline-group-${out.length}`, children: inlineGroup }));
        inlineGroup = [];
      };
      for (const child of props.node.children) {
        if (isInline(child)) inlineGroup.push(child);
        else {
          flush();
          out.push(
            MdBlock({ key: `list-block-${out.length}`, node: child, depth: props.depth + 1 }),
          );
        }
      }
      flush();

      return View({
        style: { flexDirection: 'row', gap: theme.spacing.sm, alignItems: 'flex-start' },
        children: [
          Text({
            key: 'marker',
            style: {
              width: 24,
              color: theme.colors.mutedText,
              fontSize: theme.fontSizes.md,
              textAlign: 'right',
            },
            children: checkbox ?? props.marker,
          }),
          View({ key: 'body', style: { flex: 1, gap: theme.spacing.xs }, children: out }),
        ],
      });
    },
    'MdListItem',
  ),
);

const MdCodeBlock = memo(
  Component((props: { key?: string; node: FXEMarkdown.CodeBlockNode }): Node => {
    const theme = useTheme() as MarkdownTheme;
    const node = props.node;
    const lang = node.lang;
    const { lines, hasHighlighter } = useMemo(() => {
      const text = node.children.map((child) => child.text).join('');
      const highlight = lang ? globalThis.Markdown.highlight(text, lang) : null;
      const spans = highlight ? buildSpans(text, highlight.tokens) : [{ text }];
      return {
        lines: splitLines(spans).map(expandCodeLineTabs),
        hasHighlighter: highlight !== null,
      };
    }, [node, lang]);
    const codeFontSize = theme.fontSizes.sm + 1;
    const codeLineHeight = Primitives.calcText('M', codeFontSize)[1];
    const fixedLineHeight = Math.max(codeFontSize + 4, Math.ceil(codeLineHeight));
    const codeWidth = Math.ceil(
      Math.max(1, ...lines.map((line) => codeLineWidth(line, codeFontSize))),
    );

    return View({
      style: {
        backgroundColor: theme.colors.codeBg,
        borderRadius: theme.radii.md,
        borderWidth: 1,
        borderColor: theme.colors.border,
        padding: theme.spacing.md,
        marginY: theme.spacing.sm,
        gap: 0,
      },
      children: [
        ...(lang
          ? [
              Text({
                key: 'language',
                style: {
                  marginBottom: theme.spacing.xs,
                  color: theme.colors.mutedText,
                  fontSize: theme.fontSizes.sm,
                  lineHeight: fixedLineHeight,
                },
                children: hasHighlighter ? lang : `${lang} (no highlighter)`,
              }),
            ]
          : []),
        ...lines.map((line, lineIndex) =>
          View({
            key: `code-line-${lineIndex}`,
            style: {
              flexDirection: 'row',
              alignItems: 'flex-start',
              width: codeWidth,
              height: fixedLineHeight,
            },
            children: renderCodeSpans(
              line,
              theme.syntax,
              theme.colors.text,
              theme.fonts.mono,
              codeFontSize,
              fixedLineHeight,
            ),
          }),
        ),
      ],
    });
  }, 'MdCodeBlock'),
);

const MdThematicBreak = memo(
  Component((): Node => {
    const theme = useTheme() as MarkdownTheme;
    return View({
      style: {
        height: 1,
        backgroundColor: theme.colors.border,
        marginY: theme.spacing.md,
      },
    });
  }, 'MdThematicBreak'),
);

const MdTable = memo(
  Component((props: { key?: string; node: FXEMarkdown.TableNode }): Node => {
    const theme = useTheme() as MarkdownTheme;
    const rows: Array<{ row: FXEMarkdown.TableRowNode; head: boolean }> = [];
    for (const section of props.node.children) {
      if (section.type === 'table_head') {
        for (const row of section.children) rows.push({ row, head: true });
      } else if (section.type === 'table_body') {
        for (const row of section.children) rows.push({ row, head: false });
      }
    }

    return View({
      style: {
        borderWidth: 1,
        borderColor: theme.colors.border,
        borderRadius: theme.radii.sm,
        marginY: theme.spacing.sm,
      },
      children: rows.map((entry, rowIndex) =>
        View({
          key: `table-row-${rowIndex}`,
          style: {
            flexDirection: 'row',
            backgroundColor: entry.head ? theme.colors.tableHeaderBg : undefined,
            borderTopWidth: rowIndex === 0 ? 0 : 1,
            borderTopColor: theme.colors.border,
          },
          children: entry.row.children.map((cell, cellIndex) =>
            View({
              key: `table-cell-${cellIndex}`,
              style: {
                flex: 1,
                padding: theme.spacing.sm,
                borderLeftWidth: cellIndex === 0 ? 0 : 1,
                borderLeftColor: theme.colors.border,
              },
              children: Text({
                style: {
                  color: theme.colors.text,
                  fontSize: theme.fontSizes.md,
                  fontWeight: entry.head ? 700 : 400,
                  textAlign:
                    cell.align === 'center' ? 'center' : cell.align === 'right' ? 'right' : 'left',
                },
                children: inlineText(cell.children),
              }),
            }),
          ),
        }),
      ),
    });
  }, 'MdTable'),
);

const MdHtmlBlock = memo(
  Component((props: { key?: string; node: FXEMarkdown.HtmlBlockNode }): Node => {
    const theme = useTheme() as MarkdownTheme;
    return View({
      style: {
        backgroundColor: theme.colors.codeBg,
        padding: theme.spacing.sm,
        borderRadius: theme.radii.sm,
        marginY: theme.spacing.xs,
      },
      children: Text({
        style: { color: theme.colors.mutedText, fontFamily: theme.fonts.mono, fontSize: 13 },
        children: props.node.text,
      }),
    });
  }, 'MdHtmlBlock'),
);

const MdBlock = memo(
  Component((props: { key?: string; node: MdNode; depth?: number }): Node => {
    const depth = props.depth ?? 0;
    switch (props.node.type) {
      case 'paragraph':
        return MdParagraph({ node: props.node });
      case 'heading':
        return MdHeading({ node: props.node });
      case 'blockquote':
        return MdBlockquote({ node: props.node });
      case 'list':
        return MdList({ node: props.node, depth });
      case 'list_item':
        return null as unknown as Node;
      case 'code_block':
        return MdCodeBlock({ node: props.node });
      case 'thematic_break':
        return MdThematicBreak({});
      case 'table':
        return MdTable({ node: props.node });
      case 'html_block':
        return MdHtmlBlock({ node: props.node });
      default:
        if (isInline(props.node)) return InlineRuns({ children: [props.node] });
        return null as unknown as Node;
    }
  }, 'MdBlock'),
);

export const Markdown = Component((props: MarkdownProps): Node => {
  const baseTheme = useTheme();
  const theme = resolveMarkdownTheme(baseTheme, props.theme);
  const onLinkPress =
    props.onLinkPress ??
    ((href: string): void => {
      try {
        shell.openExternal(href);
      } catch {}
    });
  const onWikilinkPress = props.onWikilinkPress ?? onLinkPress;
  // The component is also named `Markdown`, so refer to the native parser via
  // `globalThis.Markdown` to avoid shadowing the ambient runtime binding.
  const doc = useMemo(() => {
    try {
      return globalThis.Markdown.parse(props.source);
    } catch {
      return emptyDocument;
    }
  }, [props.source]);

  return ThemeProvider({
    value: theme,
    children: MarkdownHandlersContext.Provider({
      value: { onLinkPress, onWikilinkPress },
      children: View({
        style: [
          { padding: theme.spacing.lg, backgroundColor: theme.colors.surface, gap: 0 },
          props.style,
        ],
        children: doc.children.map((child, index) =>
          MdBlock({ key: `block-${index}`, node: child }),
        ),
      }),
    }),
  });
}, 'Markdown');
