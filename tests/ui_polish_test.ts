// FXE resolves these package names through the host loader rather than node_modules.
import { CommandBuffer } from 'fxe';
import {
  Component,
  createContext,
  Draw,
  Layer,
  memo,
  type Node,
  Portal,
  render,
  setRenderTarget,
  useContext,
  useId,
  useReducer,
  useRef,
  useState,
} from 'fxe-ui';

import { assert, assertEqual, assertThrows, run, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;

type Setter<T> = (next: T | ((prev: T) => T)) => void;
type Dispatch<T> = (action: T) => void;
type RefBox<T> = { current: T };

function requireSetter<T>(setter: Setter<T> | null): Setter<T> {
  assert(setter !== null, 'expected state setter to be captured');
  return setter;
}

function requireDispatch<T>(dispatch: Dispatch<T> | null): Dispatch<T> {
  assert(dispatch !== null, 'expected reducer dispatch to be captured');
  return dispatch;
}

function point(cb: CommandBuffer, count = 1): void {
  const allocation = cb.allocate(count, count, TRIANGLE);
  for (let i = 0; i < count; ++i) allocation.idxs[i] = i;
}

function root(key: string, children: readonly Node[]): Node {
  return Layer({ key, children });
}

test('memo skips stable props and re-renders after state updates', () => {
  let renders = 0;
  let setCount: Setter<number> | null = null;
  const Probe = memo(
    Component((props: { label: string }) => {
      ++renders;
      const [count, setter] = useState(1);
      setCount = setter;
      return Draw((cb: CommandBuffer) =>
        point(cb, count + props.label.length - props.label.length),
      );
    }, 'MemoProbe'),
  );
  const stableProps = { key: 'probe', label: 'same' };

  const first = new CommandBuffer();
  render(root('memo-root', [Probe(stableProps)]), first);
  assertEqual(renders, 1);
  assertEqual(first.vertexCount(), 1);

  const second = new CommandBuffer();
  render(root('memo-root', [Probe(stableProps)]), second);
  assertEqual(renders, 1);
  assertEqual(second.vertexCount(), 1);

  requireSetter(setCount)(2);
  const third = new CommandBuffer();
  render(root('memo-root', [Probe(stableProps)]), third);
  assertEqual(renders, 2);
  assertEqual(third.vertexCount(), 2);
});

test('useReducer dispatch updates state and requests redraw', () => {
  let dispatch: Dispatch<number> | null = null;
  let redraws = 0;
  const Counter = Component(() => {
    const [count, send] = useReducer((state: number, action: number) => state + action, 1);
    dispatch = send;
    return Draw((cb: CommandBuffer) => point(cb, count));
  }, 'ReducerCounter');

  setRenderTarget({ requestRedraw: () => ++redraws } as never);
  try {
    const first = new CommandBuffer();
    render(root('reducer-root', [Counter({ key: 'counter' })]), first);
    assertEqual(first.vertexCount(), 1);
    assertEqual(redraws, 0);

    requireDispatch(dispatch)(2);
    assertEqual(redraws, 1);

    const second = new CommandBuffer();
    render(root('reducer-root', [Counter({ key: 'counter' })]), second);
    assertEqual(second.vertexCount(), 3);
  } finally {
    setRenderTarget(null);
  }
});

test('createContext and useContext propagate changed values through memo boundaries', () => {
  const TextContext = createContext('default');
  const seen: string[] = [];
  const Consumer = memo(
    Component(() => {
      const value = useContext(TextContext);
      seen.push(value);
      return Draw((cb: CommandBuffer) => point(cb));
    }, 'ContextConsumer'),
  );

  render(
    root('context-root', [
      TextContext.Provider({
        key: 'provider',
        value: 'a',
        children: Consumer({ key: 'consumer' }),
      }),
    ]),
    new CommandBuffer(),
  );
  render(
    root('context-root', [
      TextContext.Provider({
        key: 'provider',
        value: 'a',
        children: Consumer({ key: 'consumer' }),
      }),
    ]),
    new CommandBuffer(),
  );
  render(
    root('context-root', [
      TextContext.Provider({
        key: 'provider',
        value: 'b',
        children: Consumer({ key: 'consumer' }),
      }),
    ]),
    new CommandBuffer(),
  );

  assertEqual(seen.join(','), 'a,b');
});

test('useRef preserves object identity across renders', () => {
  let firstRef: RefBox<{ tag: string }> | null = null;
  let secondRef: RefBox<{ tag: string }> | null = null;
  let setTick: Setter<number> | null = null;
  let renders = 0;
  const Probe = Component(() => {
    const ref = useRef({ tag: 'stable' });
    const [, setter] = useState(0);
    setTick = setter;
    ++renders;
    if (renders === 1) firstRef = ref;
    else secondRef = ref;
    return Draw((cb: CommandBuffer) => point(cb));
  }, 'RefProbe');

  render(root('ref-root', [Probe({ key: 'probe' })]), new CommandBuffer());
  assert(setTick !== null, 'expected state setter to be captured');
  requireSetter(setTick)(1);
  render(root('ref-root', [Probe({ key: 'probe' })]), new CommandBuffer());

  assert(firstRef !== null, 'first ref should be captured');
  assert(secondRef !== null, 'second ref should be captured');
  assert(firstRef === secondRef, 'useRef should return the same object on rerender');
});

test('useId is stable for the same fiber and distinct between fibers', () => {
  const firstPass: string[] = [];
  const secondPass: string[] = [];
  let pass = firstPass;
  const IdProbe = Component(() => {
    pass.push(useId());
    return Draw((cb: CommandBuffer) => point(cb));
  }, 'IdProbe');

  render(root('id-root', [IdProbe({ key: 'a' }), IdProbe({ key: 'b' })]), new CommandBuffer());
  pass = secondPass;
  render(root('id-root', [IdProbe({ key: 'a' }), IdProbe({ key: 'b' })]), new CommandBuffer());

  assertEqual(firstPass.length, 2);
  assertEqual(secondPass.length, 2);
  assert(firstPass[0] !== firstPass[1], 'sibling fibers should receive distinct ids');
  assertEqual(secondPass[0], firstPass[0]);
  assertEqual(secondPass[1], firstPass[1]);
});

test('Portal redirects draw output while preserving hook ownership', () => {
  let setCount: Setter<number> | null = null;
  const Owned = Component(() => {
    const [count, setter] = useState(1);
    setCount = setter;
    return Draw((cb: CommandBuffer) => point(cb, count));
  }, 'PortalOwned');

  const hostFirst = new CommandBuffer();
  const portalFirst = new CommandBuffer();
  render(
    root('portal-root', [
      Portal({ key: 'portal', to: portalFirst, children: Owned({ key: 'owned' }) }),
    ]),
    hostFirst,
  );
  assert(hostFirst.isEmpty(), 'portal children should not draw into the host target');
  assertEqual(portalFirst.vertexCount(), 1);

  requireSetter(setCount)(2);
  const hostSecond = new CommandBuffer();
  const portalSecond = new CommandBuffer();
  render(
    root('portal-root', [
      Portal({ key: 'portal', to: portalSecond, children: Owned({ key: 'owned' }) }),
    ]),
    hostSecond,
  );
  assert(hostSecond.isEmpty(), 'portal rerender should keep host target empty');
  assertEqual(portalSecond.vertexCount(), 2);
});

test('duplicate keys throw in dev mode', () => {
  const globals = globalThis as typeof globalThis & { __FXE_DEV?: boolean };
  const previous = globals.__FXE_DEV;
  globals.__FXE_DEV = true;
  try {
    assertThrows(() => {
      render(
        root('duplicate-root', [
          Layer({ key: 'dup', children: [] }),
          Layer({ key: 'dup', children: [] }),
        ]),
        new CommandBuffer(),
      );
    }, /fxe-ui: duplicate key "l:dup" at \$root\/l:duplicate-root/);
  } finally {
    globals.__FXE_DEV = previous;
  }
});

await run();
