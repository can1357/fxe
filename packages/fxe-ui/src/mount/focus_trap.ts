import { useEffect } from '../reconciler/fiber.ts';
import { setFocusTrapGroup } from './event_pipeline.ts';

/**
 * Trap Tab / Shift+Tab focus traversal inside a focus group.
 *
 * Use inside a modal/dialog component; pass a stable groupId derived from
 * useId() and render every focusable descendant with the same
 * `accessibilityFocusGroup={groupId}` (mapped to HitTarget.focusGroup at
 * registration time).
 *
 * Focus trap is process-wide singleton: the most-recently-enabled trap wins.
 */
export function useFocusTrap(groupId: string | null, enabled = true): void {
  useEffect(() => {
    if (!enabled || groupId == null) return;
    setFocusTrapGroup(groupId);
    return () => setFocusTrapGroup(null);
  }, [groupId, enabled]);
}

export { setFocusTrapGroup };
