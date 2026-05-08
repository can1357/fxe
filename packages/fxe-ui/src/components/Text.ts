import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { registerHitTarget, type SyntheticEvent } from '../mount/hit_test.ts';
import { paintText } from '../paint/text_painter.ts';
import {
  type BoundaryChild,
  Component,
  type Node,
  useId,
  useRef,
  useState,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { glyphIndexAt, wrapText } from '../text/wrap.ts';
import { useTextStyle } from '../theme/text_context.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';

export type TextChild = string | number | readonly TextChild[] | BoundaryChild;
export interface TextProps extends InternalLayoutProps, AccessibilityProps {
  key?: string;
  style?: StyleValue;
  children?: TextChild;
  /** Plain Text is not selectable unless explicitly enabled. */
  selectable?: boolean;
}

function textFromChildren(child: TextChild | undefined): string {
  if (child === null || child === undefined || typeof child === 'boolean') return '';
  if (typeof child === 'string' || typeof child === 'number') return String(child);
  if (Array.isArray(child)) return child.map(textFromChildren as never).join('');
  return '';
}

function orderedRange(start: number, end: number): [number, number] {
  return start <= end ? [start, end] : [end, start];
}

function clampIndex(value: number, text: string): number {
  return Math.max(0, Math.min(Math.trunc(value), text.length));
}

function textA11yProps(props: TextProps, text: string): AccessibilityProps {
  const a11y = extractA11yProps(props);
  if (a11y.accessibilityLabel === undefined && text) {
    a11y.accessibilityLabel = text;
  }
  return a11y;
}
export const Text = Component((props: TextProps): Node => {
  const id = useId();
  const inherited = useTextStyle();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = { ...inherited, ...(props.__textStyle ?? {}), ...resolved.text };
  const text = textFromChildren(props.children);
  const [selectionStart, setSelectionStart] = useState(0);
  const [selectionEnd, setSelectionEnd] = useState(0);
  const dragAnchor = useRef(0);
  const rect = props.__layout
    ? { ...props.__layout }
    : rectFromStyle(resolved.layout, props.__layout);
  // If the parent didn't size us and the style didn't either, fall back to
  // the unwrapped intrinsic measurement so single-line uses still work.
  if (rect.width === 0 || rect.height === 0) {
    const intrinsic = wrapText(text, textStyle, { maxWidth: undefined });
    if (rect.width === 0) rect.width = intrinsic.width;
    if (rect.height === 0) rect.height = intrinsic.height;
  }
  const setSelection = (start: number, end: number): void => {
    setSelectionStart(clampIndex(start, text));
    setSelectionEnd(clampIndex(end, text));
  };
  const indexFromPoint = (x: number, y: number): number => {
    const wrapped = wrapText(text, textStyle, {
      maxWidth: rect.width > 0 ? rect.width : undefined,
    });
    const lineIndex = Math.max(
      0,
      Math.min(Math.floor((y - rect.y) / wrapped.lineHeight), wrapped.lines.length - 1),
    );
    const line = wrapped.lines[lineIndex] ?? '';
    const lineStart = wrapped.lineStartIndices[lineIndex] ?? 0;
    return clampIndex(lineStart + glyphIndexAt(line, textStyle, x - rect.x), text);
  };
  const a11y = textA11yProps(props, text);
  registerHitTarget({
    id,
    rect,
    cursor: props.selectable === true ? 'ibeam' : undefined,
    a11y,
    componentType: 'Text',
    tabIndex: a11y.tabIndex,
    ...(props.selectable === true
      ? {
          onPressIn: (ev: SyntheticEvent) => {
            if ((ev.nativeEvent as { button?: number }).button !== 0) return;
            const idx = indexFromPoint(ev.x, ev.y);
            dragAnchor.current = idx;
            setSelection(idx, idx);
          },
          onDrag: (ev: SyntheticEvent) =>
            setSelection(dragAnchor.current, indexFromPoint(ev.x, ev.y)),
          onPressOut: (ev: SyntheticEvent) => {
            if ((ev.nativeEvent as { button?: number }).button !== 0) return;
            setSelection(dragAnchor.current, indexFromPoint(ev.x, ev.y));
          },
          onKeyDown: (ev: unknown) => {
            const keyEvent = ev as {
              key?: number;
              modifiers?: number;
              setClipboardText?: (text: string) => void;
            };
            const accel = ((keyEvent.modifiers ?? 0) & (2 | 8)) !== 0;
            if (!accel || keyEvent.key !== 67) return;
            const [start, end] = orderedRange(selectionStart, selectionEnd);
            if (end > start) keyEvent.setClipboardText?.(text.slice(start, end));
          },
        }
      : {}),
  });
  const hasSelection = props.selectable === true && selectionStart !== selectionEnd;
  return {
    type: 'draw',
    props: {
      deps: [
        text,
        props.style,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        props.selectable,
        selectionStart,
        selectionEnd,
      ],
      fn: (cb) =>
        paintText(cb, rect, text, textStyle, {
          selection: hasSelection ? { start: selectionStart, end: selectionEnd } : undefined,
        }),
    },
  } as Node;
}, 'Text');
