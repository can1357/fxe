import type {
  CursorKind,
  ComposeEvent,
  KeyEvent,
  KeypressEvent,
  MouseButtonEvent,
  MouseMoveEvent,
  WheelEvent,
} from 'fxe';
import { focusTarget } from './focus.ts';
import { type HitTarget, hitTest, makeSyntheticEvent } from './hit_test.ts';

let hovered: HitTarget | null = null;
let pressed: HitTarget | null = null;
let captured: HitTarget | null = null;

export interface CursorSink {
  setCursor?(kind: CursorKind): void;
}

export interface ClipboardSink {
  clipboardText?(): string;
  setClipboardText?(text: string): void;
}

export function dispatchMouseMove(ev: MouseMoveEvent, cursorSink?: CursorSink): void {
  if (captured) {
    captured.onDrag?.(makeSyntheticEvent(ev, ev.x, ev.y));
  }
  const next = hitTest(ev.x, ev.y);
  if (next !== hovered) {
    if (hovered?.onHoverOut) hovered.onHoverOut(makeSyntheticEvent(ev, ev.x, ev.y));
    hovered = next;
    if (hovered?.onHoverIn) hovered.onHoverIn(makeSyntheticEvent(ev, ev.x, ev.y));
    if (cursorSink && hovered?.cursor) cursorSink.setCursor?.(hovered.cursor);
  }
}

export function dispatchMouseDown(ev: MouseButtonEvent): void {
  pressed = hitTest(ev.x, ev.y);
  captured = ev.button === 0 ? pressed : null;
  if (!pressed) return;
  focusTarget(pressed.id);
  pressed.onPressIn?.(makeSyntheticEvent(ev, ev.x, ev.y));
}

export function dispatchMouseUp(ev: MouseButtonEvent): void {
  const released = hitTest(ev.x, ev.y);
  const down = pressed;
  pressed = null;
  if (ev.button === 0) captured = null;
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
  if (ev.key === 258) focusTarget(ev.modifiers & 1 ? 'previous' : 'next');
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
}
