import type { CommandBuffer } from 'fxe';
import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { registerHitTarget } from '../mount/hit_test.ts';
import { paintText, type TextPreeditOptions } from '../paint/text_painter.ts';
import {
  Component,
  type Node,
  useFrame,
  useId,
  useInternalLayout,
  useInternalTextStyle,
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
import { pointToTextIndex, wrapText, xAtGlyphIndex } from '../text/wrap.ts';
import { useTheme } from '../theme/provider.ts';
import { useTextStyle } from '../theme/text_context.ts';
import { rectFromStyle } from './common.ts';
import { View } from './View.ts';

export interface TextInputProps extends AccessibilityProps {
  key?: string;
  style?: StyleValue;
  value?: string;
  placeholder?: string;
  onChange?: (value: string) => void;
  onSubmit?: (value: string) => void;
  onCompose?: (preedit: string, cursor: number) => void;
  onCommit?: (committed: string) => void;
  /** Cursor caret blink interval in ms; 0 disables blink. Default 530. */
  caretBlinkMs?: number;
  /** Mask the value with bullet characters; preserves real value internally. */
  secureTextEntry?: boolean;
  /** Maximum length in code units. Inserts past this limit are clipped. */
  maxLength?: number;
  /** Select all text on the first focus. */
  selectAllOnFocus?: boolean;
  /** Hook fired before paste; return null to reject, return a string to override. */
  onPaste?: (text: string) => string | null;
  /** Fired when caret/selection changes. */
  onSelectionChange?: (selection: { start: number; end: number }) => void;
  /** Read-only: focus + selection + copy allowed; edits blocked. */
  readOnly?: boolean;
  /** Disabled: prevents focus and edits; cursor shows not_allowed. */
  disabled?: boolean;
  /** Tab key behavior: 'focus' advances focus; 'insert' inserts \t. Default 'focus'. */
  tabBehavior?: 'focus' | 'insert';
  /** Selection highlight color (RRGGBBAA). Default 0x3b82f654. */
  selectionColor?: number;
  /** Show a focus outline when focused. Default true. */
  focusRing?: boolean;
  /** Mobile keyboard hint (no-op on desktop today). */
  inputMode?: 'text' | 'numeric' | 'decimal' | 'email' | 'tel' | 'url' | 'search' | 'none';
  spellCheck?: boolean;
  autoCapitalize?: 'none' | 'sentences' | 'words' | 'characters';
  autoCorrect?: boolean;
}

interface TextInputTextProps {
  key?: string;
  style?: StyleValue;
  children: string;
  imePreedit?: TextPreeditOptions;
  selectionStart?: number;
  selectionEnd?: number;
  caretIndex?: number;
  caretOpacity?: number;
  selectionColor?: number;
  scrollX?: number;
  /** Override wrap budget. `null` disables wrap entirely (single-line). */
  wrapWidth?: number | null;
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

const TextInputText = Component((props: TextInputTextProps): Node => {
  const inherited = useTextStyle();
  const internalText = useInternalTextStyle();
  const internalLayout = useInternalLayout();
  const resolved = splitStyle(props.style);
  const textStyle: TextStyle = {
    ...inherited,
    ...(internalText ?? {}),
    ...resolved.text,
  };
  const text = props.children;
  const rect = internalLayout ? { ...internalLayout } : rectFromStyle(resolved.layout, undefined);
  const wrapWidth = props.wrapWidth;
  if (rect.width === 0 || rect.height === 0) {
    const intrinsic = wrapText(text, textStyle, {
      maxWidth: wrapWidth === null ? undefined : (wrapWidth ?? undefined),
    });
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
        props.scrollX,
        wrapWidth ?? undefined,
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
          scrollOffset: props.scrollX ? { x: props.scrollX } : undefined,
          wrapWidth: wrapWidth,
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
  const [caretPhase, setCaretPhase] = useState(true);
  const [scrollX, setScrollX] = useState(0);
  const [imePreedit, setImePreedit] = useState<{ preedit: string; cursor: number }>({
    preedit: '',
    cursor: 0,
  });

  const dragAnchor = useRef(initial.length);
  const lastClick = useRef({ time: 0, x: 0, y: 0, count: 0 });
  const blinkAccum = useRef({ ms: 0 });
  const firstFocus = useRef(true);
  const historyRef = useRef(createHistory());
  // Auto-scroll while drag-selecting outside the visible viewport.
  // `dir` is the side the mouse exited; `lastX/Y` keep the last drag
  // coords so useFrame can re-extend selection as scrollX advances.
  const dragEdge = useRef<{ dir: -1 | 1; lastX: number; lastY: number } | null>(null);
  // Drag-from-selection state: mousedown happened inside the existing
  // selection range, so we defer caret reset until we know whether the
  // gesture is a click (collapse selection) or a drag-out (startDrag).
  const pendingDragOut = useRef<{ x: number; y: number; text: string } | null>(null);
  const platform = useMemo(() => detectPlatform(), []);

  const rect = rectFromStyle(resolved.layout, useInternalLayout() ?? undefined);
  const disabled = props.disabled === true;
  const readOnly = props.readOnly === true;
  const secure = props.secureTextEntry === true;
  const maxLen = props.maxLength;
  const blinkMs = props.caretBlinkMs ?? 530;
  // Touch the no-op forward-compat props so unused-prop lint stays quiet.
  void props.inputMode;
  void props.spellCheck;
  void props.autoCapitalize;
  void props.autoCorrect;
  const a11y = extractA11yProps(props);
  const a11yState = { ...(a11y.accessibilityState ?? {}) };
  if (a11yState.disabled === undefined && disabled) a11yState.disabled = true;
  if (a11yState.readOnly === undefined && readOnly) a11yState.readOnly = true;
  const a11yValue = a11y.accessibilityValue ?? { text: value };

  const visibleWidth = (): number => {
    const padL = rect.paddingLeft ?? 0;
    const padR = rect.paddingRight ?? 0;
    return Math.max(0, rect.width - padL - padR);
  };

  // Adjust scrollX so the caret at glyph index `idx` is inside the visible
  // viewport. Uses unscrolled (text-local) coordinates.
  const ensureCaretInView = (idx: number): void => {
    const w = visibleWidth();
    if (w <= 0) return;
    const caretX = xAtGlyphIndex(value, textStyle, clampIndex(idx, value));
    let next = scrollX;
    if (caretX < next) next = Math.max(0, caretX - 8);
    else if (caretX > next + w - 1) next = Math.max(0, caretX - w + 8);
    if (next !== scrollX) setScrollX(next);
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

  const indexFromPoint = (x: number, y: number): number => {
    const padX = rect.paddingLeft ?? 0;
    const padY = rect.paddingTop ?? 0;
    const wrapped = wrapText(value, textStyle, { maxWidth: undefined });
    return pointToTextIndex(wrapped, textStyle, x - rect.x - padX + scrollX, y - rect.y - padY);
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

  // Lazy clipboard sink for right-click menu actions, since contextmenu events
  // do not carry a KeyEvent's clipboardText/setClipboardText. The mount layer
  // exposes `__fxe_clipboard` if available; otherwise cut/copy/paste from the
  // edit menu are no-ops.
  // TODO(arch): give event_pipeline a way to attach a ClipboardSink to
  // synthetic mouse events so right-click clipboard actions don't depend on
  // a side-channel global.
  const clipboardSink = (): { read?: () => string; write?: (t: string) => void } | null => {
    return (
      (
        globalThis as {
          __fxe_clipboard?: { read?: () => string; write?: (t: string) => void };
        }
      ).__fxe_clipboard ?? null
    );
  };

  // Caret blink + drag-edge autoscroll. Both run on the frame clock; the
  // blink fast-paths out when not applicable, and the autoscroll fast-paths
  // out when not actively dragging at an edge.
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
      const next = Math.max(0, scrollX + dragEdge.current.dir * delta);
      // Clamp at the right edge so we don't scroll past the text width.
      const maxScroll = Math.max(
        0,
        xAtGlyphIndex(value, textStyle, value.length) - visibleWidth() + 8,
      );
      const clamped = Math.min(next, maxScroll);
      if (clamped !== scrollX) setScrollX(clamped);
      // Re-extend selection toward the new visible end.
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
    componentType: 'TextInput',
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
        // If the press lands inside an existing selection, defer caret
        // reset: a click without movement collapses the selection at the
        // hit point (in onPressOut), and a drag past the slop initiates a
        // text drag-out (in onDrag) via the event_pipeline drag sink.
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
      // If we deferred selection-collapse on press inside a selection, a
      // drag past the slop turns into a drag-out.
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
        // Stay deferred until movement exceeds slop or button releases.
        return;
      }
      const padL = rect.paddingLeft ?? 0;
      const padR = rect.paddingRight ?? 0;
      const leftEdge = rect.x + padL;
      const rightEdge = rect.x + rect.width - padR;
      if (ev.x < leftEdge) {
        dragEdge.current = { dir: -1, lastX: ev.x, lastY: ev.y };
      } else if (ev.x > rightEdge) {
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
      // Press-then-release inside a selection without crossing the drag
      // slop collapses the selection at the click point.
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
          let t = sink?.read?.() ?? '';
          t = t.replace(/\n/g, '');
          if (props.onPaste) {
            const r = props.onPaste(t);
            if (r === null) return;
            t = r;
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
        let t = sink?.read?.() ?? '';
        t = t.replace(/\n/g, '');
        if (props.onPaste) {
          const r = props.onPaste(t);
          if (r === null) return;
          t = r;
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

      // Primary+A select all.
      if (primary && isLetter(key, KEY_A)) {
        setSelection(0, value.length);
        return;
      }
      // Primary+C copy.
      if (primary && isLetter(key, KEY_C)) {
        if (!secure) {
          const t = selectedText();
          if (t.length > 0) keyEvent.setClipboardText?.(t);
        }
        return;
      }
      // Primary+X cut.
      if (primary && isLetter(key, KEY_X)) {
        if (readOnly || secure) return;
        const t = selectedText();
        if (t.length > 0) {
          keyEvent.setClipboardText?.(t);
          replaceSelection('', 'cut');
        }
        return;
      }
      // Primary+V paste.
      if (primary && isLetter(key, KEY_V)) {
        if (readOnly) return;
        let t = keyEvent.clipboardText?.() ?? '';
        t = t.replace(/\n/g, '');
        if (props.onPaste) {
          const r = props.onPaste(t);
          if (r === null) return;
          t = r;
        }
        if (t.length > 0) replaceSelection(t, 'paste');
        return;
      }
      // Primary+Z undo (Shift = redo).
      if (primary && isLetter(key, KEY_Z)) {
        if (shift) doRedo();
        else doUndo();
        return;
      }
      // Primary+Y redo (Windows convention; harmless on macOS).
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

      if (key === KEY_HOME || key === KEY_END) {
        const next = key === KEY_HOME ? 0 : value.length;
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
        if (!readOnly) props.onSubmit?.(value);
        return;
      }

      if (key === KEY_TAB && props.tabBehavior === 'insert' && !readOnly) {
        // NOTE: event_pipeline.dispatchKeyDown advances focus before this
        // handler runs, so the focused TextInput already lost focus by the
        // time we receive Tab. Inserting here is correct only for inputs that
        // re-focus afterwards; full support requires a phase hook in
        // event_pipeline. TODO(P3): add focus-advance phase preempt.
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
  const caretOpacity =
    showCaret && !hasSelectionRange ? (caretPhase ? 1 : 0) : showCaret && hasSelectionRange ? 0 : 0;
  const focusRingOn = focused && !disabled && props.focusRing !== false;

  return View({
    ...props,
    style: [
      {
        minHeight: 34,
        paddingX: theme.spacing.sm,
        paddingY: theme.spacing.xs,
        justifyContent: 'center',
        borderWidth: focusRingOn ? 2 : 1,
        borderColor: focused ? theme.colors.primary : theme.colors.border,
        backgroundColor: theme.colors.surface,
        opacity: disabled ? 0.5 : 1,
        overflow: 'hidden',
      },
      props.style,
    ],
    children: TextInputText({
      style: { color: showPlaceholder ? theme.colors.mutedText : theme.colors.text },
      children: shown,
      selectionStart: showPlaceholder ? undefined : selectionStart,
      selectionEnd: showPlaceholder ? undefined : selectionEnd,
      caretIndex: showCaret ? selectionEnd : undefined,
      caretOpacity,
      selectionColor: props.selectionColor,
      // Single-line: disable wrap so long text scrolls horizontally instead
      // of breaking onto wrapped visual lines.
      wrapWidth: showPlaceholder ? undefined : null,
      scrollX: showPlaceholder ? 0 : scrollX,
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
