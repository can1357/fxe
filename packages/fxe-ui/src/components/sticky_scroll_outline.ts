import type { TextDocument } from 'fxe';

export interface OutlineEntry {
  line: number;
  depth: number;
  label?: string;
}

export interface OutlineProvider {
  getStickyEntries(doc: TextDocument, topVisibleLine: number, maxDepth: number): OutlineEntry[];
  revision?(doc: TextDocument): number;
}

export interface TreeSitterOutlineOptions {
  language: string;
  /** Capture names that mark a 'definition' line. Defaults to ['function','type','class','method','constructor','property']. */
  definitionCaptures?: ReadonlyArray<string>;
  tabWidth?: number;
}

const DEFAULT_DEFINITION_CAPTURES = Object.freeze([
  'function',
  'type',
  'class',
  'method',
  'constructor',
  'property',
]);
const MAX_TREE_SITTER_SOURCE_LENGTH = 1024 * 1024;

type TreeSitterOutlineCallback = (
  doc: TextDocument,
  topVisibleLine: number,
  maxDepth: number,
) => OutlineEntry[];

export function createIndentOutlineProvider(opts?: { tabWidth?: number }): OutlineProvider {
  const tabWidth = normalizeTabWidth(opts?.tabWidth);
  return {
    getStickyEntries(doc, topVisibleLine, maxDepth) {
      return getIndentStickyEntries(doc, topVisibleLine, maxDepth, tabWidth);
    },
    revision(doc) {
      return doc.revision();
    },
  };
}

export function createTreeSitterOutlineProvider(): OutlineProvider;
export function createTreeSitterOutlineProvider(opts: TreeSitterOutlineOptions): OutlineProvider;
export function createTreeSitterOutlineProvider(cb: TreeSitterOutlineCallback): OutlineProvider;
export function createTreeSitterOutlineProvider(
  arg?: TreeSitterOutlineOptions | TreeSitterOutlineCallback,
): OutlineProvider {
  if (typeof arg === 'function') {
    return {
      getStickyEntries(doc, topVisibleLine, maxDepth) {
        return arg(doc, topVisibleLine, maxDepth);
      },
    };
  }
  if (!arg) {
    return {
      getStickyEntries() {
        return [];
      },
    };
  }

  const tabWidth = normalizeTabWidth(arg.tabWidth);
  const definitionNames = new Set(
    (arg.definitionCaptures?.length ? arg.definitionCaptures : DEFAULT_DEFINITION_CAPTURES).filter(
      (name) => name.length > 0,
    ),
  );

  let cachedDoc: TextDocument | null = null;
  let cachedRevision = -1;
  let cachedDefinitionLines: ReadonlySet<number> | null = null;

  const getDefinitionLines = (doc: TextDocument): ReadonlySet<number> | null => {
    const revision = doc.revision();
    if (cachedDoc === doc && cachedRevision === revision) return cachedDefinitionLines;
    cachedDoc = doc;
    cachedRevision = revision;
    cachedDefinitionLines = computeDefinitionLines(doc, arg.language, definitionNames);
    return cachedDefinitionLines;
  };

  return {
    getStickyEntries(doc, topVisibleLine, maxDepth) {
      const definitionLines = getDefinitionLines(doc);
      if (definitionLines === null) {
        return getIndentStickyEntries(doc, topVisibleLine, maxDepth, tabWidth);
      }
      return getTreeSitterStickyEntries(doc, topVisibleLine, maxDepth, tabWidth, definitionLines);
    },
    revision(doc) {
      return doc.revision();
    },
  };
}

export function getIndentStickyEntries(
  doc: TextDocument,
  topVisibleLine: number,
  maxDepth: number,
  tabWidth = 2,
): OutlineEntry[] {
  return collectStickyEntries(doc, topVisibleLine, maxDepth, tabWidth);
}

function getTreeSitterStickyEntries(
  doc: TextDocument,
  topVisibleLine: number,
  maxDepth: number,
  tabWidth: number,
  definitionLines: ReadonlySet<number>,
): OutlineEntry[] {
  return collectStickyEntries(doc, topVisibleLine, maxDepth, tabWidth, (line) =>
    definitionLines.has(line),
  );
}

