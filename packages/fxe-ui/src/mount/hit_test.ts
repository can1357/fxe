import type { ComposeEvent, CursorKind, MouseButtonEvent } from 'fxe';
import type { AccessibilityProps } from '../a11y/types.ts';
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
  onPressIn?: (ev: SyntheticEvent<MouseButtonEvent>) => void;
  onPressOut?: (ev: SyntheticEvent<MouseButtonEvent>) => void;
  onPress?: (ev: SyntheticEvent<MouseButtonEvent>) => void;
  onContextMenu?: (ev: SyntheticEvent<MouseButtonEvent>) => void;
  onDrag?: (ev: SyntheticEvent) => void;
  onWheel?: (ev: SyntheticEvent & { dx: number; dy: number }) => void;
  onFocus?: () => void;
  onBlur?: () => void;
  onKeyDown?: (ev: unknown) => void;
  onKeyPress?: (ev: unknown) => void;
  onCompose?: (ev: ComposeEvent) => void;
  /**
   * Routed application-menu commands (Edit menu actions installed via
   * `installApplicationEditMenu`). Implementing components — currently
   * `TextInput` and `TextArea` — handle `'undo' | 'redo' | 'cut' | 'copy' |
   * 'paste' | 'selectAll'`.
   */
  onEditCommand?: (action: string) => void;
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
      target.onCompose ||
      target.onEditCommand,
  );
}

const targets: HitTarget[] = [];
let nextZ = 0;

const kSharedReplayHitTarget = Symbol('fxe-ui.sharedReplayHitTarget');
type ReplayHitTarget = HitTarget & { [kSharedReplayHitTarget]?: boolean };

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

export function materializeHitTarget(targets: HitTarget[], index: number): HitTarget {
  const target = targets[index] as ReplayHitTarget;
  if (target[kSharedReplayHitTarget] !== true) return target;
  const clone = { ...target };
  clone[kSharedReplayHitTarget] = false;
  targets[index] = clone;
  return clone;
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
  const captured = targets.slice(start);
  for (let i = 0; i < captured.length; ++i) {
    (captured[i] as ReplayHitTarget)[kSharedReplayHitTarget] = true;
  }
  return captured;
}

// Re-push a previously captured slice. Cached frames keep the original target
// objects alive and only refresh z-order when the slice moved relative to
// earlier targets. Rect clipping does copy-on-write via
// `materializeHitTarget()` so replay itself stays allocation-free.
export function replayHitTargets(captured: readonly HitTarget[]): void {
  const count = captured.length;
  if (count === 0) return;
  const zStart = nextZ;
  nextZ += count;
  const zEnd = zStart + count - 1;
  if (captured[0].z !== zStart || captured[count - 1].z !== zEnd) {
    for (let i = 0; i < count; ++i) {
      captured[i].z = zStart + i;
    }
  }
  if (count < 8192) {
    targets.push(...captured);
    return;
  }
  for (let i = 0; i < count; ++i) {
    targets.push(captured[i]);
  }
}

export function hitTest(x: number, y: number): HitTarget | null {
  let bestInteractive: HitTarget | null = null;
  let bestPassive: HitTarget | null = null;
  let bestBlockedByPassive: HitTarget | null = null;
  for (let i = 0; i < targets.length; ++i) {
    const target = targets[i];
    const r = target.rect;
    if (x < r.x || y < r.y || x > r.x + r.width || y > r.y + r.height) continue;
    if (isInteractiveTarget(target)) {
      if (bestInteractive === null || target.z > bestInteractive.z) bestInteractive = target;
      continue;
    }
    if (bestPassive === null || target.z > bestPassive.z) bestPassive = target;
    if (
      target.componentType !== 'Text' &&
      (bestBlockedByPassive === null || target.z > bestBlockedByPassive.z)
    ) {
      bestBlockedByPassive = target;
    }
  }
  return bestInteractive ?? bestBlockedByPassive ?? bestPassive;
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
