// EditableArea — keyboard-driven multi-line editor over a native
// `TextDocument`. Pairs a `LineViewport` for paint with a focusable hit
// target that owns selection state, dispatches edits through `History`,
// and translates GLFW key events into doc mutations.
//
// Scope (v1):
//   - Single cursor (multi-cursor follow-up; the underlying selection model
//     supports it but key handling is single-cursor-first).
//   - Char input via composition events.
//   - Backspace / Delete / Arrow keys / Home / End.
//   - Tab inserts the configured `tabString` (default '  ').
//   - Cmd/Ctrl + Z / Y for undo / redo through `History`.
//   - Enter inserts a newline.
//
// Multi-cursor, block-select, smart-bracket, find-and-replace are
// follow-ups that build on this base via the same selection + history
// surface.

import type { ComposeEvent, KeyEvent } from 'fxe';
import { recordLayout } from '../debug/layout_trace.ts';
import { registerHitTarget } from '../mount/hit_test.ts';
import {
  Component,
  type Node,
  useEffect,
  useId,
  useInternalLayout,
  useRef,
  useState,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { isPrimaryModifier, MOD_SHIFT } from '../text/edit_model.ts';
import { rectFromStyle } from './common.ts';
import { type LineDecorationFn, type LineDecorations, LineViewport } from './LineViewport.ts';

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
const KEY_Z = 90;
const KEY_Y = 89;

interface EditDispatcher {
  dispatch(
    edits: ReadonlyArray<{ start: number; removed: number; inserted: string }>,
    opts?: { origin?: string; break?: boolean },
  ): unknown;
  undo(): boolean;
  redo(): boolean;
  breakCoalescing?(): void;
}

export interface EditableAreaProps {
  key?: string;
  style?: StyleValue;
  document: TextDocument;
  /** History instance for undo/redo. Optional but recommended. */
  history?: EditDispatcher;
  lineHeight: number;
  /** String inserted on Tab. Default `'  '` (2 spaces). */
  tabString?: string;
  /** Visual tab stop in px — independent of `tabString`. */
  tabSize?: number;
  showWhitespace?: boolean;
  textColor?: number;
  /** Decoration provider forwarded to the underlying LineViewport. */
  getLineDecorations?: LineDecorationFn;
  /** Called whenever the cursor moves, even without an edit. */
  onCursorChange?: (line: number, col: number) => void;
  /** Y scroll offset (logical pixels). */
  scrollY?: number;
  onScrollChange?: (scrollY: number) => void;
}

export const EditableArea = Component((props: EditableAreaProps): Node => {
  const id = useId();
  const internalLayout = useInternalLayout();
  const resolved = splitStyle(props.style);
  const rect = internalLayout ? { ...internalLayout } : rectFromStyle(resolved.layout, undefined);
  recordLayout({
    component: 'EditableArea',
    rect,
    hasParentLayout: internalLayout !== null,
    styleWidth: resolved.layout.width,
    styleHeight: resolved.layout.height,
  });

  // Cursor as offset; selection range as anchor/focus (anchor = where the
  // selection started, focus = current cursor end).
  const [anchor, setAnchor] = useState(0);
  const [focus, setFocus] = useState(0);
  const wantedCol = useRef<number>(-1);
  const tabString = props.tabString ?? '  ';

  const doc = props.document;
  const dispatch = (
    edits: ReadonlyArray<{ start: number; removed: number; inserted: string }>,
    opts?: { origin?: string; break?: boolean },
  ): void => {
    if (props.history) {
      props.history.dispatch(edits, opts);
    } else {
      doc.applyBatch(edits);
    }
  };
  const undo = (): boolean => (props.history ? props.history.undo() : false);
  const redo = (): boolean => (props.history ? props.history.redo() : false);

  const moveTo = (offset: number, extend: boolean): void => {
    const clamped = Math.max(0, Math.min(doc.length(), offset));
    setFocus(clamped);
    if (!extend) setAnchor(clamped);
    wantedCol.current = -1;
    if (props.onCursorChange) {
      const lc = doc.offsetToLineCol(clamped);
      props.onCursorChange(lc.line, lc.col);
    }
  };

  const moveByLine = (delta: number, extend: boolean): void => {
    const lc = doc.offsetToLineCol(focus);
    const targetLine = Math.max(0, Math.min(doc.lineCount() - 1, lc.line + delta));
    const wantCol = wantedCol.current >= 0 ? wantedCol.current : lc.col;
    const newOffset = doc.lineColToOffset(targetLine, wantCol);
    setFocus(newOffset);
    if (!extend) setAnchor(newOffset);
    wantedCol.current = wantCol;
    if (props.onCursorChange) {
      const out = doc.offsetToLineCol(newOffset);
      props.onCursorChange(out.line, out.col);
    }
  };

  const ordered = (): { start: number; end: number } => {
    return anchor <= focus ? { start: anchor, end: focus } : { start: focus, end: anchor };
  };

  const replaceSelection = (text: string, origin: string): void => {
    const { start, end } = ordered();
    dispatch([{ start, removed: end - start, inserted: text }], { origin });
    const newPos = start + text.length;
    setAnchor(newPos);
    setFocus(newPos);
    wantedCol.current = -1;
  };

  const onKeyDown = (raw: unknown): void => {
    const ev = raw as KeyEvent;
    const mod = ev.modifiers ?? 0;
    const shift = (mod & MOD_SHIFT) !== 0;
    const primary = isPrimaryModifier(mod);

    // Undo / redo.
    if (primary && ev.key === KEY_Z) {
      if (shift) redo();
      else undo();
      return;
    }
    if (primary && ev.key === KEY_Y) {
      redo();
      return;
    }

    switch (ev.key) {
      case KEY_LEFT:
        moveTo(focus - 1, shift);
        return;
      case KEY_RIGHT:
        moveTo(focus + 1, shift);
        return;
      case KEY_UP:
        moveByLine(-1, shift);
        return;
      case KEY_DOWN:
        moveByLine(1, shift);
        return;
      case KEY_HOME: {
        const lc = doc.offsetToLineCol(focus);
        moveTo(doc.lineToOffset(lc.line), shift);
        return;
      }
      case KEY_END: {
        const lc = doc.offsetToLineCol(focus);
        moveTo(doc.lineRange(lc.line).end, shift);
        return;
      }
      case KEY_BACKSPACE: {
        const { start, end } = ordered();
        if (end > start) {
          replaceSelection('', 'delete');
        } else if (start > 0) {
          dispatch([{ start: start - 1, removed: 1, inserted: '' }], { origin: 'delete' });
          setAnchor(start - 1);
          setFocus(start - 1);
        }
        return;
      }
      case KEY_DELETE: {
        const { start, end } = ordered();
        if (end > start) {
          replaceSelection('', 'delete');
        } else if (start < doc.length()) {
          dispatch([{ start, removed: 1, inserted: '' }], { origin: 'delete' });
        }
        return;
      }
      case KEY_ENTER:
        replaceSelection('\n', 'newline');
        if (props.history?.breakCoalescing) props.history.breakCoalescing();
        return;
      case KEY_TAB:
        replaceSelection(tabString, 'indent');
        return;
      default:
        return;
    }
  };

  const onCompose = (ev: ComposeEvent): void => {
    // GLFW char/codepoint composition. Insert the typed text at the cursor.
    const composed = ev as ComposeEvent & { text?: string; codepoint?: number };
    let inserted = '';
    if (typeof composed.text === 'string') {
      inserted = composed.text;
    } else if (typeof composed.codepoint === 'number' && composed.codepoint > 0) {
      inserted = String.fromCodePoint(composed.codepoint);
    }
    if (inserted.length === 0) return;
    if (inserted === '\n' || inserted === '\r') {
      // Newlines arrive via key events on most platforms; ignore here.
      return;
    }
    replaceSelection(inserted, 'type');
  };

  // Re-render on doc revision.
  useEffect(() => {
    const subId = doc.subscribe(() => {
      // setState noop to bump the fiber.
      setFocus((f) => f);
    });
    return () => {
      doc.unsubscribe(subId);
    };
  }, [doc]);

  registerHitTarget({
    id,
    rect,
    cursor: 'ibeam',
    a11y: {},
    componentType: 'EditableArea',
    tabIndex: 0,
    onFocus: () => {
      // selected: ensure rect re-paints
    },
    onKeyDown,
    onCompose,
  });

  // Wrap the user's getLineDecorations to add primary-cursor selection rects.
  const fontSize = typeof resolved.text.fontSize === 'number' ? resolved.text.fontSize : 14;
  const charW = fontSize * 0.6;
  const range = ordered();
  const lineHeight = props.lineHeight;
  const decorate: LineDecorationFn = (line) => {
    const userDecorations: LineDecorations | null = props.getLineDecorations
      ? props.getLineDecorations(line)
      : null;
    const lineRange = doc.lineRange(line);
    if (range.end <= lineRange.start || range.start > lineRange.end) {
      return userDecorations;
    }
    const selStart = Math.max(range.start, lineRange.start);
    const selEnd = Math.min(range.end, lineRange.end);
    const colStart = selStart - lineRange.start;
    const colEnd = selEnd - lineRange.start;
    const widthCols = Math.max(colEnd - colStart, range.end > lineRange.end ? 1 : 0);
    if (widthCols <= 0 && colStart !== colEnd) return userDecorations;
    const x = colStart * charW;
    const w = widthCols * charW;
    const rects = new Float32Array([x, 0, w, lineHeight]);
    return {
      ...(userDecorations ?? {}),
      selectionRects:
        userDecorations?.selectionRects && userDecorations.selectionRects.length > 0
          ? mergeRects(userDecorations.selectionRects, rects)
          : rects,
    };
  };

  return LineViewport({
    style: props.style,
    document: doc,
    lineHeight: props.lineHeight,
    getLineDecorations: decorate,
    tabSize: props.tabSize,
    showWhitespace: props.showWhitespace,
    textColor: props.textColor,
    scrollY: props.scrollY,
    onClickPosition: (line, col) => {
      const off = doc.lineColToOffset(line, col);
      setAnchor(off);
      setFocus(off);
      wantedCol.current = col;
      if (props.onCursorChange) props.onCursorChange(line, col);
    },
  });
}, 'EditableArea');

function mergeRects(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}
