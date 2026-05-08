import type {
  ComposeEvent,
  CursorKind,
  KeyEvent,
  KeypressEvent,
  MouseButtonEvent,
  MouseMoveEvent,
  WheelEvent,
} from 'fxe';
import { focusTarget } from './focus.ts';
import { type HitTarget, hitTargets, hitTest, makeSyntheticEvent } from './hit_test.ts';

let hovered: HitTarget | null = null;
let pressed: HitTarget | null = null;
let captured: HitTarget | null = null;
let focusTrapGroup: string | null = null;

function isFocusable(target: HitTarget): boolean {
  return Boolean(target.onFocus || target.onBlur || target.onKeyDown || target.onKeyPress);
}

function orderedFocusableTargets(): HitTarget[] {
  const scoped = hitTargets().filter(
    (target) =>
      isFocusable(target) &&
      (target.tabIndex ?? 0) >= 0 &&
      (focusTrapGroup == null || target.focusGroup === focusTrapGroup),
  );
  return [...scoped].sort((a, b) => {
    const byTabIndex = (a.tabIndex ?? 0) - (b.tabIndex ?? 0);
    if (byTabIndex !== 0) return byTabIndex;
    return a.z - b.z;
  });
}

function focusRelative(direction: 'next' | 'previous'): HitTarget | null {
  const focusables = orderedFocusableTargets();
  if (focusables.length === 0) return null;
  const current = focusTarget();
  // Match historical behavior: when nothing is focused, treat the current
  // position as 0 (so first 'next' Tab focuses index 1, first 'previous' Tab
  // focuses index length-1). Tests rely on this; D5 sort still applies above.
  const idx = current ? focusables.findIndex((target) => target.id === current.id) : -1;
  const at = Math.max(0, idx);
  const delta = direction === 'next' ? 1 : -1;
  const wrapped = (at + delta + focusables.length) % focusables.length;
  return focusTarget(focusables[wrapped].id);
}

export function setFocusTrapGroup(id: string | null): void {
  focusTrapGroup = id;
}

export interface CursorSink {
  setCursor?(kind: CursorKind): void;
}

export interface ClipboardSink {
  clipboardText?(): string;
  setClipboardText?(text: string): void;
}

export interface DragOutPayload {
  files?: string[];
  text?: string;
  html?: string;
}

/** Sink that initiates an OS-level drag-out from a synthetic mouse drag. */
export interface DragSink {
  startDrag?(payload: DragOutPayload): boolean;
}

export function dispatchMouseMove(
  ev: MouseMoveEvent,
  cursorSink?: CursorSink,
  dragSink?: DragSink,
): void {
  if (captured) {
    const synthetic = makeSyntheticEvent(ev, ev.x, ev.y) as ReturnType<
      typeof makeSyntheticEvent<MouseMoveEvent>
    > & { requestDragOut?: (payload: DragOutPayload) => boolean };
    let dragStarted = false;
    if (dragSink?.startDrag) {
      synthetic.requestDragOut = (payload) => {
        const ok = dragSink.startDrag?.(payload) ?? false;
        if (ok) {
          // Hand control to the OS drag session: stop further drag dispatch
          // until the next mousedown so the captured target can't extend
          // its selection while the user is dragging out.
          captured = null;
          pressed = null;
          dragStarted = true;
        }
        return ok;
      };
    }
    captured.onDrag?.(synthetic);
    if (dragStarted) return;
  }
  const next = hitTest(ev.x, ev.y);
  if (next !== hovered) {
    if (hovered?.onHoverOut) hovered.onHoverOut(makeSyntheticEvent(ev, ev.x, ev.y));
    hovered = next;
    if (hovered?.onHoverIn) hovered.onHoverIn(makeSyntheticEvent(ev, ev.x, ev.y));
    cursorSink?.setCursor?.(hovered?.cursor ?? 'arrow');
  }
}

export function dispatchMouseDown(ev: MouseButtonEvent): void {
  const target = hitTest(ev.x, ev.y);
  if (ev.button === 1) {
    target?.onContextMenu?.(makeSyntheticEvent(ev, ev.x, ev.y));
    return;
  }
  if (ev.button !== 0) return;
  pressed = target;
  captured = pressed;
  if (!pressed) return;
  focusTarget(pressed.id);
  pressed.onPressIn?.(makeSyntheticEvent(ev, ev.x, ev.y));
}

export function dispatchMouseUp(ev: MouseButtonEvent): void {
  if (ev.button !== 0) return;
  const released = hitTest(ev.x, ev.y);
  const down = pressed;
  pressed = null;
  captured = null;
  if (!down) return;
  const event = makeSyntheticEvent(ev, ev.x, ev.y);
  down.onPressOut?.(event);
  if (released?.id === down.id) down.onPress?.(event);
}

export function dispatchWheel(ev: WheelEvent & { x?: number; y?: number }): void {
  const x = ev.x ?? hovered?.rect.x ?? 0;
  const y = ev.y ?? hovered?.rect.y ?? 0;
  const target = hitTest(x, y) ?? hovered;
  target?.onWheel?.({ ...makeSyntheticEvent(ev, x, y), dx: ev.dx, dy: ev.dy });
}

export function dispatchKeyDown(ev: KeyEvent, clipboardSink?: ClipboardSink): void {
  if (ev.key === 258) focusRelative(ev.modifiers & 1 ? 'previous' : 'next');
  const target = focusTarget();
  const routed = clipboardSink
    ? {
        ...ev,
        clipboardText: () => clipboardSink.clipboardText?.(),
        setClipboardText: (text: string) => clipboardSink.setClipboardText?.(text),
      }
    : ev;
  target?.onKeyDown?.(routed);
}

export function dispatchKeyPress(ev: KeypressEvent): void {
  focusTarget()?.onKeyPress?.(ev);
}

export function dispatchCompose(ev: ComposeEvent): void {
  focusTarget()?.onCompose?.(ev);
}

export function resetEventPipeline(): void {
  hovered = null;
  pressed = null;
  captured = null;
  focusTrapGroup = null;
}
