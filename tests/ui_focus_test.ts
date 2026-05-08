import {
  clearFocus,
  clearHitTargets,
  dispatchKeyDown,
  focusedTargetId,
  registerHitTarget,
} from 'fxe-ui';

import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

const rect = {
  x: 0,
  y: 0,
  width: 10,
  height: 10,
  paddingLeft: 0,
  paddingTop: 0,
  paddingRight: 0,
  paddingBottom: 0,
  children: [],
};

test('Tab cycles focus in registration order', () => {
  clearHitTargets();
  clearFocus();
  const calls: string[] = [];
  registerHitTarget({
    id: 'a',
    rect,
    onFocus: () => calls.push('a:focus'),
    onBlur: () => calls.push('a:blur'),
  });
  registerHitTarget({
    id: 'b',
    rect,
    onFocus: () => calls.push('b:focus'),
    onBlur: () => calls.push('b:blur'),
  });
  dispatchKeyDown({ type: 'keydown', key: 258, scancode: 0, modifiers: 0 });
  assertEqual(focusedTargetId(), 'b');
  dispatchKeyDown({ type: 'keydown', key: 258, scancode: 0, modifiers: 0 });
  assertEqual(focusedTargetId(), 'a');
  assertDeepEqual(calls, ['b:focus', 'b:blur', 'a:focus']);
});

await run();
