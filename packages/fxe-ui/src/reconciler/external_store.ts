import { useEffect, useReducer, useRef } from './fiber.ts';

interface ExternalStoreState<T> {
  snapshot: T;
  getSnapshot: () => T;
}

function increment(value: number): number {
  return value + 1;
}

export function useSyncExternalStore<T>(
  subscribe: (cb: () => void) => () => void,
  getSnapshot: () => T,
): T {
  const [, forceRender] = useReducer(increment, 0);
  const snapshot = getSnapshot();
  const stateRef = useRef<ExternalStoreState<T> | null>(null);

  if (stateRef.current === null) {
    stateRef.current = { snapshot, getSnapshot };
  } else {
    stateRef.current.getSnapshot = getSnapshot;
    if (!Object.is(stateRef.current.snapshot, snapshot)) {
      stateRef.current.snapshot = snapshot;
    }
  }

  useEffect(() => {
    const state = stateRef.current;
    if (state === null) return;

    let disposed = false;
    const rerenderIfSnapshotChanged = (): void => {
      if (disposed) return;
      const nextSnapshot = state.getSnapshot();
      if (Object.is(state.snapshot, nextSnapshot)) return;
      state.snapshot = nextSnapshot;
      forceRender(increment);
    };
    const handleStoreChange = (): void => {
      if (disposed) return;
      try {
        const nextSnapshot = state.getSnapshot();
        if (!Object.is(state.snapshot, nextSnapshot)) {
          state.snapshot = nextSnapshot;
        }
      } finally {
        forceRender(increment);
      }
    };

    const unsubscribe = subscribe(handleStoreChange);
    rerenderIfSnapshotChanged();

    return () => {
      disposed = true;
      unsubscribe();
    };
  }, [subscribe]);

  return snapshot;
}
