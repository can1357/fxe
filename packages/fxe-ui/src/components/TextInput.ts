import type { CommandBuffer } from 'fxe';
import { registerHitTarget } from '../mount/hit_test.ts';
import { Component, type Node, useId, useRef, useState } from '../reconciler/fiber.ts';
import { paintText, type TextPreeditOptions } from '../paint/text_painter.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { useTheme } from '../theme/provider.ts';
import { useTextStyle } from '../theme/text_context.ts';
import { glyphIndexAt, wrapText } from '../text/wrap.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';
import { View } from './View.ts';

export interface TextInputProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue;
  value?: string;
  placeholder?: string;
  onChange?: (value: string) => void;
  onSubmit?: (value: string) => void;
  onCompose?: (preedit: string, cursor: number) => void;
  onCommit?: (committed: string) => void;
}

interface TextInputTextProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue;
  children: string;
  imePreedit?: TextPreeditOptions;
  selectionStart?: number;
  selectionEnd?: number;
  caretIndex?: number;
}

type ClipboardKeyEvent = {
  key?: number;
  modifiers?: number;
  clipboardText?: () => string;
  setClipboardText?: (text: string) => void;
};

const KEY_ENTER = 257;
const KEY_BACKSPACE = 259;
const KEY_DELETE = 261;
const KEY_RIGHT = 262;
const KEY_LEFT = 263;
const KEY_HOME = 268;
const KEY_END = 269;
const MOD_SHIFT = 1;
const MOD_CONTROL = 2;
const MOD_SUPER = 8;
const DOUBLE_CLICK_MS = 500;
const CLICK_SLOP = 4;

function orderedRange(start: number, end: number): [number, number] {
  return start <= end ? [start, end] : [end, start];
}

function clampIndex(value: number, text: string): number {
  return Math.max(0, Math.min(Math.trunc(value), text.length));
}

function wordRangeAt(text: string, idx: number): [number, number] {
  const clamped = clampIndex(idx, text);
  const tokens = text.matchAll(/\w+|\s+|./g);
  for (const token of tokens) {
    const start = token.index ?? 0;
    const end = start + token[0].length;
    if (clamped >= start && clamped < end) return [start, end];
    if (clamped === text.length && end === text.length) return [start, end];
  }
  return [clamped, clamped];
}

const TextInputText = Component((props: TextInputTextProps): Node => {
  const inherited = useTextStyle();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = { ...inherited, ...(props.__textStyle ?? {}), ...resolved.text };
  const text = props.children;
  const rect = props.__layout
    ? { ...props.__layout }
    : rectFromStyle(resolved.layout, props.__layout);
  if (rect.width === 0 || rect.height === 0) {
    const intrinsic = wrapText(text, textStyle, { maxWidth: undefined });
    if (rect.width === 0) rect.width = intrinsic.width;
    if (rect.height === 0) rect.height = intrinsic.height;
  }
  const hasSelection =
    props.selectionStart !== undefined &&
    props.selectionEnd !== undefined &&
    props.selectionStart !== props.selectionEnd;
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
        props.imePreedit?.text,
        props.imePreedit?.cursor,
        props.imePreedit?.underlineColor,
        props.selectionStart,
        props.selectionEnd,
        props.caretIndex,
      ],
      fn: (cb: CommandBuffer) =>
        paintText(cb, rect, text, textStyle, {
          imePreedit: props.imePreedit,
          selection: hasSelection
            ? { start: props.selectionStart ?? 0, end: props.selectionEnd ?? 0 }
            : undefined,
          caretIndex: props.caretIndex,
        }),
    },
  } as Node;
}, 'Text');

