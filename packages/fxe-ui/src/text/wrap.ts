// Greedy word-wrap helper for fxe-ui Text/View.
//
// FXE exposes native text helpers next to the font stack. ASCII text uses that
// bulk path to avoid repeated JS/native metric calls; non-ASCII falls back to
// the JS path so code-unit indices stay correct for full Unicode strings.
// Single words wider than `maxWidth` are emitted on their own line (not
// char-broken) since natural prose rarely needs sub-word breaks; the caller
// can opt-in by setting `breakWords: true`.

import { Primitives } from 'fxe';
import type { TextStyle } from '../style/types.ts';

// Sub-pixel slack absorbs the rounding the layout solver applies to measured
// widths (see `round()` in layout/solver.ts). Without it, paint-time wrap
// would re-wrap at a width fractionally smaller than what the measure pass
// used and pop the trailing word onto a fresh line.
const FIT_EPS = 0.5;

export interface WrappedText {
  lines: string[];
  width: number; // widest measured line width (excludes letterSpacing tail)
  height: number; // lineHeight * lines.length
  lineHeight: number;
  lineStartIndices: number[]; // code-unit index in the original text for each line start
}

export interface WrapOptions {
  maxWidth?: number;
  breakWords?: boolean;
}

function nativeWrapText(
  text: string,
  fontSize: number,
  letterSpacing: number,
  maxWidth: number,
  lineHeight: number | undefined,
  breakWords: boolean,
): WrappedText | null {
  return Primitives.wrapTextNative(
    text,
    fontSize,
    letterSpacing,
    maxWidth,
    lineHeight,
    breakWords,
  ) as WrappedText | null;
}
function nativeXAtGlyphIndex(
  text: string,
  fontSize: number,
  letterSpacing: number,
  idx: number,
): number | null {
  return Primitives.xAtGlyphIndexNative(text, fontSize, letterSpacing, idx) as number | null;
}

function nativeGlyphIndexAt(
  text: string,
  fontSize: number,
  letterSpacing: number,
  x: number,
): number | null {
  return Primitives.glyphIndexAtNative(text, fontSize, letterSpacing, x) as number | null;
}

function measureLineWidth(text: string, fontSize: number, letterSpacing: number): number {
  if (text.length === 0) return 0;
  const [w] = Primitives.calcText(text, fontSize);
  return w + Math.max(0, text.length - 1) * letterSpacing;
}

// Memo for the (text, fontSize, letterSpacing, idx===text.length) case —
// paint emits one measuredWidth() call per wrapped line per text per frame.
// Lines come straight out of the wrapText cache so their string identity
// is stable; key is therefore safe to compose.
const TAIL_WIDTH_CACHE = new Map<string, number>();
const TAIL_WIDTH_CACHE_MAX = 4096;
let g_tail_width_old = new Map<string, number>();

export function xAtGlyphIndex(text: string, style: TextStyle, idx: number): number {
  const fontSize = style.fontSize ?? 16;
  const letterSpacing = style.letterSpacing ?? 0;
  // Hot path: full-line measurement for alignment and selection edges.
  if (idx === text.length) {
    const k = `${fontSize}|${letterSpacing}|${text}`;
    const hit = TAIL_WIDTH_CACHE.get(k);
    if (hit !== undefined) return hit;
    const old = g_tail_width_old.get(k);
    if (old !== undefined) {
      TAIL_WIDTH_CACHE.set(k, old);
      return old;
    }
    const native = nativeXAtGlyphIndex(text, fontSize, letterSpacing, idx);
    const v = native !== null ? native : measureLineWidth(text, fontSize, letterSpacing);
    TAIL_WIDTH_CACHE.set(k, v);
    if (TAIL_WIDTH_CACHE.size >= TAIL_WIDTH_CACHE_MAX) {
      g_tail_width_old = TAIL_WIDTH_CACHE;
      TAIL_WIDTH_CACHE.clear();
    }
    return v;
  }
  const native = nativeXAtGlyphIndex(text, fontSize, letterSpacing, idx);
  if (native !== null) return native;
  const clamped = Math.max(0, Math.min(Math.trunc(idx), text.length));
  return measureLineWidth(text.slice(0, clamped), fontSize, letterSpacing);
}

export function glyphIndexAt(text: string, style: TextStyle, x: number): number {
  if (text.length === 0 || x <= 0) return 0;
  if (!Number.isFinite(x)) return text.length;
  const fontSize = style.fontSize ?? 16;
  const letterSpacing = style.letterSpacing ?? 0;
  const native = nativeGlyphIndexAt(text, fontSize, letterSpacing, x);
  if (native !== null) return native;
  let lo = 0;
  let hi = text.length;
  while (lo < hi) {
    const mid = Math.floor((lo + hi) / 2);
    const boundary = (xAtGlyphIndex(text, style, mid) + xAtGlyphIndex(text, style, mid + 1)) / 2;
    if (x < boundary) hi = mid;
    else lo = mid + 1;
  }
  return lo;
}

