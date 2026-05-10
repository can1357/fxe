import type { AccessibilityNodeSnapshot } from './types.ts';

export interface VirtualDescendantSource {
  /** Stable id for the parent fiber whose children include the virtual rows. */
  parentId: string;
  /** Total number of items, including offscreen. */
  totalCount: number;
  /** Rendered window: [first, lastInclusive]. */
  renderedRange: [number, number];
  /** Build an AccessibilityNodeSnapshot for an offscreen row index. */
  buildVirtualNode(index: number): AccessibilityNodeSnapshot | null;
}

const virtualSources = new Map<string, VirtualDescendantSource>();

export function registerVirtualSource(source: VirtualDescendantSource): () => void {
  virtualSources.set(source.parentId, source);
  return () => {
    if (virtualSources.get(source.parentId) === source) {
      virtualSources.delete(source.parentId);
    }
  };
}

export function getVirtualSources(): ReadonlyMap<string, VirtualDescendantSource> {
  return virtualSources;
}

export function expandVirtualDescendants(
  node: AccessibilityNodeSnapshot,
  source: VirtualDescendantSource,
): AccessibilityNodeSnapshot {
  const totalCount = Math.max(0, Math.floor(source.totalCount));
  if (totalCount === 0) {
    return { ...node, children: [] };
  }

  const firstRendered = Math.max(0, Math.floor(source.renderedRange[0]));
  const lastRendered = Math.min(totalCount - 1, Math.floor(source.renderedRange[1]));
  if (firstRendered === 0 && lastRendered === totalCount - 1) {
    return node;
  }

  const children: AccessibilityNodeSnapshot[] = [];
  let renderedChildIndex = 0;
  for (let index = 0; index < totalCount; ++index) {
    if (index >= firstRendered && index <= lastRendered) {
      const rendered = node.children[renderedChildIndex];
      if (rendered) children.push(rendered);
      renderedChildIndex += 1;
      continue;
    }

    const virtualChild = source.buildVirtualNode(index);
    if (!virtualChild) continue;
    children.push({
      ...virtualChild,
      parentId: node.id,
      state: { ...virtualChild.state, offscreen: true },
      children: [...virtualChild.children],
    });
  }

  return { ...node, children };
}
