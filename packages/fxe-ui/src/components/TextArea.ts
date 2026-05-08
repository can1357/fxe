// Multi-line text input. Reuses the shared edit model + wrap helpers from
// TextInput; differs in:
//   - Enter inserts a newline (Cmd/Ctrl+Enter calls onSubmit instead).
//   - Tab defaults to inserting a `\t` rather than advancing focus.
//   - Up/Down arrow keys move the caret across visual (wrapped) lines while
//     trying to preserve the current x position.
//   - Home/End snap to source-line edges via `moveToLineEdge`.
//   - Default minHeight scales with `numberOfLines` (default 4).
import type { CommandBuffer } from 'fxe';
import { registerHitTarget } from '../mount/hit_test.ts';
import { paintText, type TextPreeditOptions } from '../paint/text_painter.ts';
import {
  Component,
  type Node,
  useFrame,
  useId,
  useMemo,
  useRef,
  useState,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { popupEditMenu } from '../text/edit_menu.ts';
import {
  clampIndex,
  createHistory,
  detectPlatform,
  type EditKind,
  isLineJumpModifier,
  isPrimaryModifier,
  isWordJumpModifier,
  MOD_SHIFT,
  moveByWord,
  moveToLineEdge,
  orderedRange,
  pushTransaction,
  redo as redoHistory,
  type TextEditState,
  type TextEditTransaction,
  undo as undoHistory,
  wordRangeAt,
} from '../text/edit_model.ts';
import { pointToTextIndex, textIndexToPoint, wrapText } from '../text/wrap.ts';
import { useTheme } from '../theme/provider.ts';
import { useTextStyle } from '../theme/text_context.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';
import { extractA11yProps } from '../a11y/extract.ts';
import { View } from './View.ts';

export interface TextAreaProps extends InternalLayoutProps, AccessibilityProps {
  key?: string;
  style?: StyleValue;
  value?: string;
  placeholder?: string;
  onChange?: (value: string) => void;
  onSubmit?: (value: string) => void;
  onCompose?: (preedit: string, cursor: number) => void;
  onCommit?: (committed: string) => void;
  caretBlinkMs?: number;
  secureTextEntry?: boolean;
  maxLength?: number;
  selectAllOnFocus?: boolean;
  onPaste?: (text: string) => string | null;
  onSelectionChange?: (selection: { start: number; end: number }) => void;
  readOnly?: boolean;
  disabled?: boolean;
  /** Tab key behavior: 'insert' (default for TextArea) inserts \t; 'focus' advances focus. */
  tabBehavior?: 'focus' | 'insert';
  selectionColor?: number;
  focusRing?: boolean;
  inputMode?: 'text' | 'numeric' | 'decimal' | 'email' | 'tel' | 'url' | 'search' | 'none';
  spellCheck?: boolean;
  autoCapitalize?: 'none' | 'sentences' | 'words' | 'characters';
  autoCorrect?: boolean;
  /** Suggested visible row count for sizing. Default 4. */
  numberOfLines?: number;
  /** Soft-wrap content at the box width. Default true. */
  softWrap?: boolean;
}

interface TextAreaInnerProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue;
  children: string;
  imePreedit?: TextPreeditOptions;
  selectionStart?: number;
  selectionEnd?: number;
  caretIndex?: number;
  caretOpacity?: number;
  selectionColor?: number;
  softWrap?: boolean;
  scrollY?: number;
}

type ClipboardKeyEvent = {
  key?: number;
  modifiers?: number;
  clipboardText?: () => string;
  setClipboardText?: (text: string) => void;
};

const KEY_ENTER = 257;
const KEY_TAB = 258;
const KEY_BACKSPACE = 259;
const KEY_DELETE = 261;
const KEY_RIGHT = 262;
const KEY_LEFT = 263;
const KEY_DOWN = 264;
const KEY_UP = 265;
const KEY_HOME = 268;
const KEY_END = 269;
const KEY_A = 65;
const KEY_C = 67;
const KEY_V = 86;
const KEY_X = 88;
const KEY_Y = 89;
const KEY_Z = 90;
// Letter keys arrive as uppercase GLFW codes from key events, but lowercase
// ASCII char codes from typed-character events. Match both.
const isLetter = (key: number, upper: number) => key === upper || key === upper + 32;
const DOUBLE_CLICK_MS = 500;
const CLICK_SLOP = 4;

