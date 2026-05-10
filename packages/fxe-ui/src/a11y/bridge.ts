import type { AccessibilityTreeSnapshot } from './types.ts';

export interface A11yBridge {
  publish(snapshot: AccessibilityTreeSnapshot): void;
  latest(): AccessibilityTreeSnapshot | null;
  clear(): void;
  subscribe(cb: (s: AccessibilityTreeSnapshot) => void): () => void;
}

type BridgeState = {
  snapshot: AccessibilityTreeSnapshot | null;
  revision: number;
  subscribers: Set<(snapshot: AccessibilityTreeSnapshot) => void>;
};

let bridge: A11yBridge | null = null;

function state(): BridgeState {
  if (globalThis.__fxeA11y === undefined) {
    globalThis.__fxeA11y = {
      snapshot: null,
      revision: 0,
      subscribers: new Set(),
    };
  }
  return globalThis.__fxeA11y;
}

export function getA11yBridge(): A11yBridge {
  if (bridge) return bridge;
  bridge = {
    publish(snapshot) {
      const current = state();
      current.snapshot = snapshot;
      current.revision += 1;
      const subscribers = [...current.subscribers];
      for (const subscriber of subscribers) subscriber(snapshot);
    },
    latest() {
      return state().snapshot;
    },
    clear() {
      state().snapshot = null;
    },
    subscribe(cb) {
      const current = state();
      current.subscribers.add(cb);
      return () => {
        current.subscribers.delete(cb);
      };
    },
  };
  return bridge;
}

export function publishAccessibilityTree(snapshot: AccessibilityTreeSnapshot): void {
  getA11yBridge().publish(snapshot);
}
