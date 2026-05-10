// EditableArea — keyboard-driven multi-line editor over a native
// `TextDocument`. Pairs a `LineViewport` for paint with a focusable hit
// target that owns selection state, dispatches edits through `History`,
// and translates GLFW key events into doc mutations.

import type { ComposeEvent, KeyEvent, MouseButtonEvent } from 'fxe';
import { MultiRangeSelection, type Range } from 'fxe-doc';
import { recordLayout } from '../debug/layout_trace.ts';
import { registerHitTarget, type SyntheticEvent } from '../mount/hit_test.ts';
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
import { isPrimaryModifier, MOD_ALT, MOD_SHIFT } from '../text/edit_model.ts';
import { rectFromStyle } from './common.ts';
import { addNextOccurrence, applyEditsAtRanges, expandLines } from './editable_area_logic.ts';
import { type LineDecorationFn, type LineDecorations, LineViewport } from './LineViewport.ts';

const KEY_ESCAPE = 256;
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
const KEY_D = 68;
const KEY_L = 76;
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

  const [sel, setSel] = useState(() => MultiRangeSelection.cursor(0));
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

  const notifyPrimaryCursor = (nextSel: MultiRangeSelection): void => {
    if (!props.onCursorChange) return;
    const lc = doc.offsetToLineCol(nextSel.primaryRange().focus);
    props.onCursorChange(lc.line, lc.col);
  };

  const commitSelection = (nextSel: MultiRangeSelection, nextWantedCol: number): void => {
    setSel(nextSel);
    wantedCol.current = nextWantedCol;
    notifyPrimaryCursor(nextSel);
  };

  const moveSelection = (
    mapper: (range: Range, index: number) => Range,
    nextWantedCol: number,
  ): void => {
    commitSelection(sel.with(sel.ranges.map(mapper), sel.primary), nextWantedCol);
  };

  const moveTo = (delta: number, extend: boolean): void => {
    moveSelection((range) => {
      const nextOffset = Math.max(0, Math.min(doc.length(), range.focus + delta));
      return extend
        ? { anchor: range.anchor, focus: nextOffset }
        : { anchor: nextOffset, focus: nextOffset };
    }, -1);
  };

  const moveByLine = (delta: number, extend: boolean): void => {
    const primaryCol = doc.offsetToLineCol(sel.primaryRange().focus).col;
    const nextWantedCol = wantedCol.current >= 0 ? wantedCol.current : primaryCol;
    moveSelection((range, index) => {
      const lc = doc.offsetToLineCol(range.focus);
      const targetLine = Math.max(0, Math.min(doc.lineCount() - 1, lc.line + delta));
      const targetCol = index === sel.primary ? nextWantedCol : lc.col;
      const nextOffset = doc.lineColToOffset(targetLine, targetCol);
      return extend
        ? { anchor: range.anchor, focus: nextOffset }
        : { anchor: nextOffset, focus: nextOffset };
    }, nextWantedCol);
  };

  const moveToLineBoundary = (which: 'start' | 'end', extend: boolean): void => {
    moveSelection((range) => {
      const lc = doc.offsetToLineCol(range.focus);
      const nextOffset = which === 'start' ? doc.lineToOffset(lc.line) : doc.lineRange(lc.line).end;
      return extend
        ? { anchor: range.anchor, focus: nextOffset }
        : { anchor: nextOffset, focus: nextOffset };
    }, -1);
  };

  const applySelectionEdit = (text: string, origin: string): void => {
    const { edits, nextSel } = applyEditsAtRanges(doc, sel, text, origin);
    if (edits.length === 0) return;
    dispatch(edits, { origin });
    commitSelection(nextSel, -1);
  };

  const onKeyDown = (raw: unknown): void => {
    const ev = raw as KeyEvent;
    const mod = ev.modifiers ?? 0;
    const shift = (mod & MOD_SHIFT) !== 0;
    const primary = isPrimaryModifier(mod);

    if (primary && ev.key === KEY_Z) {
      if (shift) redo();
      else undo();
      return;
    }
    if (primary && ev.key === KEY_Y) {
      redo();
      return;
    }
    if (primary && ev.key === KEY_D) {
      const nextSel = addNextOccurrence(doc, sel);
      if (nextSel !== sel) commitSelection(nextSel, -1);
      return;
    }
    if (primary && ev.key === KEY_L) {
      commitSelection(expandLines(doc, sel), -1);
      return;
    }

    switch (ev.key) {
      case KEY_LEFT:
        moveTo(-1, shift);
        return;
      case KEY_RIGHT:
        moveTo(1, shift);
        return;
      case KEY_UP:
        moveByLine(-1, shift);
        return;
      case KEY_DOWN:
        moveByLine(1, shift);
        return;
      case KEY_HOME:
        moveToLineBoundary('start', shift);
        return;
      case KEY_END:
        moveToLineBoundary('end', shift);
        return;
      case KEY_BACKSPACE:
        applySelectionEdit('', 'delete-backward');
        return;
      case KEY_DELETE:
        applySelectionEdit('', 'delete-forward');
        return;
      case KEY_ENTER:
        applySelectionEdit('\n', 'newline');
        if (props.history?.breakCoalescing) props.history.breakCoalescing();
        return;
      case KEY_TAB:
        applySelectionEdit(tabString, 'indent');
        return;
      case KEY_ESCAPE:
        commitSelection(sel.collapseToPrimary(), -1);
        return;
      default:
        return;
    }
  };

  const onCompose = (ev: ComposeEvent): void => {
    const composed = ev as ComposeEvent & { text?: string; codepoint?: number };
    let inserted = '';
    if (typeof composed.text === 'string') {
      inserted = composed.text;
    } else if (typeof composed.codepoint === 'number' && composed.codepoint > 0) {
      inserted = String.fromCodePoint(composed.codepoint);
    }
    if (inserted.length === 0) return;
    if (inserted === '\n' || inserted === '\r') {
      return;
    }
    applySelectionEdit(inserted, 'type');
  };

  useEffect(() => {
    const subId = doc.subscribe(() => {
      setSel((current) => current.with(current.ranges, current.primary));
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

  const fontSize = typeof resolved.text.fontSize === 'number' ? resolved.text.fontSize : 14;
  const charW = fontSize * 0.6;
  const lineHeight = props.lineHeight;
  const decorate: LineDecorationFn = (line) => {
    const userDecorations: LineDecorations | null = props.getLineDecorations
      ? props.getLineDecorations(line)
      : null;
    const lineRange = doc.lineRange(line);
    let selectionRects = userDecorations?.selectionRects ?? null;
    forEachRange(sel, (range) => {
      if (range.end <= lineRange.start || range.start > lineRange.end) return;
      const selStart = Math.max(range.start, lineRange.start);
      const selEnd = Math.min(range.end, lineRange.end);
      const colStart = selStart - lineRange.start;
      const colEnd = selEnd - lineRange.start;
      const widthCols = Math.max(colEnd - colStart, range.end > lineRange.end ? 1 : 0);
      if (widthCols <= 0 && colStart === colEnd) return;
      const rects = new Float32Array([colStart * charW, 0, widthCols * charW, lineHeight]);
      selectionRects =
        selectionRects && selectionRects.length > 0 ? mergeRects(selectionRects, rects) : rects;
    });
    if (!selectionRects) return userDecorations;
    return {
      ...(userDecorations ?? {}),
      selectionRects,
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
    onClickPosition: (line, col, ev) => {
      const off = doc.lineColToOffset(line, col);
      const mouse = ev as SyntheticEvent<MouseButtonEvent>;
      const nextSel =
        (mouse.nativeEvent.modifiers & MOD_ALT) !== 0
          ? sel.add({ anchor: off, focus: off })
          : MultiRangeSelection.cursor(off);
      commitSelection(nextSel, col);
    },
  });
}, 'EditableArea');

function orderedRange(range: Range): { start: number; end: number } {
  return range.anchor <= range.focus
    ? { start: range.anchor, end: range.focus }
    : { start: range.focus, end: range.anchor };
}

function forEachRange(
  selection: MultiRangeSelection,
  fn: (range: { start: number; end: number }) => void,
): void {
  selection.ranges
    .map(orderedRange)
    .sort((a, b) => a.start - b.start || a.end - b.end)
    .forEach(fn);
}

function mergeRects(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}
