// Gutter — line numbers and per-line markers (breakpoints, diagnostics, fold
// triangles). Sized to fit the maximum line number's width plus padding.
//
// Designed to share `lineHeight` and `scrollY` with a sibling LineViewport
// so both panels scroll in lock-step. The parent supplies a single
// `onScrollChange` handler (or none — read-only) and routes wheel events.

import { type Color, type CommandBuffer, Primitives } from 'fxe';
import { recordLayout } from '../debug/layout_trace.ts';
import { Component, type Node, useId } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { useTextStyle } from '../theme/text_context.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';
import { INTERNAL_LAYOUT, INTERNAL_TEXT_STYLE } from '../internal_keys.ts';

/** Per-line marker drawn left of the line number (e.g. breakpoint). */
export interface GutterMark {
  /** Filled circle at the supplied color. */
  color: Color;
  /** Diameter in logical pixels. Defaults to lineHeight * 0.5. */
  size?: number;
}

export type GutterMarkFn = (line: number) => GutterMark | null;

export interface GutterProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue;
  document: TextDocument;
  lineHeight: number;
  scrollY?: number;
  /** First line whose number to show (1-indexed in display). Default 1. */
  startLineNumber?: number;
  textColor?: Color;
  /** Highlight colour for the focused line number. Default = textColor. */
  focusedLineColor?: Color;
  focusedLine?: number;
  getMark?: GutterMarkFn;
}

export const Gutter = Component((props: GutterProps): Node => {
  useId();
  const inheritedText = useTextStyle();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = {
    ...inheritedText,
    ...(props[INTERNAL_TEXT_STYLE] ?? {}),
    ...resolved.text,
  };
  const rect = props[INTERNAL_LAYOUT]
    ? { ...props[INTERNAL_LAYOUT] }
    : rectFromStyle(resolved.layout, props[INTERNAL_LAYOUT]);
  recordLayout({
    component: 'Gutter',
    rect,
    hasParentLayout: props[INTERNAL_LAYOUT] !== undefined,
    styleWidth: resolved.layout.width,
    styleHeight: resolved.layout.height,
  });

  const lineHeight = props.lineHeight > 0 ? props.lineHeight : 18;
  const scrollY = Math.max(0, props.scrollY ?? 0);
  const fontSize = textStyle.fontSize ?? 12;
  const charW = fontSize * 0.6;
  const startLine = props.startLineNumber ?? 1;
  const totalLines = props.document.lineCount();
  const focusedLine = props.focusedLine;
  const textColor: Color = props.textColor ?? 0x88888888;
  const focusedColor: Color = props.focusedLineColor ?? 0xe0e0e0ff;

  const firstVisible = Math.max(0, Math.floor(scrollY / lineHeight) - 1);
  const lastVisible = Math.min(totalLines - 1, Math.ceil((scrollY + rect.height) / lineHeight) + 1);

  const paint = (cb: CommandBuffer): void => {
    const baselineOffset = lineHeight * 0.18;
    for (let line = firstVisible; line <= lastVisible; ++line) {
      const lineY = rect.y + line * lineHeight - scrollY;
      if (lineY > rect.y + rect.height) break;
      if (lineY + lineHeight < rect.y) continue;

      // Per-line mark left of the number.
      const mark = props.getMark ? props.getMark(line) : null;
      if (mark) {
        const size = mark.size ?? lineHeight * 0.5;
        Primitives.drawSelectionRects(
          cb,
          new Float32Array([rect.x + 4, lineY + (lineHeight - size) / 2, size, size]),
          mark.color,
        );
      }

      // Line number — right-aligned within rect.
      const num = String(line + startLine);
      const numWidth = num.length * charW;
      const x = rect.x + rect.width - numWidth - 6;
      const baselineY = lineY + baselineOffset;
      const isFocused = focusedLine !== undefined && focusedLine === line;
      Primitives.drawTextSpans(
        cb,
        x,
        baselineY,
        0,
        [{ text: num, color: isFocused ? focusedColor : textColor, size: fontSize }],
        { size: fontSize, color: textColor },
      );
    }
  };

  return {
    type: 'draw',
    props: {
      deps: [
        props.document,
        props.document.revision(),
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        lineHeight,
        scrollY,
        firstVisible,
        lastVisible,
        focusedLine,
        textColor,
        focusedColor,
        startLine,
        props.getMark,
      ],
      fn: paint,
    },
  } as Node;
}, 'Gutter');