/**
 * Maps wrapped-text local coordinates to a source UTF-16 code-unit index.
 */
export function pointToTextIndex(
  wrapped: WrappedText,
  style: TextStyle,
  x: number,
  y: number,
): number {
  if (wrapped.lines.length === 0) return 0;
  const lineIdx = Math.max(
    0,
    Math.min(Math.floor(y / wrapped.lineHeight), wrapped.lines.length - 1),
  );
  const localIdx = glyphIndexAt(wrapped.lines[lineIdx], style, x);
  // Soft-wrap EOL and next-line BOF can share the same source index; that's acceptable for caret placement.
  // Hard newlines are already reflected in `lineStartIndices`.
  return (wrapped.lineStartIndices[lineIdx] ?? 0) + localIdx;
}

/**
 * Maps a source UTF-16 code-unit index to wrapped-text local coordinates.
 */
export function textIndexToPoint(
  wrapped: WrappedText,
  style: TextStyle,
  idx: number,
): { x: number; y: number } {
  if (wrapped.lines.length === 0) return { x: 0, y: 0 };
  let i = wrapped.lines.length - 1;
  for (; i > 0; i--) {
    if ((wrapped.lineStartIndices[i] ?? 0) <= idx) break;
  }
  const start = wrapped.lineStartIndices[i] ?? 0;
  const localIdx = Math.max(0, Math.min(idx - start, wrapped.lines[i].length));
  const x = xAtGlyphIndex(wrapped.lines[i], style, localIdx);
  const y = i * wrapped.lineHeight;
  return { x, y };
}

function breakLongWord(
  word: string,
  fontSize: number,
  letterSpacing: number,
  limit: number,
): string[] {
  const out: string[] = [];
  let buffer = '';
  for (const ch of word) {
    const next = buffer + ch;
    if (measureLineWidth(next, fontSize, letterSpacing) <= limit + FIT_EPS || buffer.length === 0) {
      buffer = next;
    } else {
      out.push(buffer);
      buffer = ch;
    }
  }
  if (buffer.length > 0) out.push(buffer);
  return out;
}

// Wrap result cache. wrapText() runs through the native HarfBuzz/CoreText
// shaper for every Text/TextInput on every layout pass, so a stress scene
// with N text nodes burns N native shape calls per frame even when the
// inputs don't change. The vast majority of UI text is static across
// frames; this memo turns those repeats into a Map probe.
//
// We swap-evict instead of LRU: when `g_cache` reaches MAX, demote it to
// `g_old` and start fresh. Lookups check the new cache first, then fall
// back to the old one (and promote on hit). Memory is bounded to 2*MAX.
// Diagnostic counters; observable via globalThis.__fxeWrapStats() so we
// can verify the cache is actually firing in production.
let g_wrap_hits = 0;
let g_wrap_misses = 0;
// biome-ignore lint/suspicious/noExplicitAny: dev-only diagnostic shim.
(globalThis as any).__fxeWrapStats = () => ({
  hits: g_wrap_hits,
  misses: g_wrap_misses,
  size: g_wrap_cache.size,
  oldSize: g_wrap_cache_old.size,
});
const WRAP_CACHE_MAX = 4096;
let g_wrap_cache = new Map<string, WrappedText>();
let g_wrap_cache_old = new Map<string, WrappedText>();

function wrapKey(
  text: string,
  fontSize: number,
  letterSpacing: number,
  lineHeight: number | undefined,
  maxWidth: number | undefined,
  breakWords: boolean,
): string {
  // Pipe is rarer than other separators; the join is one V8 string concat
  // and V8 will intern this for the cache key. Float bit patterns survive
  // the concat unambiguously.
  return `${fontSize}|${letterSpacing}|${lineHeight ?? -1}|${maxWidth ?? -1}|${breakWords ? 1 : 0}|${text}`;
}

function cachedWrap(key: string): WrappedText | undefined {
  const hit = g_wrap_cache.get(key);
  if (hit !== undefined) return hit;
  const old = g_wrap_cache_old.get(key);
  if (old !== undefined) {
    g_wrap_cache.set(key, old);
    if (g_wrap_cache.size >= WRAP_CACHE_MAX) {
      g_wrap_cache_old = g_wrap_cache;
      g_wrap_cache = new Map();
    }
    return old;
  }
  return undefined;
}

function storeWrap(key: string, value: WrappedText): WrappedText {
  g_wrap_cache.set(key, value);
  if (g_wrap_cache.size >= WRAP_CACHE_MAX) {
    g_wrap_cache_old = g_wrap_cache;
    g_wrap_cache = new Map();
  }
  return value;
}

