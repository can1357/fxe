// @ts-ignore FXE synthetic package
import {
  CommandBuffer,
  type Renderer,
  type Window,
  type WindowEventMap,
  type WindowEventName,
} from 'fxe';
// @ts-ignore FXE synthetic package
import {
  clearFocus,
  clearHitTargets,
  dispatchKeyDown,
  dispatchKeyPress,
  focusTarget,
  mount,
  dispatchMouseDown,
  dispatchMouseMove,
  dispatchMouseUp,
  registerHitTarget,
  Text,
  TextInput,
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
  const renderer = new CommandBuffer() as Renderer;
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
  const renderer = new CommandBuffer() as Renderer;
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
    dispatchKeyDown({ type: 'keydown', key: 262, scancode: 0, modifiers: 1 }, win);
    rerender();
    dispatchKeyDown({ type: 'keydown', key: 67, scancode: 0, modifiers: 8 }, win);
    assertDeepEqual(clipboard, 'a');
    clipboard = 'Z';
    dispatchKeyDown({ type: 'keydown', key: 86, scancode: 0, modifiers: 8 }, win);
    assertDeepEqual(changes.at(-1), 'Zbc');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

test('selectable Text copies the dragged read-only selection', () => {
  clearHitTargets();
  clearFocus();
  const renderer = new CommandBuffer() as Renderer;
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
    dispatchKeyDown({ type: 'keydown', key: 67, scancode: 0, modifiers: 8 }, win);
    assertDeepEqual(clipboard, 'abc');
  } finally {
    dispose();
    clearHitTargets();
    clearFocus();
  }
});

await run();
