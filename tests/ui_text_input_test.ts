import {
  CommandBuffer,
  type Renderer,
  type Window,
  type WindowEventMap,
  type WindowEventName,
} from 'fxe';
import {
  clearFocus,
  clearHitTargets,
  dispatchKeyDown,
  dispatchKeyPress,
  dispatchMouseDown,
  dispatchMouseMove,
  dispatchMouseUp,
  focusTarget,
  mount,
  registerHitTarget,
  Text,
  TextArea,
  TextInput,
  View,
} from 'fxe-ui';

import { assert, assertDeepEqual, run, test } from './ts_harness.ts';

test('text input key handling inserts, deletes, and submits', () => {
  clearHitTargets();
  let value = '';
  const changes: string[] = [];
  const submits: string[] = [];
  registerHitTarget({
    id: 'input',
    rect: {
      x: 0,
      y: 0,
      width: 100,
      height: 30,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onKeyPress: (ev) => {
      const codepoint = (ev as { codepoint: number }).codepoint;
      value += String.fromCodePoint(codepoint);
      changes.push(value);
    },
    onKeyDown: (ev) => {
      const key = (ev as { key: number }).key;
      if (key === 259) {
        value = value.slice(0, -1);
        changes.push(value);
      }
      if (key === 257) submits.push(value);
    },
  });
  focusTarget('input');
  dispatchKeyPress({ type: 'keypress', key: 0, scancode: 0, modifiers: 0, codepoint: 65 });
  dispatchKeyPress({ type: 'keypress', key: 0, scancode: 0, modifiers: 0, codepoint: 66 });
  dispatchKeyDown({ type: 'keydown', key: 259, scancode: 0, modifiers: 0 });
  dispatchKeyDown({ type: 'keydown', key: 257, scancode: 0, modifiers: 0 });
  assertDeepEqual(changes, ['A', 'AB', 'A']);
  assertDeepEqual(submits, ['A']);
});

test('TextInput mount routes compose and commit callbacks to focused target', () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<WindowEventName, (ev: WindowEventMap[WindowEventName]) => void>();
  const win = {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => undefined,
    on: <T extends WindowEventName>(event: T, cb: (ev: WindowEventMap[T]) => void) => {
      listeners.set(event, cb as (ev: WindowEventMap[WindowEventName]) => void);
      return () => listeners.delete(event);
    },
  } as unknown as Window;
  const composed: Array<[string, number]> = [];
  const committed: string[] = [];

  const dispose = mount(
    TextInput({
      key: 'ime-input',
      style: { width: 120, height: 32 },
      onCompose: (preedit, cursor) => composed.push([preedit, cursor]),
      onCommit: (value) => committed.push(value),
    }),
    win,
    { renderer },
  );
  try {
    dispatchMouseDown({ type: 'mousedown', x: 1, y: 1, button: 0, modifiers: 0 });
    const compose = listeners.get('compose');
    assert(typeof compose === 'function', 'mount should register a compose listener');
    compose({ type: 'compose', preedit: 'かな', cursor: 1, committed: '' });
    compose({ type: 'compose', preedit: '', cursor: 0, committed: '字' });
    assertDeepEqual(composed, [['かな', 1]]);
    assertDeepEqual(committed, ['字']);
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

test('TextInput selection shortcuts copy and paste through the mounted window clipboard', () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<WindowEventName, (ev: WindowEventMap[WindowEventName]) => void>();
  let clipboard = '';
  const win = {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => undefined,
    clipboardText: () => clipboard,
    setClipboardText: (text: string) => {
      clipboard = text;
    },
    on: <T extends WindowEventName>(event: T, cb: (ev: WindowEventMap[T]) => void) => {
      listeners.set(event, cb as (ev: WindowEventMap[WindowEventName]) => void);
      return () => listeners.delete(event);
    },
  } as unknown as Window;
  const changes: string[] = [];
  const dispose = mount(
    TextInput({
      key: 'selection-input',
      value: 'abc',
      style: { width: 120, height: 32 },
      onChange: (value) => changes.push(value),
    }),
    win,
    { renderer },
  );
  const rerender = () => listeners.get('resize')?.({ type: 'resize', width: 160, height: 100 });
  try {
    dispatchMouseDown({ type: 'mousedown', x: 1, y: 1, button: 0, modifiers: 0 });
    rerender();
    listeners.get('keydown')?.({ type: 'keydown', key: 262, scancode: 0, modifiers: 1 });
    rerender();
    listeners.get('keydown')?.({ type: 'keydown', key: 67, scancode: 0, modifiers: 8 });
    assertDeepEqual(clipboard, 'a');
    clipboard = 'Z';
    listeners.get('keydown')?.({ type: 'keydown', key: 86, scancode: 0, modifiers: 8 });
    assertDeepEqual(changes.at(-1), 'Zbc');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});
test('TextArea preempts focus advance when Tab inserts text', () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<WindowEventName, (ev: WindowEventMap[WindowEventName]) => void>();
  const win = {
    framebufferSize: () => [200, 140] as [number, number],
    requestRedraw: () => undefined,
    clipboardText: () => '',
    setClipboardText: () => undefined,
    on: <T extends WindowEventName>(event: T, cb: (ev: WindowEventMap[T]) => void) => {
      listeners.set(event, cb as (ev: WindowEventMap[WindowEventName]) => void);
      return () => listeners.delete(event);
    },
  } as unknown as Window;
  const areaChanges: string[] = [];
  const inputChanges: string[] = [];
  const dispose = mount(
    View({
      style: { width: 200, height: 140 },
      children: [
        TextArea({
          key: 'tab-area',
          value: 'x',
          style: { width: 180, height: 72 },
          onChange: (value) => areaChanges.push(value),
        }),
        TextInput({
          key: 'next-input',
          value: 'y',
          style: { width: 180, height: 32, marginTop: 80 },
          onChange: (value) => inputChanges.push(value),
        }),
      ],
    }),
    win,
    { renderer },
  );
  const rerender = () => listeners.get('resize')?.({ type: 'resize', width: 200, height: 140 });
  try {
    listeners.get('mousedown')?.({ type: 'mousedown', x: 1, y: 1, button: 0, modifiers: 0 });
    listeners.get('mouseup')?.({ type: 'mouseup', x: 1, y: 1, button: 0, modifiers: 0 });
    rerender();
    let target = focusTarget();
    assert(target?.componentType === 'TextArea', 'TextArea must be focused before Tab');
    listeners.get('keydown')?.({ type: 'keydown', key: 258, scancode: 0, modifiers: 0 });
    rerender();
    assertDeepEqual(areaChanges.at(-1), '\tx');
    assertDeepEqual(inputChanges.length, 0);
    target = focusTarget();
    assert(target?.componentType === 'TextArea', 'TextArea keeps focus after Tab insert');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

test('selectable Text copies the dragged read-only selection', () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<WindowEventName, (ev: WindowEventMap[WindowEventName]) => void>();
  let clipboard = '';
  const win = {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => undefined,
    setClipboardText: (text: string) => {
      clipboard = text;
    },
    on: <T extends WindowEventName>(event: T, cb: (ev: WindowEventMap[T]) => void) => {
      listeners.set(event, cb as (ev: WindowEventMap[WindowEventName]) => void);
      return () => listeners.delete(event);
    },
  } as unknown as Window;
  const dispose = mount(
    Text({
      key: 'selectable-text',
      selectable: true,
      style: { width: 120, height: 32, fontSize: 16 },
      children: 'abc',
    }),
    win,
    { renderer },
  );
  const rerender = () => listeners.get('resize')?.({ type: 'resize', width: 160, height: 100 });
  try {
    dispatchMouseDown({ type: 'mousedown', x: 1, y: 1, button: 0, modifiers: 0 });
    dispatchMouseMove({ type: 'mousemove', x: 1000, y: 1, dx: 999, dy: 0, modifiers: 0 });
    dispatchMouseUp({ type: 'mouseup', x: 1000, y: 1, button: 0, modifiers: 0 });
    rerender();
    listeners.get('keydown')?.({ type: 'keydown', key: 67, scancode: 0, modifiers: 8 });
    assertDeepEqual(clipboard, 'abc');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

test('TextInput onEditCommand routes Edit menu actions to the focused input', async () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<WindowEventName, (ev: WindowEventMap[WindowEventName]) => void>();
  let clipboard = 'pasted';
  const win = {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => undefined,
    clipboardText: () => clipboard,
    setClipboardText: (text: string) => {
      clipboard = text;
    },
    on: <T extends WindowEventName>(event: T, cb: (ev: WindowEventMap[T]) => void) => {
      listeners.set(event, cb as (ev: WindowEventMap[WindowEventName]) => void);
      return () => listeners.delete(event);
    },
  } as unknown as Window;
  const changes: string[] = [];
  const dispose = mount(
    TextInput({
      key: 'edit-cmd-input',
      value: 'hello',
      style: { width: 200, height: 32 },
      onChange: (v) => changes.push(v),
    }),
    win,
    { renderer },
  );
  const rerender = () => listeners.get('resize')?.({ type: 'resize', width: 160, height: 100 });
  try {
    dispatchMouseDown({ type: 'mousedown', x: 1, y: 1, button: 0, modifiers: 0 });
    dispatchMouseUp({ type: 'mouseup', x: 1, y: 1, button: 0, modifiers: 0 });
    rerender();
    let target = focusTarget();
    assert(target?.componentType === 'TextInput', 'TextInput must be focused');
    target?.onEditCommand?.('selectAll');
    rerender();
    target = focusTarget();
    target?.onEditCommand?.('copy');
    assertDeepEqual(clipboard, 'hello');
    clipboard = 'X';
    target?.onEditCommand?.('paste');
    await Promise.resolve();
    rerender();
    assertDeepEqual(changes.at(-1), 'X');
    target = focusTarget();
    target?.onEditCommand?.('selectAll');
    rerender();
    target = focusTarget();
    target?.onEditCommand?.('cut');
    rerender();
    assertDeepEqual(clipboard, 'X');
    assertDeepEqual(changes.at(-1), '');
    target = focusTarget();
    target?.onEditCommand?.('undo');
    rerender();
    assertDeepEqual(changes.at(-1), 'X');
    target = focusTarget();
    target?.onEditCommand?.('redo');
    rerender();
    assertDeepEqual(changes.at(-1), '');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

test('TextInput drag inside selection requests a drag-out via the drag sink', () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<WindowEventName, (ev: WindowEventMap[WindowEventName]) => void>();
  const dragPayloads: Array<{ text?: string }> = [];
  const win = {
    framebufferSize: () => [320, 100] as [number, number],
    requestRedraw: () => undefined,
    clipboardText: () => '',
    setClipboardText: () => undefined,
    startDrag: (payload: { text?: string }) => {
      dragPayloads.push(payload);
      return true;
    },
    on: <T extends WindowEventName>(event: T, cb: (ev: WindowEventMap[T]) => void) => {
      listeners.set(event, cb as (ev: WindowEventMap[WindowEventName]) => void);
      return () => listeners.delete(event);
    },
  } as unknown as Window;
  const dispose = mount(
    TextInput({
      key: 'drag-out-input',
      value: 'hello world',
      style: { width: 240, height: 32 },
    }),
    win,
    { renderer },
  );
  const rerender = () => listeners.get('resize')?.({ type: 'resize', width: 320, height: 100 });
  try {
    // Click to focus, select all via Cmd+A, then verify a drag initiated
    // from inside the selection invokes the drag sink with the selected
    // text payload. Routing goes through the window listeners so the
    // mount-installed `dragSink` is wired.
    listeners.get('mousedown')?.({ type: 'mousedown', x: 1, y: 1, button: 0, modifiers: 0 });
    listeners.get('mouseup')?.({ type: 'mouseup', x: 1, y: 1, button: 0, modifiers: 0 });
    rerender();
    listeners.get('keydown')?.({ type: 'keydown', key: 65, scancode: 0, modifiers: 8 });
    rerender();
    listeners.get('mousedown')?.({ type: 'mousedown', x: 40, y: 16, button: 0, modifiers: 0 });
    listeners.get('mousemove')?.({ type: 'mousemove', x: 80, y: 16, dx: 40, dy: 0, modifiers: 0 });
    listeners.get('mouseup')?.({ type: 'mouseup', x: 80, y: 16, button: 0, modifiers: 0 });
    assert(dragPayloads.length === 1, 'startDrag was invoked exactly once');
    assertDeepEqual(dragPayloads[0]?.text, 'hello world');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

await run();
