import { CommandBuffer } from 'fxe';
import {
  Button,
  clearHitTargets,
  dispatchMouseDown,
  dispatchMouseMove,
  dispatchMouseUp,
  hitTest,
  Image,
  Pressable,
  render,
  resetEventPipeline,
  setRenderTarget,
  Text,
  View,
} from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

Font.load('/System/Library/Fonts/Monaco.ttf', 32);
test('View paints background and border into command buffer', () => {
  const cb = new CommandBuffer();
  render(
    View({
      key: 'component-view',
      style: {
        width: 80,
        height: 40,
        backgroundColor: 0x11223344,
        borderWidth: 1,
        borderColor: 0xffffffff,
      },
    }),
    cb,
  );
  assert(cb.vertexCount() > 0, 'view should emit primitive vertices');
});

test('Text paints glyph primitives', () => {
  const cb = new CommandBuffer();
  render(
    Text({
      key: 'component-text',
      style: { left: 2, top: 3, fontSize: 16, color: 0xffffffff },
      children: 'Hello',
    }),
    cb,
  );
  assert(cb.vertexCount() > 0, 'text should emit primitive vertices');
});

test('Image emits placeholder quad with tint', () => {
  const cb = new CommandBuffer();
  render(Image({ key: 'component-image', width: 24, height: 16, tint: 0xff00ffff }), cb);
  assertEqual(cb.vertexCount(), 4);
});

test('Button composes pressable view and text', () => {
  const cb = new CommandBuffer();
  let presses = 0;
  render(
    Button({
      key: 'component-button',
      title: 'Save',
      style: { width: 90, height: 36 },
      onPress: () => {
        presses += 1;
      },
    }),
    cb,
  );
  assert(cb.vertexCount() > 4, 'button should paint surface and label');
  assert(hitTest(45, 18)?.componentType === 'Button', 'button hit target should be registered');
  dispatchMouseDown({ type: 'mousedown', x: 45, y: 18, button: 0, modifiers: 0 });
  dispatchMouseUp({ type: 'mouseup', x: 45, y: 18, button: 0, modifiers: 0 });
  assertEqual(presses, 1);
});

test('static Pressable does not request redraw for interaction state', () => {
  clearHitTargets();
  resetEventPipeline();
  let redraws = 0;
  setRenderTarget({ requestRedraw: () => ++redraws } as never);
  try {
    const cb = new CommandBuffer();
    render(
      Pressable({
        key: 'static-pressable',
        style: { width: 40, height: 20 },
        onPress: () => undefined,
        children: Text({ key: 'label', children: 'Static' }),
      }),
      cb,
    );
    dispatchMouseMove({ type: 'mousemove', x: 5, y: 5, dx: 0, dy: 0, modifiers: 0 });
    dispatchMouseDown({ type: 'mousedown', x: 5, y: 5, button: 0, modifiers: 0 });
    dispatchMouseUp({ type: 'mouseup', x: 5, y: 5, button: 0, modifiers: 0 });
    assertEqual(redraws, 0);
  } finally {
    setRenderTarget(null);
    clearHitTargets();
    resetEventPipeline();
  }
});

test('stateful Pressable requests redraw for interaction state', () => {
  clearHitTargets();
  resetEventPipeline();
  let redraws = 0;
  setRenderTarget({ requestRedraw: () => ++redraws } as never);
  try {
    const cb = new CommandBuffer();
    render(
      Pressable({
        key: 'stateful-pressable',
        style: (state) => ({
          width: 40,
          height: 20,
          opacity: state.hovered ? 0.5 : 1,
        }),
        onPress: () => undefined,
        children: Text({ key: 'label', children: 'Dynamic' }),
      }),
      cb,
    );
    dispatchMouseMove({ type: 'mousemove', x: 5, y: 5, dx: 0, dy: 0, modifiers: 0 });
    dispatchMouseDown({ type: 'mousedown', x: 5, y: 5, button: 0, modifiers: 0 });
    dispatchMouseUp({ type: 'mouseup', x: 5, y: 5, button: 0, modifiers: 0 });
    assert(redraws >= 2, 'stateful pressable should request redraw on hover/press');
  } finally {
    setRenderTarget(null);
    clearHitTargets();
    resetEventPipeline();
  }
});

await run();
