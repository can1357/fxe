// LineViewport — line-organised virtualization over a native TextDocument.
//
// Why this exists separate from <VirtualList>:
//   - VirtualList is row-generic; an editor wants per-line painting with
//     stable monospace line heights and shared scroll geometry across
//     viewport / gutter / minimap.
//   - Lines come from the document's native line index (O(log P)), not a
//     JS array.
//   - Decoration overlays (selection, diagnostics, find matches, search
//     highlights) paint AFTER text via a single `drawSelectionRects` call,
//     so 100 selection rects cost one V8 trampoline.
//   - Edit-keystroke invalidation tracks `doc.revision()`, not span
//     objects; unchanged frames re-use the cached Layer paint output.

import {
  type Color,
  type CommandBuffer,
  type MouseButtonEvent,
  Primitives,
  type TextDocument,
} from 'fxe';
import { recordLayout } from '../debug/layout_trace.ts';
import { registerHitTarget, type SyntheticEvent } from '../mount/hit_test.ts';
import {
  Component,
  type Node,
  useEffect,
  useId,
  useInternalLayout,
  useInternalTextStyle,
  useState,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { useTextStyle } from '../theme/text_context.ts';
import { rectFromStyle } from './common.ts';

/** A single styled run on one line. Cols are UTF-16 code-unit indices. */
export interface LineSpan {
  start: number;
  end: number;
  color?: Color;
  bold?: boolean;
  italic?: boolean;
  underline?: boolean;
  strikethrough?: boolean;
}

export interface DiagnosticUnderline {
  x1: number;
  x2: number;
  style: 'solid' | 'dashed' | 'dotted' | 'wavy';
  color?: Color;
  thickness?: number;
}

/** Per-line decoration payload returned by `getLineDecorations`. */
export interface LineDecorations {
  /** Styled spans for the line, sorted ascending by `start`. */
  spans?: ReadonlyArray<LineSpan>;
  /** Selection rects in line-local coords: [x, y, w, h, ...]. */
  selectionRects?: Float32Array;
  /** Background highlight (e.g. current line). */
  background?: Color;
  /** Diagnostic underlines in line-local x coords (y is the baseline). */
  diagnostics?: ReadonlyArray<DiagnosticUnderline>;
}

export type LineDecorationFn = (line: number) => LineDecorations | null;

export interface LineViewportProps {
  key?: string;
  style?: StyleValue;
  document: TextDocument;
  /** Logical pixel height per line (monospace assumption). */
  lineHeight: number;
  /** Provider for per-line decorations. Called per visible line per frame. */
  getLineDecorations?: LineDecorationFn;
  /** Number of extra lines to render above/below viewport. Default 4. */
  overscan?: number;
  /** Tab stop in pixels. 0 = render TAB literally. */
  tabSize?: number;
  /** Visualise tabs/spaces with faint marks. */
  showWhitespace?: boolean;
  /** Default text colour when a span doesn't override. */
  textColor?: Color;
  /** Y scroll offset in logical pixels. */
  scrollY?: number;
  /** Invoked on a viewport click with the resolved {line, col}. */
  onClickPosition?: (line: number, col: number, ev: SyntheticEvent) => void;
  /** Invoked on mouse-move while the primary button stays held over this viewport. */
  onDragPosition?: (line: number, col: number, ev: SyntheticEvent) => void;
  /** Invoked when the primary-button press that began in this viewport ends. */
  onPressUp?: (line: number, col: number, ev: SyntheticEvent<MouseButtonEvent>) => void;
}

const DEFAULT_OVERSCAN = 4;
const DEFAULT_SELECTION_COLOR: Color = 0x3b82f654;

export const LineViewport = Component((props: LineViewportProps): Node => {
  const id = useId();
  const inheritedText = useTextStyle();
  const internalText = useInternalTextStyle();
  const internalLayout = useInternalLayout();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = {
    ...inheritedText,
    ...(internalText ?? {}),
    ...resolved.text,
  };
  const rect = internalLayout ? { ...internalLayout } : rectFromStyle(resolved.layout, undefined);
  recordLayout({
    component: 'LineViewport',
    rect,
    hasParentLayout: internalLayout !== null,
    styleWidth: resolved.layout.width,
    styleHeight: resolved.layout.height,
  });

  // Re-render when the document mutates. Track revision so unchanged frames
  // skip the diff.
  const [, setRev] = useState<number>(props.document.revision());
  useEffect(() => {
    const doc = props.document;
    let last = doc.revision();
    setRev(last);
    const subId = doc.subscribe(() => {
      const r = doc.revision();
      if (r !== last) {
        last = r;
        setRev(r);
      }
    });
    return () => {
      doc.unsubscribe(subId);
    };
  }, [props.document]);

  const lineHeight = props.lineHeight > 0 ? props.lineHeight : 18;
  const overscan = props.overscan ?? DEFAULT_OVERSCAN;
  const scrollY = Math.max(0, props.scrollY ?? 0);
  const totalLines = props.document.lineCount();
  const firstVisible = Math.max(0, Math.floor(scrollY / lineHeight) - overscan);
  const lastVisible = Math.min(
    totalLines - 1,
    Math.ceil((scrollY + rect.height) / lineHeight) + overscan,
  );

  const fontSize = textStyle.fontSize ?? 14;
  // Monospace approximation: column → x within line (no per-glyph measure).
  const charW = fontSize * 0.6;
  const indexFromPoint = (x: number, y: number): { line: number; col: number } => {
    const localY = y - rect.y + scrollY;
    const line = Math.max(0, Math.min(totalLines - 1, Math.floor(localY / lineHeight)));
    const localX = x - rect.x;
    const lineRange = props.document.lineRange(line);
    const lineLen = lineRange.end - lineRange.start;
    const col = Math.max(0, Math.min(lineLen, Math.round(localX / Math.max(1, charW))));
    return { line, col };
  };

  registerHitTarget({
    id,
    rect,
    cursor: 'ibeam',
    a11y: {},
    componentType: 'LineViewport',
    onPressIn: (ev: SyntheticEvent) => {
      const native = ev.nativeEvent as { button?: number };
      if (native.button !== 0 && native.button !== undefined) return;
      if (props.onClickPosition) {
        const pos = indexFromPoint(ev.x, ev.y);
        props.onClickPosition(pos.line, pos.col, ev);
      }
    },
    onDrag: (ev: SyntheticEvent) => {
      if (!props.onDragPosition) return;
      const pos = indexFromPoint(ev.x, ev.y);
      props.onDragPosition(pos.line, pos.col, ev);
    },
    onPressOut: (ev: SyntheticEvent<MouseButtonEvent>) => {
      const native = ev.nativeEvent as { button?: number };
      if (native.button !== 0 && native.button !== undefined) return;
      if (!props.onPressUp) return;
      const pos = indexFromPoint(ev.x, ev.y);
      props.onPressUp(pos.line, pos.col, ev);
    },
  });
  const doc = props.document;
  const getLineDecorations = props.getLineDecorations;
  const tabSize = props.tabSize ?? 0;
  const showWhitespace = props.showWhitespace ?? false;
  const textColor: Color = props.textColor ?? 0xe6e6e6ff;

  const paint = (cb: CommandBuffer): void => {
    const baselineOffset = lineHeight * 0.18;
    for (let line = firstVisible; line <= lastVisible; ++line) {
      const lineY = rect.y + line * lineHeight - scrollY;
      if (lineY > rect.y + rect.height) break;
      if (lineY + lineHeight < rect.y) continue;

      const decorations = getLineDecorations ? getLineDecorations(line) : null;

      // Background highlight.
      if (decorations?.background !== undefined) {
        Primitives.drawSelectionRects(
          cb,
          new Float32Array([rect.x, lineY, rect.width, lineHeight]),
          decorations.background,
        );
      }

      // Selection rects (multi-cursor) — translate from line-local to viewport.
      const selRects = decorations?.selectionRects;
      if (selRects && selRects.length >= 4) {
        const translated = new Float32Array(selRects.length);
        for (let i = 0; i < selRects.length; i += 4) {
          translated[i] = rect.x + selRects[i];
          translated[i + 1] = lineY + selRects[i + 1];
          translated[i + 2] = selRects[i + 2];
          translated[i + 3] = selRects[i + 3];
        }
        Primitives.drawSelectionRects(cb, translated, DEFAULT_SELECTION_COLOR);
      }

      // Glyph run.
      const lineText = doc.lineText(line);
      const baselineY = lineY + baselineOffset;
      if (lineText.length > 0) {
        const spans = decorations?.spans;
        if (spans && spans.length > 0) {
          // Stitch span list against the line text. Any column not covered
          // by an explicit span paints with the default colour.
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
          for (const sp of spans) {
            const start = Math.max(cursor, sp.start);
            const end = Math.min(lineText.length, sp.end);
            if (end <= start) continue;
            if (start > cursor) {
              drawSpans.push({
                text: lineText.slice(cursor, start),
                color: textColor,
                size: fontSize,
              });
            }
            drawSpans.push({
              text: lineText.slice(start, end),
              color: sp.color ?? textColor,
              size: fontSize,
              bold: sp.bold,
              italic: sp.italic,
              underline: sp.underline,
              strikethrough: sp.strikethrough,
            });
            cursor = end;
          }
          if (cursor < lineText.length) {
            drawSpans.push({
              text: lineText.slice(cursor),
              color: textColor,
              size: fontSize,
            });
          }
          Primitives.drawTextSpans(cb, rect.x, baselineY, 0, drawSpans, {
            tabSize,
            tabOriginX: rect.x,
            showWhitespace,
            size: fontSize,
            color: textColor,
          });
        } else {
          Primitives.drawTextSpans(
            cb,
            rect.x,
            baselineY,
            0,
            [{ text: lineText, color: textColor, size: fontSize }],
            { tabSize, tabOriginX: rect.x, showWhitespace, size: fontSize, color: textColor },
          );
        }
      }

      // Diagnostic underlines.
      const diagnostics = decorations?.diagnostics;
      if (diagnostics) {
        const underlineY = baselineY + fontSize * 1.05;
        for (const d of diagnostics) {
          Primitives.drawDecorationUnderline(
            cb,
            rect.x + d.x1,
            rect.x + d.x2,
            underlineY,
            d.style,
            d.color,
            d.thickness,
          );
        }
      }
    }
  };

  return {
    type: 'draw',
    props: {
      deps: [
        doc,
        doc.revision(),
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        lineHeight,
        scrollY,
        firstVisible,
        lastVisible,
        textColor,
        tabSize,
        showWhitespace,
        getLineDecorations,
      ],
      fn: paint,
    },
  } as Node;
}, 'LineViewport');
