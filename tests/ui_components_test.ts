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
  registerHitTarget,
  render,
  resetEventPipeline,
  setRenderTarget,
  Text,
  View,
} from 'fxe-ui';
import {
  captureHitTargetsSince,
  clearHitTargets as clearRawHitTargets,
  hitTargets as rawHitTargets,
  registerHitTarget as registerRawHitTarget,
  replayHitTargets,
} from '../packages/fxe-ui/src/mount/hit_test.ts';

import { assert, assertEqual, run, test } from './ts_harness.ts';

function hitRect() {
  return {
    x: 0,
    y: 0,
    width: 20,
    height: 20,
    paddingLeft: 0,
    paddingTop: 0,
    paddingRight: 0,
    paddingBottom: 0,
    children: [],
  };
}

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

test('hitTest chooses highest z without sorting allocation', () => {
  clearHitTargets();
  registerHitTarget({
    id: 'low',
    rect: hitRect(),
    componentType: 'View',
    a11y: {},
    z: 10,
  });
  registerHitTarget({
    id: 'high',
    rect: hitRect(),
    componentType: 'Pressable',
    a11y: {},
    onPress: () => undefined,
    z: 100,
  });
  registerHitTarget({
    id: 'middle',
    rect: hitRect(),
    componentType: 'Button',
    a11y: {},
    onPress: () => undefined,
    z: 50,
  });
  assertEqual(hitTest(5, 5)?.id, 'high');
  clearHitTargets();
});

test('replayHitTargets skips z rewrites for stable cached slices', () => {
  clearRawHitTargets();
  registerRawHitTarget({ id: 'a', rect: hitRect() });
  registerRawHitTarget({ id: 'b', rect: hitRect() });
  const captured = captureHitTargetsSince(0);

  clearRawHitTargets();
  replayHitTargets(captured);
  assertEqual(captured[0]?.z, 0);
  assertEqual(captured[1]?.z, 1);
  Object.freeze(captured[0]);
  Object.freeze(captured[1]);

  clearRawHitTargets();
  replayHitTargets(captured);
  assertEqual(rawHitTargets().length, 2);

  clearRawHitTargets();
  registerRawHitTarget({ id: 'c', rect: hitRect() });
  registerRawHitTarget({ id: 'd', rect: hitRect() });
  const shifted = captureHitTargetsSince(0);
  clearRawHitTargets();
  registerRawHitTarget({ id: 'leading', rect: hitRect() });
  replayHitTargets(shifted);
  assertEqual(shifted[0]?.z, 1);
  assertEqual(shifted[1]?.z, 2);
  clearRawHitTargets();
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
