import { CommandBuffer, type Renderer, type Window, type WindowEventName } from 'fxe';
import { memo, mount, Text, type Node as UiNode, View } from 'fxe-ui';
import {
  drainLayoutTrace,
  setLayoutTraceEnabled,
} from '../packages/fxe-ui/src/debug/layout_trace.ts';

import { assert, assertEqual, run, test } from './ts_harness.ts';

test('mount renders one frame and disposes listeners', () => {
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<string, number>();
  const frameCallbacks: Array<((window: Window) => void) | null> = [];
  let redrawRequests = 0;
  let redrawAcks = 0;
  const win = {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => {
      ++redrawRequests;
    },
    setFrameCallback: (cb: ((window: Window) => void) | null) => {
      frameCallbacks.push(cb);
    },
    takeRedrawRequest: () => {
      ++redrawAcks;
      return true;
    },
    on: (event: WindowEventName) => {
      listeners.set(event, (listeners.get(event) ?? 0) + 1);
      return () => listeners.set(event, (listeners.get(event) ?? 1) - 1);
    },
  } as unknown as Window;

  const dispose = mount(
    View({
      style: { width: 160, height: 100, backgroundColor: 0x101010ff },
      children: Text({ children: 'Mounted' }),
    }),
    win,
    { renderer },
  );
  assert(renderer.vertexCount() > 0, 'mount should render non-empty command buffer');
  assertEqual(frameCallbacks.length, 1);
  assert(frameCallbacks[0] !== null);
  assertEqual(redrawRequests, 0);
  assertEqual(redrawAcks, 1);
  dispose();
  for (const count of listeners.values())
    assertEqual(count, 0, 'dispose should remove every listener');
  assertEqual(frameCallbacks.length, 2);
  assertEqual(frameCallbacks[1], null);
});

test('mount settles initial layout before presenting memo children', () => {
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<string, number>();
  const frameCallback: { current: ((window: Window) => void) | null } = { current: null };
  let redrawPending = true;
  let redrawRequests = 0;
  let redrawAcks = 0;
  const win = {
    framebufferSize: () => [400, 400] as [number, number],
    requestRedraw: () => {
      ++redrawRequests;
      redrawPending = true;
    },
    setFrameCallback: (cb: ((window: Window) => void) | null) => {
      frameCallback.current = cb;
    },
    takeRedrawRequest: () => {
      ++redrawAcks;
      const pending = redrawPending;
      redrawPending = false;
      return pending;
    },
    on: (event: WindowEventName) => {
      listeners.set(event, (listeners.get(event) ?? 0) + 1);
      return () => listeners.set(event, (listeners.get(event) ?? 1) - 1);
    },
  } as unknown as Window;

  function block(props: { label: string }): UiNode {
    return View({
      style: { marginBottom: 8 },
      children: Text({
        style: { fontSize: 24, color: 0xffffffff },
        children: props.label,
      }),
    });
  }
  const Block = memo(block);
  const root = View({
    style: { width: 400, height: 400, padding: 16 },
    children: [Block({ label: 'A' }), Block({ label: 'B' }), Block({ label: 'C' })],
  });

  setLayoutTraceEnabled(true, { limit: 100 });
  try {
    const dispose = mount(root, win, { renderer });
    try {
      assertEqual(redrawAcks, 2);
      assertEqual(redrawRequests, 1);
      assert(!redrawPending, 'initial unresolved layout redraw should settle inside mount');
      assert(frameCallback.current !== null, 'lazy mount should install frame callback');
      const settledBlock = drainLayoutTrace().find(
        (entry) => entry.rect.height > 0 && entry.rect.y > 16 && entry.styleHeight === undefined,
      );
      assert(settledBlock !== undefined, 'startup frame should settle memo child layout');
    } finally {
      dispose();
    }
  } finally {
    setLayoutTraceEnabled(false);
    drainLayoutTrace();
  }
});

test('mount rebuilds memo caches captured during fixed-size unresolved layout', () => {
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<string, number>();
  const frameCallback: { current: ((window: Window) => void) | null } = { current: null };
  let redrawPending = true;
  const win = {
    framebufferSize: () => [320, 240] as [number, number],
    requestRedraw: () => {
      redrawPending = true;
    },
    setFrameCallback: (cb: ((window: Window) => void) | null) => {
      frameCallback.current = cb;
    },
    takeRedrawRequest: () => {
      const pending = redrawPending;
      redrawPending = false;
      return pending;
    },
    on: (event: WindowEventName) => {
      listeners.set(event, (listeners.get(event) ?? 0) + 1);
      return () => listeners.set(event, (listeners.get(event) ?? 1) - 1);
    },
  } as unknown as Window;

  const Deep = memo(function deep(): UiNode {
    return View({
      __traceTag: 'deep-child',
      style: { marginBottom: 4 },
      children: Text({
        style: { fontSize: 22, color: 0xffffffff },
        children: 'settled',
      }),
    });
  });
  const Wrapper = memo(function wrapper(): UiNode {
    return View({ children: Deep({}) });
  });
  const Block = memo(function block(props: { label: string }): UiNode {
    return View({
      __traceTag: `block-${props.label}`,
      style: { height: 56, marginBottom: 8 },
      children: Wrapper({}),
    });
  });
  const root = View({
    style: { width: 320, height: 240, padding: 16 },
    children: [Block({ label: 'a' }), Block({ label: 'b' })],
  });

  setLayoutTraceEnabled(true, { limit: 200 });
  try {
    const dispose = mount(root, win, { renderer });
    try {
      const entries = drainLayoutTrace();
      assert(frameCallback.current !== null, 'lazy mount should install frame callback');
      assert(!redrawPending, 'unresolved fixed-size memo layout should settle inside mount');
      const deepChildren = entries.filter(
        (entry) => entry.tag === 'deep-child' && entry.rect.height > 0,
      );
      assertEqual(deepChildren.length, 2, 'expected both deep memo children to settle');
    } finally {
      dispose();
    }
  } finally {
    setLayoutTraceEnabled(false);
    drainLayoutTrace();
  }
});

await run();
