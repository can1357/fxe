import { type HitTarget, hitTargets } from './hit_test.ts';

let focusedId: string | null = null;

export function focusTarget(): HitTarget | null;
export function focusTarget(id: string | 'next' | 'previous'): HitTarget | null;
export function focusTarget(id?: string | 'next' | 'previous'): HitTarget | null {
  const focusables = hitTargets().filter(
    (t) => t.onFocus || t.onBlur || t.onKeyDown || t.onKeyPress,
  );
  if (id === undefined) return focusables.find((t) => t.id === focusedId) ?? null;
  const previous = focusables.find((t) => t.id === focusedId) ?? null;
  let next: HitTarget | null = null;
  if (id === 'next' || id === 'previous') {
    if (focusables.length === 0) return null;
    const at = Math.max(
      0,
      focusables.findIndex((t) => t.id === focusedId),
    );
    const delta = id === 'next' ? 1 : -1;
    next = focusables[(at + delta + focusables.length) % focusables.length];
  } else {
    next = focusables.find((t) => t.id === id) ?? null;
  }
  if (next?.id === previous?.id) return next;
  previous?.onBlur?.();
  focusedId = next?.id ?? null;
  next?.onFocus?.();
  return next;
}

export function focusedTargetId(): string | null {
  return focusedId;
}

export function clearFocus(): void {
  focusedId = null;
}
