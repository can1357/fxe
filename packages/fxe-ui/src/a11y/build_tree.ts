import type {
  AccessibilityNodeSnapshot,
  AccessibilityProps,
  AccessibilityRole,
  AccessibilityState,
  AccessibilityTreeSnapshot,
  AccessibilityValue,
} from './types.ts';
import { expandVirtualDescendants, getVirtualSources } from './virtual.ts';

/**
 * Public input shape. Apps emit this from the reconciler post-commit. The
 * exact reconciler integration (walking retained fibers) lands when component
 * props start producing AccessibilityProps; v1 lets external callers feed in
 * a structural input directly.
 */
export interface AccessibilityFiberLike {
  /** Stable ID derived from accessibilityId or fiber path. */
  id: string;
  /** Component display name; used to default role when accessibilityRole is unset. */
  componentType: string;
  /** AccessibilityProps from the component. */
  a11y: AccessibilityProps;
  /** Layout rect in window coordinates. */
  rect: { x: number; y: number; width: number; height: number };
  /** Children in render order. */
  children: AccessibilityFiberLike[];
  /** Fallback label when accessibilityLabel is unset (e.g. flattened text). */
  fallbackLabel?: string;
}

function normalizeValue(
  v: AccessibilityProps['accessibilityValue'],
): AccessibilityValue | undefined {
  if (v === undefined) return;
  if (typeof v === 'string') return { text: v };
  if (typeof v === 'number') return { now: v };
  return { ...v };
}

function defaultRoleFor(componentType: string, a11y: AccessibilityProps): AccessibilityRole {
  if (a11y.accessibilityRole) return a11y.accessibilityRole;
  switch (componentType) {
    case 'View':
      return 'group';
    case 'Text':
      return 'text';
    case 'Pressable':
      return 'button';
    case 'Button':
      return 'button';
    case 'TextInput':
      return 'textbox';
    case 'TextArea':
      return 'textbox';
    case 'ScrollView':
      return 'scrollview';
    case 'VirtualList':
      return 'list';
    case 'Image':
      return 'image';
    default:
      return 'group';
  }
}

function collectFallbackLabel(node: AccessibilityFiberLike): string {
  if (node.fallbackLabel) return node.fallbackLabel;
  // Flatten descendants until the first labelled boundary.
  const parts: string[] = [];
  for (const child of node.children) {
    if (child.a11y.accessible === false) continue;
    if (child.a11y.accessibilityLabel) {
      parts.push(child.a11y.accessibilityLabel);
      continue;
    }
    if (child.fallbackLabel) {
      parts.push(child.fallbackLabel);
      continue;
    }
    parts.push(collectFallbackLabel(child));
  }
  return parts
    .filter((s) => s.length > 0)
    .join(' ')
    .trim();
}

export interface BuildTreeOptions {
  /** Generation number for native cache invalidation; should monotonically increase. */
  generation: number;
  /** Focused node id, if any. */
  focusedId?: string | null;
}

export function buildAccessibilityTree(
  root: AccessibilityFiberLike,
  options: BuildTreeOptions,
): AccessibilityTreeSnapshot {
  const nodesById: Record<string, AccessibilityNodeSnapshot> = {};
  const childrenById: Record<string, string[]> = {};

  function syncSnapshot(snapshot: AccessibilityNodeSnapshot): void {
    nodesById[snapshot.id] = snapshot;
    childrenById[snapshot.id] = snapshot.children.map((child) => child.id);
    for (const child of snapshot.children) syncSnapshot(child);
  }

  function visit(
    node: AccessibilityFiberLike,
    parentId: string | null,
  ): AccessibilityNodeSnapshot | null {
    if (node.a11y.accessible === false) {
      // Skip self; reparent children to current parent.
      for (const c of node.children) {
        visit(c, parentId);
      }
      // Return null so the caller doesn't register this node.
      return null;
    }
    const role = defaultRoleFor(node.componentType, node.a11y);
    if (role === 'none' || node.a11y.accessibilityRole === 'none') {
      // Subtree fully hidden.
      return null;
    }
    const label = node.a11y.accessibilityLabel ?? collectFallbackLabel(node);
    const state: AccessibilityState = { ...(node.a11y.accessibilityState ?? {}) };
    const value = normalizeValue(node.a11y.accessibilityValue);
    const focusable =
      node.a11y.focusable ??
      (role === 'button' || role === 'textbox' || role === 'searchbox' || role === 'link');
    const snapshot: AccessibilityNodeSnapshot = {
      id: node.id,
      parentId,
      role,
      label,
      hint: node.a11y.accessibilityHint,
      value,
      state,
      rect: { ...node.rect },
      focusable,
      tabIndex: node.a11y.tabIndex,
      liveRegion: node.a11y.accessibilityLiveRegion ?? 'off',
      language: node.a11y.accessibilityLanguage,
      headingLevel: node.a11y.accessibilityHeadingLevel,
      children: [],
    };
    if (parentId !== null) {
      let existing = childrenById[parentId];
      if (existing === undefined) {
        existing = [];
        childrenById[parentId] = existing;
      }
      existing.push(node.id);
    } else {
      childrenById[node.id] = [];
    }
    if (!childrenById[node.id]) childrenById[node.id] = [];
    for (const c of node.children) {
      const r = visit(c, node.id);
      if (r) snapshot.children.push(r);
    }
    const virtualSource = getVirtualSources().get(node.id);
    const expanded = virtualSource ? expandVirtualDescendants(snapshot, virtualSource) : snapshot;
    syncSnapshot(expanded);
    return expanded;
  }

  visit(root, null);
  return {
    rootId: root.id,
    generation: options.generation,
    focusedId: options.focusedId ?? null,
    nodesById,
    childrenById,
  };
}