function collectStickyEntries(
  doc: TextDocument,
  topVisibleLine: number,
  maxDepth: number,
  tabWidth: number,
  includeLine?: (line: number) => boolean,
): OutlineEntry[] {
  const safeMaxDepth = Math.max(0, maxDepth | 0);
  if (safeMaxDepth === 0) return [];

  const totalLines = doc.lineCount();
  if (totalLines === 0 || topVisibleLine <= 0 || topVisibleLine >= totalLines) return [];

  const referenceIndent = findReferenceIndent(doc, topVisibleLine, tabWidth);
  if (referenceIndent === null) return [];

  const openers: Array<{ line: number; indent: number }> = [];
  let minIndent = referenceIndent;
  for (let line = topVisibleLine - 1; line >= 0; --line) {
    const info = getLineInfo(doc, line, tabWidth);
    if (!info || info.commentOnly) continue;
    if (info.indent < minIndent) {
      if (!includeLine || includeLine(line)) {
        openers.push({ line, indent: info.indent });
      }
      minIndent = info.indent;
      if (openers.length >= safeMaxDepth || minIndent === 0) break;
    }
  }

  openers.reverse();
  return openers.map((entry, index) => ({ line: entry.line, depth: index }));
}

function computeDefinitionLines(
  doc: TextDocument,
  language: string,
  definitionNames: ReadonlySet<string>,
): ReadonlySet<number> | null {
  const markdown = (globalThis as { Markdown?: typeof Markdown }).Markdown;
  if (!markdown || typeof markdown.highlight !== 'function') return null;

  const source = doc.text();
  if (source.length > MAX_TREE_SITTER_SOURCE_LENGTH) return null;

  const result = markdown.highlight(source, language);
  if (!result) return null;

  const definitionLines = new Set<number>();
  const maxOffset = doc.length();
  for (const token of result.tokens) {
    if (!matchesDefinitionCapture(definitionNames, token.name)) continue;
    const start = clampOffset(token.start, maxOffset);
    const end = clampOffset(Math.max(token.start, token.end - 1), maxOffset);
    const startLine = doc.offsetToLine(start);
    const endLine = doc.offsetToLine(end);
    for (let line = startLine; line <= endLine; ++line) {
      definitionLines.add(line);
    }
  }
  return definitionLines;
}

function matchesDefinitionCapture(
  definitionNames: ReadonlySet<string>,
  captureName: string,
): boolean {
  if (definitionNames.has(captureName)) return true;
  const dot = captureName.indexOf('.');
  return dot !== -1 && definitionNames.has(captureName.slice(0, dot));
}

function clampOffset(offset: number, maxOffset: number): number {
  return Math.max(0, Math.min(maxOffset, offset));
}

function findReferenceIndent(
  doc: TextDocument,
  startLine: number,
  tabWidth: number,
): number | null {
  for (let line = startLine; line < doc.lineCount(); ++line) {
    const info = getLineInfo(doc, line, tabWidth);
    if (!info || info.commentOnly) continue;
    return info.indent;
  }
  return null;
}

type LineInfo = {
  indent: number;
  commentOnly: boolean;
};

function getLineInfo(doc: TextDocument, line: number, tabWidth: number): LineInfo | null {
  const text = doc.lineText(line);
  if (text.trim().length === 0) return null;
  const trimmed = text.trimStart();
  return {
    indent: leadingIndentColumn(text, tabWidth),
    commentOnly: isCommentOnlyLine(trimmed),
  };
}

function leadingIndentColumn(text: string, tabWidth: number): number {
  let column = 0;
  for (let i = 0; i < text.length; ++i) {
    const ch = text.charCodeAt(i);
    if (ch === 32) {
      column += 1;
      continue;
    }
    if (ch === 9) {
      column += tabWidth;
      continue;
    }
    break;
  }
  return column;
}

function isCommentOnlyLine(trimmed: string): boolean {
  return (
    trimmed.startsWith('//') ||
    trimmed.startsWith('/*') ||
    trimmed.startsWith('*/') ||
    trimmed.startsWith('*') ||
    trimmed.startsWith('--') ||
    trimmed.startsWith(';')
  );
}

function normalizeTabWidth(tabWidth: number | undefined): number {
  return Number.isFinite(tabWidth) && tabWidth && tabWidth > 0 ? Math.floor(tabWidth) : 2;
}
