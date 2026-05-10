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
  useInternalLayout,
  useReducer,
  useRef,
  useState,
  View,
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

function layoutResult(width: number, height: number) {
  return {
    x: 0,
    y: 0,
    width,
    height,
    paddingLeft: 0,
    paddingTop: 0,
    paddingRight: 0,
    paddingBottom: 0,
    children: [],
  };
}

function withInternalLayout(node: Node, width: number, height: number): Node {
  if (node.type !== 'component') throw new Error('expected component node');
  return { ...node, internalLayout: layoutResult(width, height) } as Node;
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

test('memo re-renders when parent internal layout changes', () => {
  let renders = 0;
  const Probe = memo(
    Component(() => {
      ++renders;
      const layout = useInternalLayout();
      return Draw((cb: CommandBuffer) => point(cb, layout?.width ?? 1));
    }, 'InternalLayoutMemoProbe'),
  );
  const stableProps = { key: 'probe' };

  const first = new CommandBuffer();
  render(root('internal-layout-root', [withInternalLayout(Probe(stableProps), 1, 1)]), first);
  assertEqual(renders, 1);
  assertEqual(first.vertexCount(), 1);

  const second = new CommandBuffer();
  render(root('internal-layout-root', [withInternalLayout(Probe(stableProps), 3, 1)]), second);
  assertEqual(renders, 2);
  assertEqual(second.vertexCount(), 3);
});

test('single-child Layer forwards parent layout to wrapped component', () => {
  let renders = 0;
  const Probe = Component(() => {
    ++renders;
    const layout = useInternalLayout();
    return Draw((cb: CommandBuffer) => point(cb, layout?.width ?? 1));
  }, 'LayerLayoutForwardProbe');

  const cb = new CommandBuffer();
  render(
    root('layer-layout-root', [
      View({
        key: 'host',
        style: { width: 3, height: 1 },
        children: Layer({ key: 'wrapped', children: [Probe({ key: 'probe' })] }),
      }),
    ]),
    cb,
  );

  assertEqual(renders, 1);
  assertEqual(cb.vertexCount(), 3);
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

test('context-dirtied components redraw stable descendant layers', () => {
  const CountContext = createContext(1);
  const seen: number[] = [];
  const Consumer = memo(
    Component(() => {
      const count = useContext(CountContext);
      seen.push(count);
      return Layer({
        key: 'stable-layer',
        deps: ['stable'],
        children: [Draw((cb: CommandBuffer) => point(cb, count))],
      });
    }, 'LayeredContextConsumer'),
  );

  const tree = (count: number): Node =>
    root('context-layer-root', [
      CountContext.Provider({
        key: 'provider',
        value: count,
        children: Consumer({ key: 'consumer' }),
      }),
    ]);

  const first = new CommandBuffer();
  render(tree(1), first);
  assertEqual(first.vertexCount(), 1);

  const second = new CommandBuffer();
  render(tree(1), second);
  assertEqual(second.vertexCount(), 1);

  const third = new CommandBuffer();
  render(tree(3), third);
  assertEqual(third.vertexCount(), 3);
  assertEqual(seen.join(','), '1,3');
});

test('mounting a sibling provider does not dirty existing memo consumers', () => {
  const TextContext = createContext('default');
  const renders: Record<string, number> = { a: 0, b: 0 };
  const Consumer = memo(
    Component((props: { id: 'a' | 'b' }) => {
      renders[props.id] += 1;
      useContext(TextContext);
      return Draw((cb: CommandBuffer) => point(cb));
    }, 'ScopedContextConsumer'),
  );

  const providerA = (): Node =>
    TextContext.Provider({
      key: 'provider-a',
      value: 'a',
      children: Consumer({ key: 'consumer-a', id: 'a' }),
    });
  const providerB = (): Node =>
    TextContext.Provider({
      key: 'provider-b',
      value: 'b',
      children: Consumer({ key: 'consumer-b', id: 'b' }),
    });

  render(root('provider-scope-root', [providerA()]), new CommandBuffer());
  assertEqual(renders.a, 1);
  assertEqual(renders.b, 0);

  render(root('provider-scope-root', [providerA(), providerB()]), new CommandBuffer());
  assertEqual(renders.a, 1);
  assertEqual(renders.b, 1);

  render(root('provider-scope-root', [providerA(), providerB()]), new CommandBuffer());
  assertEqual(renders.a, 1);
  assertEqual(renders.b, 1);
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
  const previous = globalThis.__FXE_DEV;
  globalThis.__FXE_DEV = true;
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
    globalThis.__FXE_DEV = previous;
  }
});

await run();
