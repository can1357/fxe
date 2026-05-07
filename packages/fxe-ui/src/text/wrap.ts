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

export function xAtGlyphIndex(text: string, style: TextStyle, idx: number): number {
  const fontSize = style.fontSize ?? 16;
  const letterSpacing = style.letterSpacing ?? 0;
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

export function wrapText(text: string, style: TextStyle, options: WrapOptions = {}): WrappedText {
  const fontSize = style.fontSize ?? 16;
  const letterSpacing = style.letterSpacing ?? 0;
  const native = nativeWrapText(
    text,
    fontSize,
    letterSpacing,
    options.maxWidth ?? Number.POSITIVE_INFINITY,
    style.lineHeight,
    options.breakWords === true,
  );
  if (native !== null) return native;
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
    return { lines: [''], width: 0, height: lineHeight, lineHeight, lineStartIndices: [0] };
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
    // Tokenize on whitespace runs but preserve them as separators so we don't
    // collapse intentional double-spaces inside a single line.
    const words = paragraph.split(/\s+/).filter((w) => w.length > 0);
    let current = '';
    let currentStart = paragraphStart;
    let searchFrom = paragraphStart;
    for (let word of words) {
      const wordStart = text.indexOf(word, searchFrom);
      const safeWordStart = wordStart >= 0 ? wordStart : searchFrom;
      const candidate = current.length === 0 ? word : `${current} ${word}`;
      if (measureLineWidth(candidate, fontSize, letterSpacing) <= limit + FIT_EPS) {
        if (current.length === 0) currentStart = safeWordStart;
        current = candidate;
        searchFrom = safeWordStart + word.length;
        continue;
      }
      if (current.length > 0) {
        pushLine(current, currentStart);
        current = '';
      }
      // `word` is now alone on its line. If it overflows and breakWords is on,
      // split it across multiple lines; otherwise let it overflow horizontally.
      if (
        options.breakWords &&
        measureLineWidth(word, fontSize, letterSpacing) > limit + FIT_EPS &&
        word.length > 1
      ) {
        const pieces = breakLongWord(word, fontSize, letterSpacing, limit);
        let pieceStart = safeWordStart;
        for (let i = 0; i < pieces.length - 1; i++) {
          pushLine(pieces[i], pieceStart);
          pieceStart += pieces[i].length;
        }
        word = pieces[pieces.length - 1] ?? '';
        currentStart = pieceStart;
      } else {
        currentStart = safeWordStart;
      }
      current = word;
      searchFrom = safeWordStart + word.length;
    }
    if (current.length > 0) {
      pushLine(current, currentStart);
    } else if (lines.length === 0) {
      pushLine('', paragraphStart);
    }
    paragraphStart += paragraph.length + 1;
  }

  if (lines.length === 0) pushLine('', 0);
  return { lines, width: widest, height: lineHeight * lines.length, lineHeight, lineStartIndices };
}
