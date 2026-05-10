// IncrementalHighlighter adapts native Markdown.highlight output to the
// existing LineViewport `getLineDecorations` contract; LineViewport itself
// does not need any highlighting-specific changes.

import type { LineDecorations, LineSpan } from '../components/LineViewport.ts';

export type CaptureName = string;

export interface HighlightStyle {
  color?: number;
  bold?: boolean;
  italic?: boolean;
  underline?: boolean;
  strikethrough?: boolean;
}

export type HighlightTheme =
  | ReadonlyMap<CaptureName, HighlightStyle>
  | Record<CaptureName, HighlightStyle>;

export interface IncrementalHighlighterOptions {
  document: TextDocument;
  language: string;
  theme: HighlightTheme;
  defaultStyle?: HighlightStyle;
}

export interface IncrementalHighlighter {
  getLineDecorations: (line: number) => LineDecorations | null;
  invalidate(): void;
  revision(): number;
  dispose(): void;
}

type SplitHighlightToken = {
  start: number;
  end: number;
  name: string;
};

const EMPTY_SPANS: readonly LineSpan[] = Object.freeze([]);

export function createIncrementalHighlighter(
  opts: IncrementalHighlighterOptions,
): IncrementalHighlighter {
  const { document, language, theme, defaultStyle } = opts;
  const lineCache = new Map<number, ReadonlyArray<LineSpan>>();
  const tokensByLine = new Map<number, SplitHighlightToken[]>();

  let disposed = false;
  let highlightedRevision = -1;
  let fullDocRevision = -1;
  let bucketedRevision = -1;
  let fullDocTokens: ReadonlyArray<FXEMarkdown.HighlightToken> | null = null;

  const subscriptionId = document.subscribe((edits) => {
    if (disposed) return;

    let firstAffected = document.lineCount() - 1;
    for (const edit of edits) {
      const endOffset = edit.start + Math.max(edit.removed, edit.inserted.length);
      const startLine = document.offsetToLine(clampOffset(document, edit.start));
      const endLine = document.offsetToLine(clampOffset(document, endOffset));
      firstAffected = Math.min(firstAffected, startLine, endLine);
    }

    dropCachedLinesFrom(lineCache, firstAffected);
    fullDocRevision = -1;
    bucketedRevision = -1;
    fullDocTokens = null;
    tokensByLine.clear();
  });

  const invalidate = (): void => {
    lineCache.clear();
    tokensByLine.clear();
    fullDocRevision = -1;
    bucketedRevision = -1;
    fullDocTokens = null;
    highlightedRevision = -1;
  };

  const ensureHighlighted = (): void => {
    const currentRevision = document.revision();
    if (fullDocRevision === currentRevision) return;

    const result = Markdown.highlight(document.text(), language);
    fullDocTokens = result?.tokens ?? null;
    fullDocRevision = currentRevision;
    bucketedRevision = -1;
    tokensByLine.clear();
    highlightedRevision = currentRevision;
  };

  const ensureTokensByLine = (): void => {
    if (fullDocTokens === null || bucketedRevision === fullDocRevision) return;

    tokensByLine.clear();
    for (const token of fullDocTokens) {
      const startLine = document.offsetToLine(clampOffset(document, token.start));
      const endLine = document.offsetToLine(
        clampOffset(document, Math.max(token.start, token.end - 1)),
      );
      for (let line = startLine; line <= endLine; ++line) {
        const range = document.lineRange(line);
        const start = Math.max(token.start, range.start);
        const end = Math.min(token.end, range.end);
        if (end <= start) continue;
        const bucket = tokensByLine.get(line);
        const split = { start, end, name: token.name };
        if (bucket) {
          bucket.push(split);
        } else {
          tokensByLine.set(line, [split]);
        }
      }
    }

    bucketedRevision = fullDocRevision;
  };

  const getLineDecorations = (line: number): LineDecorations | null => {
    if (disposed) return null;
    if (line < 0 || line >= document.lineCount()) return null;

    const cached = lineCache.get(line);
    if (cached !== undefined) {
      return cached.length > 0 ? { spans: cached } : null;
    }

    ensureHighlighted();
    if (fullDocTokens === null) {
      lineCache.set(line, EMPTY_SPANS);
      return null;
    }

    ensureTokensByLine();
    const range = document.lineRange(line);
    const tokens = tokensByLine.get(line) ?? [];
    const spans = tokens
      .map((token) => {
        const style = resolveStyle(theme, token.name) ?? defaultStyle;
        return {
          start: token.start - range.start,
          end: token.end - range.start,
          ...(style ?? {}),
        } satisfies LineSpan;
      })
      .sort((a, b) => a.start - b.start);

    const readonlySpans = spans.length > 0 ? spans : EMPTY_SPANS;
    lineCache.set(line, readonlySpans);
    return spans.length > 0 ? { spans: readonlySpans } : null;
  };

  const dispose = (): void => {
    if (disposed) return;
    disposed = true;
    document.unsubscribe(subscriptionId);
    lineCache.clear();
    tokensByLine.clear();
    fullDocRevision = -1;
    bucketedRevision = -1;
    fullDocTokens = null;
    highlightedRevision = -1;
  };

  return {
    getLineDecorations,
    invalidate,
    revision: () => highlightedRevision,
    dispose,
  };
}

function clampOffset(document: TextDocument, offset: number): number {
  return Math.max(0, Math.min(document.length(), offset));
}

function dropCachedLinesFrom(
  cache: Map<number, ReadonlyArray<LineSpan>>,
  firstAffected: number,
): void {
  for (const line of cache.keys()) {
    if (line >= firstAffected) cache.delete(line);
  }
}

function resolveStyle(theme: HighlightTheme, captureName: string): HighlightStyle | undefined {
  const exact = readThemeStyle(theme, captureName);
  if (exact !== undefined) return exact;

  const dot = captureName.indexOf('.');
  if (dot === -1) return undefined;
  return readThemeStyle(theme, captureName.slice(0, dot));
}

function readThemeStyle(theme: HighlightTheme, captureName: string): HighlightStyle | undefined {
  if (theme instanceof Map) {
    return theme.get(captureName);
  }
  return Object.hasOwn(theme, captureName) ? theme[captureName] : undefined;
}
