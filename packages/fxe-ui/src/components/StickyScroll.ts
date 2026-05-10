import { type Color, Primitives, type TextDocument } from 'fxe';
import {
  Component,
  Draw,
  type Node,
  useInternalLayout,
  useInternalTextStyle,
  useMemo,
} from '../reconciler/fiber.ts';
import type { Style } from '../style/types.ts';
import { useTextStyle } from '../theme/index.ts';
import { useTheme } from '../theme/provider.ts';
import type { LineSpan } from './LineViewport.ts';
import {
  createIndentOutlineProvider,
  type OutlineEntry,
  type OutlineProvider,
} from './sticky_scroll_outline.ts';
import { View } from './View.ts';

export interface StickyScrollProps {
  key?: string;
  doc: TextDocument;
  scrollOffset: number;
  lineHeight: number;
  width: number;
  outline: OutlineProvider;
  maxDepth?: number;
  getLineSpans?: (line: number) => readonly LineSpan[];
  textColor?: number;
  backgroundColor?: number;
  borderColor?: number;
  style?: Style;
  testID?: string;
}

const DEFAULT_LINE_HEIGHT = 18;

export function stickyScrollTopVisibleLine(scrollOffset: number, lineHeight: number): number {
  return Math.max(0, Math.floor(Math.max(0, scrollOffset) / Math.max(1, lineHeight)));
}

export function resolveStickyScrollEntries(
  doc: TextDocument,
  outline: OutlineProvider,
  scrollOffset: number,
  lineHeight: number,
  maxDepth = 4,
): OutlineEntry[] {
  const topVisibleLine = stickyScrollTopVisibleLine(scrollOffset, lineHeight);
  if (topVisibleLine === 0) return [];
  return outline.getStickyEntries(doc, topVisibleLine, maxDepth);
}

const StickyScrollCanvas = Component(
  (props: {
    key?: string;
    style?: Style;
    doc: TextDocument;
    entries: readonly OutlineEntry[];
    lineHeight: number;
    getLineSpans?: (line: number) => readonly LineSpan[];
    textColor: Color;
  }): Node => {
    const inheritedText = useTextStyle();
    const internalText = useInternalTextStyle();
    const fontSize =
      internalText?.fontSize ??
      inheritedText.fontSize ??
      (typeof props.style?.fontSize === 'number' ? props.style.fontSize : 14);
    const rect = useInternalLayout();

    return Draw(
      (cb) => {
        if (!rect) return;
        const baselineOffset = props.lineHeight * 0.18;
        for (let index = 0; index < props.entries.length; ++index) {
          const entry = props.entries[index];
          const lineY = rect.y + index * props.lineHeight;
          const baselineY = lineY + baselineOffset;
          const lineText = entry.label ?? props.doc.lineText(entry.line);
          if (lineText.length === 0) continue;
          const spans = props.getLineSpans?.(entry.line);
          Primitives.drawTextSpans(
            cb,
            rect.x,
            baselineY,
            0,
            buildDrawSpans(lineText, spans, props.textColor, fontSize),
            { size: fontSize, color: props.textColor },
          );
        }
      },
      [
        props.doc,
        props.doc.revision(),
        props.entries,
        props.lineHeight,
        props.getLineSpans,
        props.textColor,
        rect,
        fontSize,
      ],
    );
  },
  'StickyScrollCanvas',
);

export const StickyScroll = Component((props: StickyScrollProps): Node => {
  const theme = useTheme();
  const defaultOutline = useMemo(() => createIndentOutlineProvider(), []);
  const outline = props.outline ?? defaultOutline;
  const lineHeight = props.lineHeight > 0 ? props.lineHeight : DEFAULT_LINE_HEIGHT;
  const maxDepth = props.maxDepth ?? 4;
  const topVisibleLine = stickyScrollTopVisibleLine(props.scrollOffset, lineHeight);
  const outlineRevision = outline.revision?.(props.doc) ?? 0;
  const entries = useMemo(
    () => resolveStickyScrollEntries(props.doc, outline, props.scrollOffset, lineHeight, maxDepth),
    [props.doc.revision(), outlineRevision, topVisibleLine, maxDepth, outline],
  );

  const resolvedTextColor: Color =
    props.textColor ?? (typeof theme.colors.text === 'number' ? theme.colors.text : 0xf4f6fbff);
  const baseStyle: Style = {
    position: 'absolute',
    top: 0,
    left: 0,
    width: props.width,
    height: entries.length === 0 || topVisibleLine === 0 ? 0 : entries.length * lineHeight + 1,
    backgroundColor: props.backgroundColor ?? theme.colors.surface,
    borderBottomWidth: entries.length === 0 || topVisibleLine === 0 ? 0 : 1,
    borderBottomColor: props.borderColor ?? theme.colors.border,
    overflow: 'hidden',
    pointerEvents: 'none',
  };

  if (entries.length === 0 || topVisibleLine === 0) {
    return View({ key: props.key, style: [baseStyle, props.style] });
  }

  return View({
    key: props.key,
    style: [baseStyle, props.style],
    children: StickyScrollCanvas({
      key: props.testID,
      style: { width: '100%', height: '100%', color: resolvedTextColor },
      doc: props.doc,
      entries,
      lineHeight,
      getLineSpans: props.getLineSpans,
      textColor: resolvedTextColor,
    }),
  });
}, 'StickyScroll');

function buildDrawSpans(
  lineText: string,
  spans: readonly LineSpan[] | undefined,
  textColor: Color,
  fontSize: number,
): Array<{
  text: string;
  color?: Color;
  size?: number;
  bold?: boolean;
  italic?: boolean;
  underline?: boolean;
  strikethrough?: boolean;
}> {
  if (!spans || spans.length === 0) {
    return [{ text: lineText, color: textColor, size: fontSize }];
  }

  const drawSpans: Array<{
    text: string;
    color?: Color;
    size?: number;
    bold?: boolean;
    italic?: boolean;
    underline?: boolean;
    strikethrough?: boolean;
  }> = [];
  let cursor = 0;
  for (const span of spans) {
    const start = Math.max(cursor, span.start);
    const end = Math.min(lineText.length, span.end);
    if (end <= start) continue;
    if (start > cursor) {
      drawSpans.push({ text: lineText.slice(cursor, start), color: textColor, size: fontSize });
    }
    drawSpans.push({
      text: lineText.slice(start, end),
      color: span.color ?? textColor,
      size: fontSize,
      bold: span.bold,
      italic: span.italic,
      underline: span.underline,
      strikethrough: span.strikethrough,
    });
    cursor = end;
  }
  if (cursor < lineText.length) {
    drawSpans.push({ text: lineText.slice(cursor), color: textColor, size: fontSize });
  }
  return drawSpans;
}
