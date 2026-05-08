import {
  flushSync,
  getCurrentSchedulerLane,
  type SchedulerLane,
  scheduleCallback,
  scheduleWork,
} from './scheduler.ts';

type SignalSetter<T> = (next: T | ((prev: T) => T)) => void;
type FiberSubscriber = number;
type Subscriber = FiberSubscriber | ReactiveComputation<unknown>;

interface ReactiveSource {
  subscribers: Set<Subscriber>;
}

interface SignalCell<T> extends ReactiveSource {
  value: T;
}

type ReactiveComputationKind = 'effect' | 'memo';

interface ReactiveComputation<T> extends ReactiveSource {
  kind: ReactiveComputationKind;
  fn: () => T;
  deps: Set<ReactiveSource>;
  value: T | undefined;
  dirty: boolean;
  queued: boolean;
  running: boolean;
}

type TrackingContext = {
  subscriber: Subscriber | null;
  enabled: boolean;
};

const fiberDeps = new Map<FiberSubscriber, Set<ReactiveSource>>();
const pendingSubscribers = new Set<Subscriber>();
let currentTracking: TrackingContext = { subscriber: null, enabled: true };
let batchDepth = 0;

function depsForSubscriber(subscriber: Subscriber): Set<ReactiveSource> {
  if (typeof subscriber !== 'number') return subscriber.deps;
  let deps = fiberDeps.get(subscriber);
  if (!deps) {
    deps = new Set();
    fiberDeps.set(subscriber, deps);
  }
  return deps;
}

function cleanupSubscriber(subscriber: Subscriber): void {
  const deps = depsForSubscriber(subscriber);
  for (const source of deps) source.subscribers.delete(subscriber);
  deps.clear();
}

function trackSource(source: ReactiveSource): void {
  if (!currentTracking.enabled || currentTracking.subscriber === null) return;
  const subscriber = currentTracking.subscriber;
  source.subscribers.add(subscriber);
  depsForSubscriber(subscriber).add(source);
}

function withTracking<T>(subscriber: Subscriber, fn: () => T): T {
  cleanupSubscriber(subscriber);
  const prev = currentTracking;
  currentTracking = { subscriber, enabled: true };
  try {
    return fn();
  } finally {
    currentTracking = prev;
  }
}

function runComputation<T>(computation: ReactiveComputation<T>): void {
  if (computation.running) return;
  computation.queued = false;
  computation.running = true;
  try {
    computation.value = withTracking(computation, computation.fn);
    computation.dirty = false;
  } finally {
    computation.running = false;
  }
}

function scheduleSubscriber(subscriber: Subscriber, lane: SchedulerLane): void {
  if (typeof subscriber === 'number') {
    scheduleWork(subscriber, lane);
    return;
  }

  if (subscriber.kind === 'memo') {
    if (subscriber.dirty) return;
    subscriber.dirty = true;
    notifySource(subscriber, lane);
    return;
  }

  if (subscriber.queued) return;
  subscriber.queued = true;
  scheduleCallback(() => runComputation(subscriber), lane);
}

function notifySource(source: ReactiveSource, lane: SchedulerLane): void {
  for (const subscriber of [...source.subscribers]) {
    if (batchDepth > 0) {
      pendingSubscribers.add(subscriber);
    } else {
      scheduleSubscriber(subscriber, lane);
    }
  }
}

function flushPendingSubscribers(lane: SchedulerLane): void {
  const pending = [...pendingSubscribers];
  pendingSubscribers.clear();
  for (const subscriber of pending) scheduleSubscriber(subscriber, lane);
}

export function createSignal<T>(initial: T): [() => T, SignalSetter<T>] {
  const cell: SignalCell<T> = { value: initial, subscribers: new Set() };
  const getter = (): T => {
    trackSource(cell);
    return cell.value;
  };
  const setter: SignalSetter<T> = (next) => {
    const prev = cell.value;
    const resolved = typeof next === 'function' ? (next as (prev: T) => T)(prev) : next;
    if (Object.is(prev, resolved)) return;
    cell.value = resolved;
    const lane = getCurrentSchedulerLane();
    notifySource(cell, lane);
    if (lane === 'sync' && batchDepth === 0) flushSync();
  };
  return [getter, setter];
}

export function createMemo<T>(fn: () => T): () => T {
  const computation: ReactiveComputation<T> = {
    kind: 'memo',
    fn,
    deps: new Set(),
    subscribers: new Set(),
    value: undefined,
    dirty: true,
    queued: false,
    running: false,
  };
  const getter = (): T => {
    trackSource(computation);
    if (computation.dirty) runComputation(computation);
    return computation.value as T;
  };
  return getter;
}

export function createEffect(fn: () => void): void {
  const computation: ReactiveComputation<void> = {
    kind: 'effect',
    fn,
    deps: new Set(),
    subscribers: new Set(),
    value: undefined,
    dirty: true,
    queued: false,
    running: false,
  };
  runComputation(computation);
}

export function untrack<T>(fn: () => T): T {
  const prev = currentTracking;
  currentTracking = { subscriber: prev.subscriber, enabled: false };
  try {
    return fn();
  } finally {
    currentTracking = prev;
  }
}

export function batch<T>(fn: () => T): T {
  ++batchDepth;
  try {
    return fn();
  } finally {
    --batchDepth;
    if (batchDepth === 0) {
      const lane = getCurrentSchedulerLane();
      flushPendingSubscribers(lane);
      if (lane === 'sync') flushSync();
    }
  }
}

export function beginFiberSignalTracking(fiberId: number): TrackingContext {
  const prev = currentTracking;
  cleanupSubscriber(fiberId);
  currentTracking = { subscriber: fiberId, enabled: true };
  return prev;
}

export function endFiberSignalTracking(prev: TrackingContext): void {
  currentTracking = prev;
}

export function unregisterFiberSignalSubscriptions(fiberId: number): void {
  cleanupSubscriber(fiberId);
  fiberDeps.delete(fiberId);
}
