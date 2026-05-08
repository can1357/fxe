// FXE resolves this package name through the host loader rather than node_modules.
import { App, CommandBuffer, Primitives } from 'fxe';
import {
  Component,
  createEffect,
  createSignal,
  Draw,
  ErrorBoundary,
  flushSync,
  Layer,
  render,
  Suspense,
  setPaintFlash,
  setRenderTarget,
  snapshotFiberTree,
  startFrameLoop,
  tickFrame,
  useFrame,
  useState,
  useTransition,
} from 'fxe-ui';
import { jsx } from 'fxe-ui/jsx-runtime';

import { assert, assertEqual, run, test } from './ts_harness.ts';

function renderFrameProbe(onFrame: (dtMs: number) => void): void {
  const Probe = Component(() => {
    useFrame(onFrame);
    return Draw(() => undefined);
  }, 'FrameProbe');

  render(Layer({ children: [Probe({})] }), new CommandBuffer());
}

test('tickFrame invokes registered useFrame callbacks with dt', () => {
  let calls = 0;
  let totalDtMs = 0;
  renderFrameProbe((dtMs) => {
    ++calls;
    totalDtMs += dtMs;
  });

  tickFrame(12);
  tickFrame(8);

  assertEqual(calls, 2);
  assertEqual(totalDtMs, 20);
});

test('startFrameLoop advances callbacks through a controlled scheduler and disposes', () => {
  let calls = 0;
  let totalDtMs = 0;
  renderFrameProbe((dtMs) => {
    ++calls;
    totalDtMs += dtMs;
  });

  const scheduled: Array<(timeMs: number) => void> = [];
  const canceled: unknown[] = [];
  const dispose = startFrameLoop({
    requestAnimationFrame: (fn: (timeMs: number) => void) => {
      scheduled.push(fn);
      return scheduled.length;
    },
    cancelAnimationFrame: (id: unknown) => {
      canceled.push(id);
    },
  });

  assertEqual(scheduled.length, 1);
  scheduled.shift()?.(100);
  assertEqual(calls, 1);
  assertEqual(totalDtMs, 0);
  assertEqual(scheduled.length, 1);

  scheduled.shift()?.(116);
  assertEqual(calls, 2);
  assertEqual(totalDtMs, 16);
  assertEqual(scheduled.length, 1);

  dispose();
  assertEqual(canceled.length, 1);
  scheduled.shift()?.(132);
  assertEqual(calls, 2);
  assertEqual(totalDtMs, 16);
});

test('ErrorBoundary catches descendant render errors and renders fallback', () => {
  let captured: unknown = null;
  let fallbackDraws = 0;
  const Bomb = Component(() => {
    throw new Error('boom');
  }, 'Bomb');

  render(
    ErrorBoundary({
      children: Bomb({}),
      fallback: Draw(() => {
        ++fallbackDraws;
      }),
      onError: (error: unknown) => {
        captured = error;
      },
    }),
    new CommandBuffer(),
  );

  assertEqual(fallbackDraws, 1);
  assertEqual(captured instanceof Error ? captured.message : String(captured), 'boom');
});

test('Suspense catches thrown promises, renders fallback, and requests redraw on settle', async () => {
  let fallbackDraws = 0;
  let resolved = false;
  let resolve!: () => void;
  const promise = new Promise<void>((done) => {
    resolve = done;
  });
  const Pending = Component(() => {
    if (!resolved) throw promise;
    return Draw(() => undefined);
  }, 'Pending');
  let redraws = 0;
  setRenderTarget({
    requestRedraw: () => {
      ++redraws;
    },
  } as never);

  try {
    render(
      Suspense({
        children: Pending({}),
        fallback: Draw(() => {
          ++fallbackDraws;
        }),
      }),
      new CommandBuffer(),
    );
    assertEqual(fallbackDraws, 1);
    assertEqual(redraws, 0);

    resolved = true;
    resolve();
    await promise;
    await Promise.resolve();
    assertEqual(redraws, 1);
  } finally {
    setRenderTarget(null);
  }
});

test('JSX class components render through their render method', () => {
  let draws = 0;
  class ClassView {
    constructor(public props: { value: number }) {}

    render() {
      return Draw(() => {
        draws += this.props.value;
      });
    }
  }

  render(jsx(ClassView, { value: 3 }), new CommandBuffer());

  assertEqual(draws, 3);
});

