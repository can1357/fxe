import type { AccessibilityTreeSnapshot } from 'fxe-ui';
import { getA11yBridge, publishAccessibilityTree } from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

function makeSnapshot(generation: number): AccessibilityTreeSnapshot {
  const root = {
    id: `root-${generation}`,
    parentId: null,
    role: 'group' as const,
    label: `root ${generation}`,
    state: {},
    rect: { x: 0, y: 0, width: 10, height: 10 },
    focusable: false,
    liveRegion: 'off' as const,
    children: [],
  };
  return {
    rootId: root.id,
    generation,
    focusedId: null,
    nodesById: { [root.id]: root },
    childrenById: { [root.id]: [] },
  };
}

test('a11y bridge publishes snapshots through the global cache', () => {
  const bridgeA = getA11yBridge();
  const bridgeB = getA11yBridge();
  assertEqual(bridgeA, bridgeB);
  bridgeA.clear();

  const initialRevision = globalThis.__fxeA11y?.revision ?? 0;
  const snapshot = makeSnapshot(initialRevision + 1);
  const received: AccessibilityTreeSnapshot[] = [];
  let nestedCalls = 0;
  let disposeNested: () => void = () => undefined;
  const dispose = bridgeA.subscribe((value) => {
    received.push(value);
    disposeNested = bridgeA.subscribe(() => {
      nestedCalls += 1;
    });
  });

  publishAccessibilityTree(snapshot);

  assertEqual(bridgeA.latest(), snapshot);
  assertEqual(globalThis.__fxeA11y?.snapshot, snapshot);
  assertEqual(globalThis.__fxeA11y?.revision, initialRevision + 1);
  assertEqual(received.length, 1);
  assertEqual(received[0], snapshot);
  assertEqual(nestedCalls, 0);

  dispose();
  disposeNested();
});

test('a11y bridge subscriber disposers detach cleanly', () => {
  const bridge = getA11yBridge();
  bridge.clear();

  let calls = 0;
  const dispose = bridge.subscribe(() => {
    calls += 1;
  });

  publishAccessibilityTree(makeSnapshot(101));
  dispose();
  publishAccessibilityTree(makeSnapshot(102));

  assertEqual(calls, 1);
  assert(bridge.latest() !== null);
});

void run();
