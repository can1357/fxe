export type SchedulerLane = 'sync' | 'transition';

export type FiberWorkHandler = (fiberId: number, lane: SchedulerLane) => void;
export type ScheduledCallback = () => void;

export const DEFAULT_SCHEDULER_FRAME_BUDGET_MS = 8;

const fiberHandlers = new Map<number, FiberWorkHandler>();
const syncFibers = new Set<number>();
const transitionFibers = new Set<number>();
const syncCallbacks: ScheduledCallback[] = [];
const transitionCallbacks: ScheduledCallback[] = [];

let syncFlushQueued = false;
let currentLane: SchedulerLane = 'sync';
let activeFlushLane: SchedulerLane | null = null;
let transitionIdleCallbacks: ScheduledCallback[] = [];

function nowMs(): number {
  return performance.now();
}

function queueSyncFlush(): void {
  if (syncFlushQueued) return;
  syncFlushQueued = true;
  queueMicrotask(flushSync);
}

function runCallback(callback: ScheduledCallback, lane: SchedulerLane): void {
  const prevLane = currentLane;
  const prevFlushLane = activeFlushLane;
  currentLane = lane;
  activeFlushLane = lane;
  try {
    callback();
  } finally {
    currentLane = prevLane;
    activeFlushLane = prevFlushLane;
  }
}

function notifyTransitionIdleIfNeeded(): void {
  if (transitionCallbacks.length > 0 || transitionFibers.size > 0) return;
  const callbacks = transitionIdleCallbacks;
  transitionIdleCallbacks = [];
  for (const callback of callbacks) scheduleCallback(callback, 'sync');
}

export function registerFiberWork(fiberId: number, handler: FiberWorkHandler): void {
  fiberHandlers.set(fiberId, handler);
}

export function unregisterFiberWork(fiberId: number): void {
  fiberHandlers.delete(fiberId);
  syncFibers.delete(fiberId);
  transitionFibers.delete(fiberId);
}

export function scheduleWork(fiberId: number, lane: SchedulerLane = 'sync'): void {
  if (lane === 'sync') {
    syncFibers.add(fiberId);
    queueSyncFlush();
  } else {
    transitionFibers.add(fiberId);
  }
}

export function scheduleCallback(callback: ScheduledCallback, lane: SchedulerLane = 'sync'): void {
  if (lane === 'sync') {
    syncCallbacks.push(callback);
    queueSyncFlush();
  } else {
    transitionCallbacks.push(callback);
  }
}

export function flushSync(): void {
  syncFlushQueued = false;
  while (syncCallbacks.length > 0 || syncFibers.size > 0) {
    while (syncCallbacks.length > 0) {
      const callback = syncCallbacks.shift();
      if (callback) runCallback(callback, 'sync');
    }
    const fibers = [...syncFibers];
    syncFibers.clear();
    for (const fiberId of fibers) {
      const handler = fiberHandlers.get(fiberId);
      if (handler) handler(fiberId, 'sync');
    }
  }
}

export function tickSchedulerFrame(
  _dtMs: number,
  budgetMs: number = DEFAULT_SCHEDULER_FRAME_BUDGET_MS,
): void {
  const budget = Math.max(0, budgetMs);
  const start = nowMs();
  let processed = false;

  while (transitionCallbacks.length > 0) {
    const callback = transitionCallbacks.shift();
    if (callback) runCallback(callback, 'transition');
    processed = true;
    if (nowMs() - start >= budget) {
      notifyTransitionIdleIfNeeded();
      return;
    }
  }

  while (transitionFibers.size > 0) {
    const fiberId = transitionFibers.values().next().value as number | undefined;
    if (fiberId === undefined) break;
    transitionFibers.delete(fiberId);
    const handler = fiberHandlers.get(fiberId);
    if (handler) handler(fiberId, 'transition');
    processed = true;
    if (nowMs() - start >= budget) break;
  }

  if (processed) notifyTransitionIdleIfNeeded();
}

export function schedulerFrameBudgetMs(): number {
  return DEFAULT_SCHEDULER_FRAME_BUDGET_MS;
}

export function getCurrentSchedulerLane(): SchedulerLane {
  return currentLane;
}

export function isTransitionFlushActive(): boolean {
  return activeFlushLane === 'transition';
}

export function runWithSchedulerLane<T>(lane: SchedulerLane, fn: () => T): T {
  const prev = currentLane;
  currentLane = lane;
  try {
    return fn();
  } finally {
    currentLane = prev;
  }
}

type HookApi = {
  useState<S>(initial: S): [S, (next: S | ((prev: S) => S)) => void];
  useRef<T>(initial: T): { current: T };
};

let hookApi: HookApi | null = null;

export function installSchedulerHookApi(api: HookApi): void {
  hookApi = api;
}

function requireHookApi(name: string): HookApi {
  if (!hookApi) throw new Error(`${name}() called before scheduler hook API installation`);
  return hookApi;
}

export function useTransition(): [boolean, (fn: () => void) => void] {
  const hooks = requireHookApi('useTransition');
  const [isPending, setPending] = hooks.useState(false);
  const pendingRef = hooks.useRef(0);
  const startTransition = (fn: () => void): void => {
    pendingRef.current += 1;
    setPending(true);
    try {
      runWithSchedulerLane('transition', fn);
    } finally {
      transitionIdleCallbacks.push(() => {
        pendingRef.current = Math.max(0, pendingRef.current - 1);
        if (pendingRef.current === 0) setPending(false);
      });
      notifyTransitionIdleIfNeeded();
    }
  };
  return [isPending, startTransition];
}

export function useDeferredValue<T>(value: T): T {
  const hooks = requireHookApi('useDeferredValue');
  const [deferred, setDeferred] = hooks.useState(value);
  const state = hooks.useRef({ latest: value, queued: false });
  if (!Object.is(state.current.latest, value)) {
    state.current.latest = value;
    if (!state.current.queued) {
      state.current.queued = true;
      scheduleCallback(() => {
        state.current.queued = false;
        setDeferred(state.current.latest);
      }, 'transition');
    }
  }
  return deferred;
}