// Test/dev hook for invalidating the cache (exposed so the font binding
// can flush it after a font swap; safe to leave unused).
export function _resetWrapCache(): void {
  g_wrap_cache = new Map();
  g_wrap_cache_old = new Map();
}

export function wrapText(text: string, style: TextStyle, options: WrapOptions = {}): WrappedText {
  const fontSize = style.fontSize ?? 16;
  const letterSpacing = style.letterSpacing ?? 0;
  const breakWords = options.breakWords === true;
  const key = wrapKey(
    text,
    fontSize,
    letterSpacing,
    style.lineHeight,
    options.maxWidth,
    breakWords,
  );
  const cached = cachedWrap(key);
  if (cached !== undefined) {
    g_wrap_hits++;
    return cached;
  }
  g_wrap_misses++;
  const native = nativeWrapText(
    text,
    fontSize,
    letterSpacing,
    options.maxWidth ?? Number.POSITIVE_INFINITY,
    style.lineHeight,
    breakWords,
  );
  if (native !== null) return storeWrap(key, native);
  // `calcText('')` is well-defined in the FXE binding (returns height of the
  // active font), but we substitute 'M' to make sure we always get a real
  // glyph metric for empty inputs.
  const probe = text.length > 0 ? text : 'M';
  const baseHeight = Primitives.calcText(probe, fontSize)[1];
  const lineHeight = style.lineHeight ?? baseHeight;
  const limit =
    options.maxWidth !== undefined && Number.isFinite(options.maxWidth) && options.maxWidth > 0
      ? options.maxWidth
      : Number.POSITIVE_INFINITY;

  if (text.length === 0) {
    return storeWrap(key, {
      lines: [''],
      width: 0,
      height: lineHeight,
      lineHeight,
      lineStartIndices: [0],
    });
  }

  const paragraphs = text.split('\n');
  const lines: string[] = [];
  const lineStartIndices: number[] = [];
  let widest = 0;
  let paragraphStart = 0;

  const pushLine = (line: string, start: number): void => {
    lines.push(line);
    lineStartIndices.push(Math.max(0, Math.min(start, text.length)));
    widest = Math.max(widest, measureLineWidth(line, fontSize, letterSpacing));
  };

  for (const paragraph of paragraphs) {
    if (paragraph.length === 0) {
      pushLine('', paragraphStart);
      paragraphStart += 1;
      continue;
    }
    if (limit === Number.POSITIVE_INFINITY) {
      pushLine(paragraph, paragraphStart);
      paragraphStart += paragraph.length + 1;
      continue;
    }
    // Tokenize on whitespace runs and non-whitespace runs, preserving the
    // separators. Markdown/code renderers depend on leading, trailing, and
    // repeated spaces having measurable width; collapsing them makes adjacent
    // styled Text runs visually concatenate.
    const tokens = paragraph.match(/\S+|\s+/g) ?? [];
    let current = '';
    let currentStart = paragraphStart;
    let tokenOffset = 0;
    for (let token of tokens) {
      const tokenStart = paragraphStart + tokenOffset;
      tokenOffset += token.length;
      const isSpaceRun = /^\s+$/.test(token);
      const candidate = current + token;
      if (measureLineWidth(candidate, fontSize, letterSpacing) <= limit + FIT_EPS) {
        if (current.length === 0) currentStart = tokenStart;
        current = candidate;
        continue;
      }
      if (current.length > 0) {
        pushLine(current, currentStart);
        current = '';
      }
      // `token` is now alone on its line. If it overflows and breakWords is on,
      // split non-space tokens across multiple lines; otherwise let it overflow
      // horizontally. Space runs stay intact so indentation remains visible.
      if (
        !isSpaceRun &&
        options.breakWords &&
        measureLineWidth(token, fontSize, letterSpacing) > limit + FIT_EPS &&
        token.length > 1
      ) {
        const pieces = breakLongWord(token, fontSize, letterSpacing, limit);
        let pieceStart = tokenStart;
        for (let i = 0; i < pieces.length - 1; i++) {
          pushLine(pieces[i], pieceStart);
          pieceStart += pieces[i].length;
        }
        token = pieces[pieces.length - 1] ?? '';
        currentStart = pieceStart;
      } else {
        currentStart = tokenStart;
      }
      current = token;
    }
    if (current.length > 0) {
      pushLine(current, currentStart);
    } else if (lines.length === 0) {
      pushLine('', paragraphStart);
    }
    paragraphStart += paragraph.length + 1;
  }

  if (lines.length === 0) pushLine('', 0);
  return storeWrap(key, {
    lines,
    width: widest,
    height: lineHeight * lines.length,
    lineHeight,
    lineStartIndices,
  });
}
