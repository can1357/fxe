import { type Color, type CommandBuffer, Primitives } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';
import type { TextStyle } from '../style/types.ts';
import { wrapText, xAtGlyphIndex } from '../text/wrap.ts';

export interface TextPreeditOptions {
  text: string;
  cursor: number;
  underlineColor?: Color;
}

/**
 * Selection range in UTF-16 indices.
 */
export interface TextSelectionOptions {
  start: number;
  end: number;
  /** Selection highlight color (defaults to `0x3b82f654`). */
  color?: Color;
}

/**
 * Painter-only text rendering options.
 *
 * `caretOpacity` lets callers drive caret blink animation over time. The
 * painter is stateless and applies opacity only to the caret draw.
 */
export interface PaintTextOptions {
  imePreedit?: TextPreeditOptions;
  selection?: TextSelectionOptions;
  caretIndex?: number;
  caretColor?: Color;
  caretOpacity?: number;
}

function measuredWidth(text: string, style: TextStyle): number {
  return xAtGlyphIndex(text, style, text.length);
}

function alignedX(rect: LayoutResult, lineWidth: number, align: TextStyle['textAlign']): number {
  if (align === 'center' && rect.width > 0)
    return rect.x + Math.max(0, (rect.width - lineWidth) / 2);
  if (align === 'right' && rect.width > 0) return rect.x + Math.max(0, rect.width - lineWidth);
  return rect.x;
}

function scaleAlpha(color: number, factor: number): number {
  const a = (color & 0xff) * factor;
  return ((color >>> 8) << 8) | Math.max(0, Math.min(255, Math.round(a)));
}

export function paintText(
  cb: CommandBuffer,
  rect: LayoutResult,
  text: string,
  style: TextStyle,
  options: PaintTextOptions = {},
): void {
  const preedit = options.imePreedit;
  if (text.length === 0 && !preedit?.text && options.caretIndex === undefined) return;
  const fontSize = style.fontSize ?? 16;
  const color = style.color ?? 0xffffffff;
  const align = style.textAlign ?? 'left';
  const selection = options.selection;
  const selectionStart =
    selection === undefined
      ? 0
      : Math.max(0, Math.min(selection.start, selection.end, text.length));
  const selectionEnd =
    selection === undefined
      ? 0
      : Math.max(0, Math.min(Math.max(selection.start, selection.end), text.length));
  const selectionColor = selection?.color ?? 0x3b82f654;
  const caretIndex =
    options.caretIndex === undefined
      ? undefined
      : Math.max(0, Math.min(Math.trunc(options.caretIndex), text.length));
  const caretColor = options.caretColor ?? color;
  let caretDrawn = false;
  const caretOpacity =
    options.caretOpacity === undefined
      ? 1
      : Number.isFinite(options.caretOpacity)
        ? Math.max(0, Math.min(1, options.caretOpacity))
        : 1;
  const visibleCaretColor = scaleAlpha(caretColor, caretOpacity);
  const wrapWidth = rect.width > 0 ? rect.width : undefined;
  let caretAnchorX: number | undefined;
  let caretAnchorY: number | undefined;
  const wrapped = wrapText(text, style, { maxWidth: wrapWidth });
  const baseY = rect.y;
  let caretBaseX = alignedX(rect, 0, align);
  let caretY = baseY;
  // Collect text lines and emit them in a single drawTextRun() call to skip
  // a V8 trampoline per line. Selection / caret rects still emit inline so
  // their z-order relative to the text stays correct (they render before
  // the run, behind the glyphs).
  const runs: { x: number; y: number; text: string; size: number; color: Color }[] = [];
  for (let i = 0; i < wrapped.lines.length; i++) {
    const line = wrapped.lines[i];
    const lineStart = wrapped.lineStartIndices[i] ?? 0;
    const lineWidth = measuredWidth(line, style);
    const x = alignedX(rect, lineWidth, align);
    caretBaseX = x + lineWidth;
    caretY = baseY + i * wrapped.lineHeight;
    if (selection && selectionEnd > selectionStart) {
      const startInLine = Math.max(0, Math.min(selectionStart - lineStart, line.length));
      const endInLine = Math.max(0, Math.min(selectionEnd - lineStart, line.length));
      if (
        endInLine > startInLine ||
        (line.length === 0 && selectionStart <= lineStart && selectionEnd > lineStart)
      ) {
        const selX = x + xAtGlyphIndex(line, style, startInLine);
        const selEndX = x + xAtGlyphIndex(line, style, endInLine);
        Primitives.fillRect(
          cb,
          selX,
          caretY,
          Math.max(1, selEndX - selX),
          wrapped.lineHeight,
          0,
          selectionColor,
        );
      }
    }
    if (caretIndex !== undefined && !caretDrawn) {
      const caretInLine = Math.max(0, Math.min(caretIndex - lineStart, line.length));
      if (caretIndex >= lineStart && caretIndex <= lineStart + line.length) {
        const caretX = x + xAtGlyphIndex(line, style, caretInLine);
        caretAnchorX = caretX;
        caretAnchorY = caretY;
        if (caretOpacity > 0) {
          Primitives.fillRect(cb, caretX, caretY, 1, wrapped.lineHeight, 0, visibleCaretColor);
        }
        caretDrawn = true;
      }
    }
    if (line.length === 0) continue;
    runs.push({ x, y: caretY, text: line, size: fontSize, color });
  }
  if (runs.length === 1) {
    // Avoid the array-iteration cost in the binding for the single-line case
    // (overwhelmingly common for UI labels).
    const r = runs[0];
    Primitives.drawText(cb, r.x, r.y, 0, r.text, r.size, r.color);
  } else if (runs.length > 1) {
    Primitives.drawTextRun(cb, runs);
  }

  if (!preedit?.text) return;
  const preeditWidth = measuredWidth(preedit.text, style);
  const cursor = Math.max(0, Math.min(preedit.cursor, preedit.text.length));
  const preeditCursorWidth = measuredWidth(preedit.text.slice(0, cursor), style);
  const preeditX = caretAnchorX ?? caretBaseX;
  const preeditY = caretAnchorY ?? caretY;
  // v1 behavior: when preedit starts inside existing text, draw it over any
  // trailing glyphs instead of reflowing / shifting the buffer tail.
  Primitives.drawText(cb, preeditX, preeditY, 0, preedit.text, fontSize, color);
  Primitives.fillRect(
    cb,
    preeditX,
    preeditY + fontSize + 1,
    Math.max(1, preeditWidth),
    1,
    0,
    preedit.underlineColor ?? color,
  );
  Primitives.fillRect(cb, preeditX + preeditCursorWidth, preeditY, 1, fontSize, 0, color);
}