const TextAreaInner = Component((props: TextAreaInnerProps): Node => {
  const inherited = useTextStyle();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = { ...inherited, ...(props.__textStyle ?? {}), ...resolved.text };
  const text = props.children;
  const rect = props.__layout
    ? { ...props.__layout }
    : rectFromStyle(resolved.layout, props.__layout);
  const wrapMax = props.softWrap === false ? undefined : rect.width > 0 ? rect.width : undefined;
  if (rect.width === 0 || rect.height === 0) {
    const intrinsic = wrapText(text, textStyle, { maxWidth: wrapMax });
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
        props.caretOpacity,
        props.selectionColor,
        props.softWrap,
        props.scrollY,
      ],
      fn: (cb: CommandBuffer) =>
        paintText(cb, rect, text, textStyle, {
          imePreedit: props.imePreedit,
          selection: hasSelection
            ? {
                start: props.selectionStart ?? 0,
                end: props.selectionEnd ?? 0,
                color: props.selectionColor,
              }
            : undefined,
          caretIndex: props.caretIndex,
          caretOpacity: props.caretOpacity,
          scrollOffset: props.scrollY ? { y: props.scrollY } : undefined,
        }),
    },
  } as Node;
}, 'Text');

export const TextArea = Component((props: TextAreaProps): Node => {
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
  const [caretPhase, setCaretPhase] = useState(true);
  const [scrollY, setScrollY] = useState(0);
  const [imePreedit, setImePreedit] = useState<{ preedit: string; cursor: number }>({
    preedit: '',
    cursor: 0,
  });

  const dragAnchor = useRef(initial.length);
  const lastClick = useRef({ time: 0, x: 0, y: 0, count: 0 });
  const blinkAccum = useRef({ ms: 0 });
  const firstFocus = useRef(true);
  const historyRef = useRef(createHistory());
  // Drag-edge auto-scroll on the Y axis (vertical scroll for multi-line).
  const dragEdge = useRef<{ dir: -1 | 1; lastX: number; lastY: number } | null>(null);
  const pendingDragOut = useRef<{ x: number; y: number; text: string } | null>(null);
  const platform = useMemo(() => detectPlatform(), []);

  const rect = rectFromStyle(resolved.layout, props.__layout);
  const disabled = props.disabled === true;
  const readOnly = props.readOnly === true;
  const secure = props.secureTextEntry === true;
  const maxLen = props.maxLength;
  const blinkMs = props.caretBlinkMs ?? 530;
  const tabBehavior = props.tabBehavior ?? 'insert';
  const softWrap = props.softWrap !== false;
  const numberOfLines = Math.max(1, Math.trunc(props.numberOfLines ?? 4));
  void props.inputMode;
  void props.spellCheck;
  void props.autoCapitalize;
  void props.autoCorrect;
  const a11y = extractA11yProps(props);
  const a11yState = { ...(a11y.accessibilityState ?? {}) };
  if (a11yState.disabled === undefined && disabled) a11yState.disabled = true;
  if (a11yState.readOnly === undefined && readOnly) a11yState.readOnly = true;
  const a11yValue = a11y.accessibilityValue ?? { text: value };

  const visibleHeight = (): number => {
    const padT = rect.paddingTop ?? 0;
    const padB = rect.paddingBottom ?? 0;
    return Math.max(0, rect.height - padT - padB);
  };

  // Adjust scrollY so the caret at glyph index `idx` is inside the visible
  // viewport. Uses unscrolled (text-local) coordinates.
  const ensureCaretInView = (idx: number): void => {
    const h = visibleHeight();
    if (h <= 0) return;
    const wrapped = wrapText(value, textStyle, { maxWidth: wrapMaxForHit() });
    if (wrapped.lines.length === 0) {
      if (scrollY !== 0) setScrollY(0);
      return;
    }
    const point = textIndexToPoint(wrapped, textStyle, clampIndex(idx, value));
    let next = scrollY;
    if (point.y < next) next = Math.max(0, point.y);
    else if (point.y + wrapped.lineHeight > next + h)
      next = Math.max(0, point.y + wrapped.lineHeight - h);
    if (next !== scrollY) setScrollY(next);
  };

  const setSelection = (anchor: number, focus: number, fire = true): void => {
    const a = clampIndex(anchor, value);
    const f = clampIndex(focus, value);
    setSelectionStart(a);
    setSelectionEnd(f);
    blinkAccum.current.ms = 0;
    setCaretPhase(true);
    ensureCaretInView(f);
    if (fire) props.onSelectionChange?.({ start: a, end: f });
  };

  const commit = (next: string, anchor: number, focus: number, kind: EditKind): void => {
    const before: TextEditState = {
      value,
      selection: { anchor: selectionStart, focus: selectionEnd },
    };
    const after: TextEditState = { value: next, selection: { anchor, focus } };
    const tx: TextEditTransaction = {
      before,
      after,
      kind,
      timestamp: Date.now(),
    };
    historyRef.current = pushTransaction(historyRef.current, tx);
    setValue(next);
    setSelection(anchor, focus, false);
    props.onChange?.(next);
    props.onSelectionChange?.({ start: anchor, end: focus });
  };

  const replaceSelection = (insert: string, kind: EditKind = 'type'): void => {
    if (readOnly || disabled) return;
    const [start, end] = orderedRange(selectionStart, selectionEnd);
    let ins = insert;
    if (maxLen !== undefined) {
      const room = Math.max(0, maxLen - (value.length - (end - start)));
      if (room <= 0 && end === start) return;
      ins = ins.slice(0, room);
    }
    const next = value.slice(0, start) + ins + value.slice(end);
    const caret = start + ins.length;
    commit(next, caret, caret, kind);
  };

  const selectedText = (): string => {
    const [s, e] = orderedRange(selectionStart, selectionEnd);
    return value.slice(s, e);
  };

  const wrapMaxForHit = (): number | undefined => {
    if (!softWrap) return undefined;
    return rect.width > 0 ? rect.width : undefined;
  };

  const indexFromPoint = (x: number, y: number): number => {
    const padX = rect.paddingLeft ?? 0;
    const padY = rect.paddingTop ?? 0;
    const wrapped = wrapText(value, textStyle, { maxWidth: wrapMaxForHit() });
    return pointToTextIndex(wrapped, textStyle, x - rect.x - padX, y - rect.y - padY + scrollY);
  };

  const moveCaretVertical = (idx: number, dir: 1 | -1): number => {
    const wrapped = wrapText(value, textStyle, { maxWidth: wrapMaxForHit() });
    if (wrapped.lines.length === 0) return idx;
    const cur = textIndexToPoint(wrapped, textStyle, idx);
    const targetY = cur.y + dir * wrapped.lineHeight;
    return pointToTextIndex(wrapped, textStyle, cur.x, targetY);
  };

  const doUndo = (): void => {
    if (readOnly || disabled) return;
    const r = undoHistory(
      { value, selection: { anchor: selectionStart, focus: selectionEnd } },
      historyRef.current,
    );
    if (!r) return;
    historyRef.current = r.history;
    setValue(r.state.value);
    setSelection(r.state.selection.anchor, r.state.selection.focus, false);
    props.onChange?.(r.state.value);
    props.onSelectionChange?.({
      start: r.state.selection.anchor,
      end: r.state.selection.focus,
    });
  };

  const doRedo = (): void => {
    if (readOnly || disabled) return;
    const r = redoHistory(
      { value, selection: { anchor: selectionStart, focus: selectionEnd } },
      historyRef.current,
    );
    if (!r) return;
    historyRef.current = r.history;
    setValue(r.state.value);
    setSelection(r.state.selection.anchor, r.state.selection.focus, false);
    props.onChange?.(r.state.value);
    props.onSelectionChange?.({
      start: r.state.selection.anchor,
      end: r.state.selection.focus,
    });
  };

  const clipboardSink = (): { read?: () => string; write?: (t: string) => void } | null => {
    return (
      (
        globalThis as {
          __fxe_clipboard?: { read?: () => string; write?: (t: string) => void };
        }
      ).__fxe_clipboard ?? null
    );
  };

  useFrame((dt) => {
    if (focused && !disabled && blinkMs > 0) {
      if (selectionStart === selectionEnd && imePreedit.preedit.length === 0) {
        blinkAccum.current.ms += dt;
        if (blinkAccum.current.ms >= blinkMs) {
          blinkAccum.current.ms = 0;
          setCaretPhase((p) => !p);
        }
      }
    }
    if (dragEdge.current && !disabled) {
      const SPEED_PX_PER_SEC = 600;
      const delta = (SPEED_PX_PER_SEC * dt) / 1000;
      const next = Math.max(0, scrollY + dragEdge.current.dir * delta);
      const wrapped = wrapText(value, textStyle, { maxWidth: wrapMaxForHit() });
      const contentH = wrapped.lines.length * wrapped.lineHeight;
      const maxScroll = Math.max(0, contentH - visibleHeight() + 8);
      const clamped = Math.min(next, maxScroll);
      if (clamped !== scrollY) setScrollY(clamped);
      const idx = indexFromPoint(dragEdge.current.lastX, dragEdge.current.lastY);
      if (idx !== selectionEnd) {
        const a = clampIndex(dragAnchor.current, value);
        const f = clampIndex(idx, value);
        setSelectionStart(a);
        setSelectionEnd(f);
        blinkAccum.current.ms = 0;
        setCaretPhase(true);
        props.onSelectionChange?.({ start: a, end: f });
      }
    }
  });

  registerHitTarget({
    id,
    rect,
    a11y: {
      ...a11y,
      accessibilityRole: a11y.accessibilityRole ?? 'textbox',
      accessibilityState: a11yState,
      accessibilityValue: a11yValue,
    },
    componentType: 'TextArea',
    tabIndex: props.tabIndex,
    cursor: disabled ? 'not_allowed' : 'ibeam',
    onFocus: () => {
      if (disabled) return;
      setFocused(true);
      blinkAccum.current.ms = 0;
      setCaretPhase(true);
      if (firstFocus.current && props.selectAllOnFocus === true && value.length > 0) {
        setSelection(0, value.length);
      }
      firstFocus.current = false;
    },
    onBlur: () => {
      setFocused(false);
      setImePreedit({ preedit: '', cursor: 0 });
    },
    onPressIn: (ev) => {
      if (disabled) return;
      if ((ev.nativeEvent as { button?: number }).button !== 0) return;
      const idx = indexFromPoint(ev.x, ev.y);
      const now = Date.now();
      const dx = ev.x - lastClick.current.x;
      const dy = ev.y - lastClick.current.y;
      const sameSpot = dx * dx + dy * dy <= CLICK_SLOP * CLICK_SLOP;
      const count =
        now - lastClick.current.time <= DOUBLE_CLICK_MS && sameSpot
          ? lastClick.current.count + 1
          : 1;
      lastClick.current = { time: now, x: ev.x, y: ev.y, count };
      pendingDragOut.current = null;
      dragEdge.current = null;
      if (count >= 3) {
        dragAnchor.current = 0;
        setSelection(0, value.length);
      } else if (count === 2) {
        const [start, end] = wordRangeAt(value, idx);
        dragAnchor.current = start;
        setSelection(start, end);
      } else {
        const [s, e] = orderedRange(selectionStart, selectionEnd);
        const insideSelection = !secure && s !== e && idx >= s && idx <= e;
        if (insideSelection) {
          pendingDragOut.current = { x: ev.x, y: ev.y, text: selectedText() };
          dragAnchor.current = idx;
        } else {
          dragAnchor.current = idx;
          setSelection(idx, idx);
        }
      }
    },
    onDrag: (ev) => {
      if (disabled) return;
      const pend = pendingDragOut.current;
      if (pend) {
        const dx = ev.x - pend.x;
        const dy = ev.y - pend.y;
        if (dx * dx + dy * dy > CLICK_SLOP * CLICK_SLOP) {
          const drag = (ev as { requestDragOut?: (payload: { text: string }) => boolean })
            .requestDragOut;
          if (drag) drag({ text: pend.text });
          pendingDragOut.current = null;
          dragEdge.current = null;
          return;
        }
        return;
      }
      const padT = rect.paddingTop ?? 0;
      const padB = rect.paddingBottom ?? 0;
      const topEdge = rect.y + padT;
      const botEdge = rect.y + rect.height - padB;
      if (ev.y < topEdge) {
        dragEdge.current = { dir: -1, lastX: ev.x, lastY: ev.y };
      } else if (ev.y > botEdge) {
        dragEdge.current = { dir: 1, lastX: ev.x, lastY: ev.y };
      } else {
        dragEdge.current = null;
      }
      setSelection(dragAnchor.current, indexFromPoint(ev.x, ev.y));
    },
    onPressOut: (ev) => {
      dragEdge.current = null;
      if (disabled) return;
      if ((ev.nativeEvent as { button?: number }).button !== 0) return;
      if (pendingDragOut.current) {
        const idx = indexFromPoint(ev.x, ev.y);
        pendingDragOut.current = null;
        setSelection(idx, idx);
        return;
      }
      if (lastClick.current.count === 1) {
        setSelection(dragAnchor.current, indexFromPoint(ev.x, ev.y));
      }
    },
    onContextMenu: (ev) => {
      if (disabled) return;
      const hasSel = selectionStart !== selectionEnd;
      const hist = historyRef.current;
      void popupEditMenu(ev.x, ev.y, {
        hasSelection: hasSel,
        canUndo: hist.past.length > 0,
        canRedo: hist.future.length > 0,
        canPaste: !readOnly,
        readOnly,
        disabled,
      }).then((action) => {
        if (action === null) return;
        if (action === 'undo') {
          doUndo();
          return;
        }
        if (action === 'redo') {
          doRedo();
          return;
        }
        if (action === 'selectAll') {
          setSelection(0, value.length);
          return;
        }
        const sink = clipboardSink();
        if (action === 'copy') {
          if (secure) return;
          const t = selectedText();
          if (t.length > 0) sink?.write?.(t);
          return;
        }
        if (action === 'cut') {
          if (secure || readOnly) return;
          const t = selectedText();
          if (t.length > 0) {
            sink?.write?.(t);
            replaceSelection('', 'cut');
          }
          return;
        }
        if (action === 'paste') {
          if (readOnly) return;
          const t = sink?.read?.() ?? '';
          if (props.onPaste) {
            const r = props.onPaste(t);
            if (r === null) return;
            if (r.length > 0) replaceSelection(r, 'paste');
            return;
          }
          if (t.length > 0) replaceSelection(t, 'paste');
          return;
        }
      });
    },
    onEditCommand: (action) => {
      if (disabled) return;
      if (action === 'undo') return doUndo();
      if (action === 'redo') return doRedo();
      if (action === 'selectAll') {
        setSelection(0, value.length);
        return;
      }
      const sink = clipboardSink();
      if (action === 'copy') {
        if (secure) return;
        const t = selectedText();
        if (t.length > 0) sink?.write?.(t);
        return;
      }
      if (action === 'cut') {
        if (secure || readOnly) return;
        const t = selectedText();
        if (t.length > 0) {
          sink?.write?.(t);
          replaceSelection('', 'cut');
        }
        return;
      }
      if (action === 'paste') {
        if (readOnly) return;
        const t = sink?.read?.() ?? '';
        if (props.onPaste) {
          const r = props.onPaste(t);
          if (r === null) return;
          if (r.length > 0) replaceSelection(r, 'paste');
          return;
        }
        if (t.length > 0) replaceSelection(t, 'paste');
      }
    },
    onKeyPress: (ev) => {
      if (disabled || readOnly) return;
      const codepoint = (ev as { codepoint?: number }).codepoint ?? 0;
      if (codepoint >= 32) replaceSelection(String.fromCodePoint(codepoint), 'type');
    },
    onCompose: (ev) => {
      if (disabled) return;
      const committed = ev.committed ?? '';
      const preedit = ev.preedit ?? '';
      const cursor = ev.cursor ?? 0;
      if (committed.length === 0 && preedit.length === 0) return;
      if (committed.length > 0) {
        replaceSelection(committed, 'ime');
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
      const mods = keyEvent.modifiers ?? 0;
      const shift = (mods & MOD_SHIFT) !== 0;
      const primary = isPrimaryModifier(mods, platform);
      const wordJump = isWordJumpModifier(mods, platform);
      const lineJump = isLineJumpModifier(mods, platform);

      if (primary && isLetter(key, KEY_A)) {
        setSelection(0, value.length);
        return;
      }
      if (primary && isLetter(key, KEY_C)) {
        if (!secure) {
          const t = selectedText();
          if (t.length > 0) keyEvent.setClipboardText?.(t);
        }
        return;
      }
      if (primary && isLetter(key, KEY_X)) {
        if (readOnly || secure) return;
        const t = selectedText();
        if (t.length > 0) {
          keyEvent.setClipboardText?.(t);
          replaceSelection('', 'cut');
        }
        return;
      }
      if (primary && isLetter(key, KEY_V)) {
        if (readOnly) return;
        let t = keyEvent.clipboardText?.() ?? '';
        if (props.onPaste) {
          const r = props.onPaste(t);
          if (r === null) return;
          t = r;
        }
        if (t.length > 0) replaceSelection(t, 'paste');
        return;
      }
      if (primary && isLetter(key, KEY_Z)) {
        if (shift) doRedo();
        else doUndo();
        return;
      }
      if (primary && isLetter(key, KEY_Y)) {
        doRedo();
        return;
      }

      if (key === KEY_LEFT || key === KEY_RIGHT) {
        const dir: 1 | -1 = key === KEY_LEFT ? -1 : 1;
        let next = selectionEnd;
        if (wordJump) {
          next = moveByWord(value, selectionEnd, dir);
        } else if (lineJump) {
          next = moveToLineEdge(value, selectionEnd, dir < 0 ? 'start' : 'end');
        } else if (!shift && selectionStart !== selectionEnd) {
          const [s, e] = orderedRange(selectionStart, selectionEnd);
          next = dir < 0 ? s : e;
        } else {
          next = clampIndex(selectionEnd + dir, value);
        }
        if (shift) setSelection(selectionStart, next);
        else setSelection(next, next);
        return;
      }

      if (key === KEY_UP || key === KEY_DOWN) {
        const dir: 1 | -1 = key === KEY_UP ? -1 : 1;
        const next = moveCaretVertical(selectionEnd, dir);
        if (shift) setSelection(selectionStart, next);
        else setSelection(next, next);
        return;
      }

      if (key === KEY_HOME || key === KEY_END) {
        const next =
          key === KEY_HOME
            ? moveToLineEdge(value, selectionEnd, 'start')
            : moveToLineEdge(value, selectionEnd, 'end');
        if (shift) setSelection(selectionStart, next);
        else setSelection(next, next);
        return;
      }

      if (key === KEY_BACKSPACE) {
        if (readOnly) return;
        if (selectionStart !== selectionEnd) {
          replaceSelection('', 'delete');
        } else if (selectionEnd > 0) {
          const start = wordJump ? moveByWord(value, selectionEnd, -1) : selectionEnd - 1;
          commit(value.slice(0, start) + value.slice(selectionEnd), start, start, 'delete');
        }
        return;
      }
      if (key === KEY_DELETE) {
        if (readOnly) return;
        if (selectionStart !== selectionEnd) {
          replaceSelection('', 'delete');
        } else if (selectionEnd < value.length) {
          const end = wordJump ? moveByWord(value, selectionEnd, 1) : selectionEnd + 1;
          commit(
            value.slice(0, selectionEnd) + value.slice(end),
            selectionEnd,
            selectionEnd,
            'delete',
          );
        }
        return;
      }

      if (key === KEY_ENTER) {
        // Cmd/Ctrl+Enter submits; plain Enter inserts newline.
        if (primary) {
          if (!readOnly) props.onSubmit?.(value);
          return;
        }
        if (!readOnly) replaceSelection('\n', 'type');
        return;
      }

      if (key === KEY_TAB && tabBehavior === 'insert' && !readOnly) {
        replaceSelection('\t', 'type');
        return;
      }
    },
  });

  const showPlaceholder = value.length === 0 && imePreedit.preedit.length === 0;
  const masked = secure ? '\u2022'.repeat(value.length) : value;
  const shown = showPlaceholder ? (props.placeholder ?? '') : masked;
  const hasSelectionRange = !showPlaceholder && selectionStart !== selectionEnd;
  const showCaret = focused && !disabled && !showPlaceholder && imePreedit.preedit.length === 0;
  const caretOpacity = showCaret && !hasSelectionRange ? (caretPhase ? 1 : 0) : 0;
  const focusRingOn = focused && !disabled && props.focusRing !== false;
  const lineHeight = (textStyle.fontSize ?? 16) * 1.4;
  const minHeight = Math.max(34, Math.round(numberOfLines * lineHeight + theme.spacing.xs * 2));

  return View({
    ...props,
    style: [
      {
        minHeight,
        paddingX: theme.spacing.sm,
        paddingY: theme.spacing.xs,
        borderWidth: focusRingOn ? 2 : 1,
        borderColor: focused ? theme.colors.primary : theme.colors.border,
        backgroundColor: theme.colors.surface,
        opacity: disabled ? 0.5 : 1,
        overflow: 'hidden',
      },
      props.style,
    ],
    children: TextAreaInner({
      style: { color: showPlaceholder ? theme.colors.mutedText : theme.colors.text },
      children: shown,
      selectionStart: showPlaceholder ? undefined : selectionStart,
      selectionEnd: showPlaceholder ? undefined : selectionEnd,
      caretIndex: showCaret ? selectionEnd : undefined,
      caretOpacity,
      selectionColor: props.selectionColor,
      softWrap,
      scrollY: showPlaceholder ? 0 : scrollY,
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
}, 'TextArea');
