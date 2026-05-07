// @ts-ignore FXE synthetic package
import {
  clearHitTargets,
  dispatchMouseDown,
  dispatchMouseMove,
  dispatchMouseUp,
  registerHitTarget,
  resetEventPipeline,
} from 'fxe-ui';

import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('mouse dispatch sends hover and press callbacks in order', () => {
  clearHitTargets();
  const calls: string[] = [];
  registerHitTarget({
    id: 'button',
    rect: {
      x: 10,
      y: 10,
      width: 20,
      height: 20,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onHoverIn: () => calls.push('hover-in'),
    onPressIn: () => calls.push('press-in'),
    onPressOut: () => calls.push('press-out'),
    onPress: () => calls.push('press'),
    onHoverOut: () => calls.push('hover-out'),
  });
  dispatchMouseMove({ type: 'mousemove', x: 15, y: 15, dx: 0, dy: 0, modifiers: 0 });
  dispatchMouseDown({ type: 'mousedown', x: 15, y: 15, button: 0, modifiers: 0 });
  dispatchMouseUp({ type: 'mouseup', x: 15, y: 15, button: 0, modifiers: 0 });
  dispatchMouseMove({ type: 'mousemove', x: 40, y: 40, dx: 25, dy: 25, modifiers: 0 });
  assertDeepEqual(calls, ['hover-in', 'press-in', 'press-out', 'press', 'hover-out']);
});

test('topmost hit target receives events', () => {
  clearHitTargets();
  let pressed = '';
  registerHitTarget({
    id: 'bottom',
    z: 0,
    rect: {
      x: 0,
      y: 0,
      width: 30,
      height: 30,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onPress: () => {
      pressed = 'bottom';
    },
  });
  registerHitTarget({
    id: 'top',
    z: 1,
    rect: {
      x: 0,
      y: 0,
      width: 30,
      height: 30,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onPress: () => {
      pressed = 'top';
    },
  });
  dispatchMouseDown({ type: 'mousedown', x: 5, y: 5, button: 0, modifiers: 0 });
  dispatchMouseUp({ type: 'mouseup', x: 5, y: 5, button: 0, modifiers: 0 });
  assertEqual(pressed, 'top');
});

test('primary-button drag is delivered to the captured mousedown target', () => {
  clearHitTargets();
  resetEventPipeline();
  const calls: string[] = [];
  registerHitTarget({
    id: 'source',
    rect: {
      x: 0,
      y: 0,
      width: 10,
      height: 10,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onDrag: (ev) => calls.push(`source:${ev.x},${ev.y}`),
    onPressOut: () => calls.push('source:out'),
  });
  registerHitTarget({
    id: 'other',
    rect: {
      x: 100,
      y: 0,
      width: 10,
      height: 10,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onDrag: () => calls.push('other:drag'),
  });
  dispatchMouseDown({ type: 'mousedown', x: 5, y: 5, button: 0, modifiers: 0 });
  dispatchMouseMove({ type: 'mousemove', x: 105, y: 5, dx: 100, dy: 0, modifiers: 0 });
  dispatchMouseUp({ type: 'mouseup', x: 105, y: 5, button: 0, modifiers: 0 });
  dispatchMouseMove({ type: 'mousemove', x: 106, y: 5, dx: 1, dy: 0, modifiers: 0 });
  assertDeepEqual(calls, ['source:105,5', 'source:out']);
});

await run();
