import {
  attachFocusAdvancePreempt,
  dispatchKeyDown,
  resetEventPipeline,
} from '../packages/fxe-ui/src/mount/event_pipeline.ts';
import { clearFocus, focusedTargetId, focusTarget } from '../packages/fxe-ui/src/mount/focus.ts';
import { clearHitTargets, registerHitTarget } from '../packages/fxe-ui/src/mount/hit_test.ts';

import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

const rect = {
  x: 0,
  y: 0,
  width: 16,
  height: 16,
  paddingLeft: 0,
  paddingTop: 0,
  paddingRight: 0,
  paddingBottom: 0,
  children: [],
};

function describeKeyEvent(ev: unknown): string {
  const key = ev as { key: number; modifiers: number };
  return `${String(key.key)}:${String(key.modifiers)}`;
}

function resetFixture(): void {
  resetEventPipeline();
  clearHitTargets();
  clearFocus();
}

function registerFocusablePair(events: string[]): void {
  registerHitTarget({
    id: 'first',
    rect,
    onFocus: () => events.push('first:focus'),
    onBlur: () => events.push('first:blur'),
    onKeyDown: (ev) => events.push(`first:key:${describeKeyEvent(ev)}`),
  });
  registerHitTarget({
    id: 'second',
    rect,
    onFocus: () => events.push('second:focus'),
    onBlur: () => events.push('second:blur'),
    onKeyDown: (ev) => events.push(`second:key:${describeKeyEvent(ev)}`),
  });
}

// Proves the baseline contract: without a preempt hook, Tab advances focus
// before keydown routing continues to the newly focused target.
test('fxe_ui_text_input_tab_behavior_test default Tab advances focus', () => {
  resetFixture();
  try {
    const events: string[] = [];
    registerFocusablePair(events);
    focusTarget('first');
    dispatchKeyDown({ type: 'keydown', key: 258, scancode: 0, modifiers: 0 });
    assertEqual(focusedTargetId(), 'second');
    assertDeepEqual(events, ['first:focus', 'first:blur', 'second:focus', 'second:key:258:0']);
  } finally {
    resetFixture();
  }
});

// Proves the regression target: when a preempt hook claims Tab, focus stays put
// and the focused control still receives the Tab keydown it needs to insert text.
test('fxe_ui_text_input_tab_behavior_test preempted Tab stays on current target and still routes keydown', () => {
  resetFixture();
  try {
    const events: string[] = [];
    registerFocusablePair(events);
    const detach = attachFocusAdvancePreempt({ shouldPreemptFocusAdvance: (ev) => ev.key === 258 });
    try {
      focusTarget('first');
      dispatchKeyDown({ type: 'keydown', key: 258, scancode: 0, modifiers: 0 });
      assertEqual(focusedTargetId(), 'first');
      assertDeepEqual(events, ['first:focus', 'first:key:258:0']);
    } finally {
      detach();
    }
  } finally {
    resetFixture();
  }
});
// Proves Shift+Tab follows the same preempt path, preserving modifiers for the
// focused control instead of moving focus backward.

test('fxe_ui_text_input_tab_behavior_test shift Tab is also routed to a preempting target', () => {
  resetFixture();
  try {
    const events: string[] = [];
    registerFocusablePair(events);
    const detach = attachFocusAdvancePreempt({ shouldPreemptFocusAdvance: (ev) => ev.key === 258 });
    try {
      focusTarget('first');
      dispatchKeyDown({ type: 'keydown', key: 258, scancode: 0, modifiers: 1 });
      assertEqual(focusedTargetId(), 'first');
      assertDeepEqual(events, ['first:focus', 'first:key:258:1']);
    } finally {
      detach();
    }
  } finally {
    resetFixture();
  }
});

// Proves the hook is not sticky: once detached, normal focus traversal resumes.
test('fxe_ui_text_input_tab_behavior_test detaching preempt restores focus advance', () => {
  resetFixture();
  try {
    const events: string[] = [];
    registerFocusablePair(events);
    const detach = attachFocusAdvancePreempt({ shouldPreemptFocusAdvance: (ev) => ev.key === 258 });
    focusTarget('first');
    detach();
    dispatchKeyDown({ type: 'keydown', key: 258, scancode: 0, modifiers: 0 });
    assertEqual(focusedTargetId(), 'second');
    assertDeepEqual(events, ['first:focus', 'first:blur', 'second:focus', 'second:key:258:0']);
  } finally {
    resetFixture();
  }
});

await run();
