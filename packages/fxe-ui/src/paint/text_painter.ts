import { type Color, type CommandBuffer, Primitives } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';
import type { TextStyle } from '../style/types.ts';
import { wrapText, xAtGlyphIndex } from '../text/wrap.ts';

export interface TextPreeditOptions {
  text: string;
  cursor: number;
  underlineColor?: Color;
}

export interface TextSelectionOptions {
  start: number;
  end: number;
  color?: Color;
}

export interface PaintTextOptions {
  imePreedit?: TextPreeditOptions;
  selection?: TextSelectionOptions;
  caretIndex?: number;
  caretColor?: Color;
}

function measuredWidth(text: string, style: TextStyle, fontSize: number): number {
  if (text.length === 0) return 0;
  const [width] = Primitives.calcText(text, fontSize);
  return width + Math.max(0, text.length - 1) * (style.letterSpacing ?? 0);
}

function alignedX(rect: LayoutResult, lineWidth: number, align: TextStyle['textAlign']): number {
  if (align === 'center' && rect.width > 0)
    return rect.x + Math.max(0, (rect.width - lineWidth) / 2);
  if (align === 'right' && rect.width > 0) return rect.x + Math.max(0, rect.width - lineWidth);
  return rect.x;
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
  const wrapWidth = rect.width > 0 ? rect.width : undefined;
  const wrapped = wrapText(text, style, { maxWidth: wrapWidth });
  const baseY = rect.y;
  let caretBaseX = alignedX(rect, 0, align);
  let caretY = baseY;
  for (let i = 0; i < wrapped.lines.length; i++) {
    const line = wrapped.lines[i];
    const lineStart = wrapped.lineStartIndices[i] ?? 0;
    const lineWidth = measuredWidth(line, style, fontSize);
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
        Primitives.fillRect(cb, caretX, caretY, 1, wrapped.lineHeight, 0, caretColor);
        caretDrawn = true;
      }
    }
    if (line.length === 0) continue;
    Primitives.drawText(cb, x, caretY, 0, line, fontSize, color);
  }

  if (!preedit?.text) return;
  const preeditWidth = measuredWidth(preedit.text, style, fontSize);
  const cursor = Math.max(0, Math.min(preedit.cursor, preedit.text.length));
  const preeditCursorWidth = measuredWidth(preedit.text.slice(0, cursor), style, fontSize);
  Primitives.drawText(cb, caretBaseX, caretY, 0, preedit.text, fontSize, color);
  Primitives.fillRect(
    cb,
    caretBaseX,
    caretY + fontSize + 1,
    Math.max(1, preeditWidth),
    1,
    0,
    preedit.underlineColor ?? color,
  );
  Primitives.fillRect(cb, caretBaseX + preeditCursorWidth, caretY, 1, fontSize, 0, color);
}
