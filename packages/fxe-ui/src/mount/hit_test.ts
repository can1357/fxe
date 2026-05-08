import type { AccessibilityProps } from '../a11y/types.ts';
import type { ComposeEvent, CursorKind, MouseButtonEvent } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';

export interface SyntheticEvent<T = unknown> {
  nativeEvent: T;
  x: number;
  y: number;
  defaultPrevented: boolean;
  propagationStopped: boolean;
  preventDefault(): void;
  stopPropagation(): void;
}

export interface HitTarget {
  id: string;
  rect: LayoutResult;
  z: number;
  /** Tab traversal order. Negative = unfocusable. Default 0 (registration order). */
  tabIndex?: number;
  /** Logical group id; focus trap can scope traversal to a single group. */
  focusGroup?: string;
  componentType?: string;
  a11y?: AccessibilityProps;
  cursor?: CursorKind;
  onHoverIn?: (ev: SyntheticEvent) => void;
  onHoverOut?: (ev: SyntheticEvent) => void;
  onPressIn?: (ev: SyntheticEvent) => void;
  onPressOut?: (ev: SyntheticEvent) => void;
  onPress?: (ev: SyntheticEvent) => void;
  onContextMenu?: (ev: SyntheticEvent<MouseButtonEvent>) => void;
  onDrag?: (ev: SyntheticEvent) => void;
  onWheel?: (ev: SyntheticEvent & { dx: number; dy: number }) => void;
  onFocus?: () => void;
  onBlur?: () => void;
  onKeyDown?: (ev: unknown) => void;
  onKeyPress?: (ev: unknown) => void;
  onCompose?: (ev: ComposeEvent) => void;
}
function isInteractiveTarget(target: HitTarget): boolean {
  return Boolean(
    target.onHoverIn ||
      target.onHoverOut ||
      target.onPressIn ||
      target.onPressOut ||
      target.onPress ||
      target.onContextMenu ||
      target.onDrag ||
      target.onWheel ||
      target.onFocus ||
      target.onBlur ||
      target.onKeyDown ||
      target.onKeyPress ||
      target.onCompose,
  );
}

const targets: HitTarget[] = [];
let nextZ = 0;

export function clearHitTargets(): void {
  targets.length = 0;
  nextZ = 0;
}

export function registerHitTarget(target: Omit<HitTarget, 'z'> & { z?: number }): void {
  targets.push({ ...target, z: target.z ?? nextZ++ });
}

export function hitTargets(): readonly HitTarget[] {
  return targets;
}

// Snapshot helpers used by the Layer cache to re-emit hit targets that were
// registered during a Layer's last rebuild when a subsequent frame hits the
// cached buffer (and therefore skips descent into the children that originally
// called `registerHitTarget`).  Without this, only the very first frame that
// rebuilds a Layer leaves any hit targets behind; all subsequent cached
// frames silently drop them, so clicks after the first stable frame stop
// reaching Pressable / TextInput / ScrollView / selectable Text.
export function hitTargetCount(): number {
  return targets.length;
}

export function captureHitTargetsSince(start: number): HitTarget[] {
  return targets.slice(start);
}

// Re-push a previously captured slice. We allocate fresh z values so the
// replay slots into the current frame's painter ordering.
export function replayHitTargets(captured: readonly HitTarget[]): void {
  for (const t of captured) {
    targets.push({ ...t, z: nextZ++ });
  }
}

export function hitTest(x: number, y: number): HitTarget | null {
  let passive: HitTarget | null = null;
  for (const target of [...targets].sort((a, b) => b.z - a.z)) {
    const r = target.rect;
    if (x < r.x || y < r.y || x > r.x + r.width || y > r.y + r.height) continue;
    if (isInteractiveTarget(target)) return target;
    passive ??= target;
  }
  return passive;
}

export function makeSyntheticEvent<T>(nativeEvent: T, x: number, y: number): SyntheticEvent<T> {
  return {
    nativeEvent,
    x,
    y,
    defaultPrevented: false,
    propagationStopped: false,
    preventDefault() {
      this.defaultPrevented = true;
    },
    stopPropagation() {
      this.propagationStopped = true;
    },
  };
}