export const TextInput = Component((props: TextInputProps): Node => {
  const id = useId();
  const theme = useTheme();
  const inherited = useTextStyle();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = { ...inherited, ...resolved.text };
  const initial = props.value ?? '';
  const [value, setValue] = useState(initial);
  const [selectionStart, setSelectionStart] = useState(initial.length);
  const [selectionEnd, setSelectionEnd] = useState(initial.length);
  const [focused, setFocused] = useState(false);
  const dragAnchor = useRef(initial.length);
  const lastClick = useRef({ time: 0, x: 0, y: 0, count: 0 });
  const [imePreedit, setImePreedit] = useState<{ preedit: string; cursor: number }>({
    preedit: '',
    cursor: 0,
  });
  const rect = rectFromStyle(resolved.layout, props.__layout);
  const disabled = (props as { disabled?: boolean }).disabled === true;

  const setSelection = (start: number, end: number): void => {
    setSelectionStart(clampIndex(start, value));
    setSelectionEnd(clampIndex(end, value));
  };
  const commit = (next: string, nextStart = next.length, nextEnd = nextStart): void => {
    setValue(next);
    setSelectionStart(clampIndex(nextStart, next));
    setSelectionEnd(clampIndex(nextEnd, next));
    props.onChange?.(next);
  };
  const replaceSelection = (insert: string): void => {
    const [start, end] = orderedRange(selectionStart, selectionEnd);
    const next = value.slice(0, start) + insert + value.slice(end);
    const caret = start + insert.length;
    commit(next, caret, caret);
  };
  const selectedText = (): string => {
    const [start, end] = orderedRange(selectionStart, selectionEnd);
    return value.slice(start, end);
  };
  const indexFromPoint = (x: number): number => {
    const textX = x - rect.x - (rect.paddingLeft ?? 0);
    return glyphIndexAt(value, textStyle, textX);
  };
  const handlePointerSelection = (x: number): void => {
    const idx = indexFromPoint(x);
    setSelection(dragAnchor.current, idx);
  };

  registerHitTarget({
    id,
    rect,
    cursor: 'ibeam',
    onFocus: () => setFocused(true),
    onBlur: () => {
      setFocused(false);
      setImePreedit({ preedit: '', cursor: 0 });
    },
    onPressIn: (ev) => {
      if (disabled || (ev.nativeEvent as { button?: number }).button !== 0) return;
      const idx = indexFromPoint(ev.x);
      const now = Date.now();
      const dx = ev.x - lastClick.current.x;
      const dy = ev.y - lastClick.current.y;
      const sameSpot = dx * dx + dy * dy <= CLICK_SLOP * CLICK_SLOP;
      const count =
        now - lastClick.current.time <= DOUBLE_CLICK_MS && sameSpot
          ? lastClick.current.count + 1
          : 1;
      lastClick.current = { time: now, x: ev.x, y: ev.y, count };
      if (count >= 3) {
        dragAnchor.current = 0;
        setSelection(0, value.length);
      } else if (count === 2) {
        const [start, end] = wordRangeAt(value, idx);
        dragAnchor.current = start;
        setSelection(start, end);
      } else {
        dragAnchor.current = idx;
        setSelection(idx, idx);
      }
    },
    onDrag: (ev) => {
      if (disabled) return;
      handlePointerSelection(ev.x);
    },
    onPressOut: (ev) => {
      if (disabled || (ev.nativeEvent as { button?: number }).button !== 0) return;
      if (lastClick.current.count === 1) handlePointerSelection(ev.x);
    },
    onKeyPress: (ev) => {
      if (disabled) return;
      const codepoint = (ev as { codepoint?: number }).codepoint ?? 0;
      if (codepoint >= 32) replaceSelection(String.fromCodePoint(codepoint));
    },
    // Compose is routed through the focused HitTarget rather than component-level
    // useEvent because TextInput does not own the Window; mount/event_pipeline
    // forwards the window compose event to this focused handler.
    onCompose: (ev) => {
      if (disabled) return;
      const committed = ev.committed ?? '';
      const preedit = ev.preedit ?? '';
      const cursor = ev.cursor ?? 0;
      if (committed.length === 0 && preedit.length === 0) return;
      if (committed.length > 0) {
        replaceSelection(committed);
        props.onCommit?.(committed);
      }
      if (preedit.length > 0) {
        setImePreedit({ preedit, cursor });
        props.onCompose?.(preedit, cursor);
      } else if (committed.length > 0) {
        setImePreedit({ preedit: '', cursor: 0 });
      }
    },
    onKeyDown: (ev) => {
      if (disabled) return;
      const keyEvent = ev as ClipboardKeyEvent;
      const key = keyEvent.key;
      const modifiers = keyEvent.modifiers ?? 0;
      const shift = (modifiers & MOD_SHIFT) !== 0;
      const accel = (modifiers & (MOD_CONTROL | MOD_SUPER)) !== 0;

      if (accel && (key === 65 || key === 97)) {
        setSelection(0, value.length);
        return;
      }
      if (accel && (key === 67 || key === 99)) {
        const text = selectedText();
        if (text.length > 0) keyEvent.setClipboardText?.(text);
        return;
      }
      if (accel && (key === 88 || key === 120)) {
        const text = selectedText();
        if (text.length > 0) {
          keyEvent.setClipboardText?.(text);
          replaceSelection('');
        }
        return;
      }
      if (accel && (key === 86 || key === 118)) {
        const text = keyEvent.clipboardText?.();
        if (text !== undefined) replaceSelection(text);
        return;
      }

      if (key === KEY_LEFT || key === KEY_RIGHT || key === KEY_HOME || key === KEY_END) {
        const [start, end] = orderedRange(selectionStart, selectionEnd);
        let next = selectionEnd;
        if (key === KEY_HOME) next = 0;
        else if (key === KEY_END) next = value.length;
        else if (!shift && selectionStart !== selectionEnd) next = key === KEY_LEFT ? start : end;
        else next = clampIndex(selectionEnd + (key === KEY_LEFT ? -1 : 1), value);
        if (shift) setSelection(selectionStart, next);
        else setSelection(next, next);
        return;
      }

      if (key === KEY_BACKSPACE) {
        if (selectionStart !== selectionEnd) replaceSelection('');
        else if (selectionEnd > 0) {
          const caret = selectionEnd - 1;
          commit(value.slice(0, caret) + value.slice(selectionEnd), caret, caret);
        }
        return;
      }
      if (key === KEY_DELETE) {
        if (selectionStart !== selectionEnd) replaceSelection('');
        else if (selectionEnd < value.length)
          commit(
            value.slice(0, selectionEnd) + value.slice(selectionEnd + 1),
            selectionEnd,
            selectionEnd,
          );
        return;
      }
      if (key === KEY_ENTER) props.onSubmit?.(value);
    },
  });
  const isShowingPlaceholder = value.length === 0 && imePreedit.preedit.length === 0;
  const shown = isShowingPlaceholder ? (props.placeholder ?? '') : value;
  return View({
    ...props,
    style: [
      {
        minHeight: 34,
        paddingX: theme.spacing.sm,
        paddingY: theme.spacing.xs,
        // Vertically centre the inner text rather than top-aligning it.
        // Without this a single-line input with extra height (e.g. 38–44px)
        // paints the glyphs flush to the top of the box, which looks awkward
        // next to placeholder rules and adjacent labels.
        justifyContent: 'center',
        borderWidth: 1,
        borderColor: focused ? theme.colors.primary : theme.colors.border,
        backgroundColor: theme.colors.surface,
      },
      props.style,
    ],
    children: TextInputText({
      style: { color: isShowingPlaceholder ? theme.colors.mutedText : theme.colors.text },
      children: shown,
      selectionStart: isShowingPlaceholder ? undefined : selectionStart,
      selectionEnd: isShowingPlaceholder ? undefined : selectionEnd,
      caretIndex: focused && selectionStart === selectionEnd ? selectionEnd : undefined,
      imePreedit:
        imePreedit.preedit.length > 0
          ? {
              text: imePreedit.preedit,
              cursor: imePreedit.cursor,
              underlineColor: theme.colors.primary,
            }
          : undefined,
    }),
  });
}, 'TextInput');