test('App.run({ animate: true }) starts the shared fxe-ui frame bridge', () => {
  let calls = 0;
  renderFrameProbe(() => {
    ++calls;
  });

  const globals = globalThis as typeof globalThis & {
    requestAnimationFrame: (fn: (timeMs: number) => void) => number;
    cancelAnimationFrame?: (id: unknown) => void;
    __fxeUiEnsureFrameLoop?: () => () => void;
  };
  const originalRequestAnimationFrame = globals.requestAnimationFrame;
  const originalCancelAnimationFrame = globals.cancelAnimationFrame;
  const scheduled: Array<(timeMs: number) => void> = [];
  const canceled: unknown[] = [];
  globals.requestAnimationFrame = (fn: (timeMs: number) => void): number => {
    scheduled.push(fn);
    return scheduled.length;
  };
  globals.cancelAnimationFrame = (id: unknown): void => {
    canceled.push(id);
  };

  try {
    App.run({ animate: true });
    assertEqual(scheduled.length, 1);
    scheduled.shift()?.(100);
    assertEqual(calls, 1);
    globals.__fxeUiEnsureFrameLoop?.()();
  } finally {
    globals.requestAnimationFrame = originalRequestAnimationFrame;
    globals.cancelAnimationFrame = originalCancelAnimationFrame;
  }
});

test('signals rerun dependent effects through the scheduler', () => {
  const observed: number[] = [];
  const [a, setA] = createSignal(1);
  const [b] = createSignal(2);
  createEffect(() => {
    observed.push(a() + b());
  });

  setA(10);
  flushSync();

  assertEqual(observed.length, 2);
  assertEqual(observed[0], 3);
  assertEqual(observed[1], 12);
});

test('useTransition defers state visibility until the scheduler frame', () => {
  let start: ((fn: () => void) => void) | null = null;
  let setValue: ((next: number) => void) | null = null;
  let renderedValue = -1;
  let renderedPending = false;
  const Probe = Component(() => {
    const [isPending, startTransition] = useTransition();
    const [value, updateValue] = useState(0);
    start = startTransition;
    setValue = updateValue;
    renderedValue = value;
    renderedPending = isPending;
    return Draw(() => undefined);
  }, 'TransitionProbe');

  render(Layer({ children: [Probe({})] }), new CommandBuffer());
  const beginTransition = start as ((fn: () => void) => void) | null;
  const update = setValue as ((next: number) => void) | null;
  assert(beginTransition, 'transition starter should be captured');
  assert(update, 'transition setter should be captured');
  beginTransition(() => update(7));
  render(Layer({ children: [Probe({})] }), new CommandBuffer());
  assertEqual(renderedPending, true);
  assertEqual(renderedValue, 0);

  tickFrame(16);
  render(Layer({ children: [Probe({})] }), new CommandBuffer());
  assertEqual(renderedValue, 7);
});

test('snapshotFiberTree exposes rendered fibers for devtools', () => {
  const Probe = Component(() => Draw(() => undefined), 'SnapshotProbe');
  render(Layer({ children: [Probe({})] }), new CommandBuffer());
  const snapshot = snapshotFiberTree();
  assert(snapshot.tree.length > 0, 'snapshot should include the rendered root child');
  assert(snapshot.tree[0].children.length > 0, 'snapshot should preserve child fibers');
  assertEqual(snapshot.tree[0].dirty, false);
});

test('paint flash adds a one-frame outline for rebuilt layers', () => {
  const layerProps = {
    deps: [1],
    children: [
      Draw((cb) => {
        Primitives.fillRect(cb, 0, 0, 10, 10, 0, 0xffffffff);
      }),
    ],
  };
  const node = Layer(layerProps);

  setPaintFlash(false);
  const normal = new CommandBuffer();
  render(node, normal);
  const normalVertices = normal.vertexBuffer().length;

  setPaintFlash(true);
  try {
    const flashed = new CommandBuffer();
    render(Layer({ ...layerProps, deps: [2] }), flashed);
    assert(
      flashed.vertexBuffer().length > normalVertices,
      'paint flash should append outline geometry',
    );
  } finally {
    setPaintFlash(false);
  }
});

await run();
