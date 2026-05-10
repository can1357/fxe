import type { AccessibilityNodeSnapshot, VirtualDescendantSource } from 'fxe-ui';
import { expandVirtualDescendants, getVirtualSources, registerVirtualSource } from 'fxe-ui';

import { assertEqual, run, test } from './ts_harness.ts';

function makeRenderedChild(index: number): AccessibilityNodeSnapshot {
  return {
    id: `rendered-${index}`,
    parentId: 'listA',
    role: 'listitem',
    label: `rendered ${index}`,
    state: {},
    rect: { x: index, y: index, width: 10, height: 10 },
    focusable: false,
    liveRegion: 'off',
    children: [],
  };
}

function makeVirtualSource(): VirtualDescendantSource {
  return {
    parentId: 'listA',
    totalCount: 10,
    renderedRange: [2, 4],
    buildVirtualNode(index) {
      return {
        id: `listA-row-${index}`,
        parentId: 'listA',
        role: 'listitem',
        label: `row ${index}`,
        state: {},
        rect: { x: 0, y: 0, width: 0, height: 0 },
        focusable: false,
        liveRegion: 'off',
        children: [],
      };
    },
  };
}

test('expandVirtualDescendants preserves rendered rows and synthesises offscreen rows', () => {
  const rendered2 = makeRenderedChild(2);
  const rendered3 = makeRenderedChild(3);
  const rendered4 = makeRenderedChild(4);
  const source = makeVirtualSource();
  const node: AccessibilityNodeSnapshot = {
    id: 'listA',
    parentId: null,
    role: 'list',
    label: 'List A',
    state: {},
    rect: { x: 0, y: 0, width: 100, height: 100 },
    focusable: false,
    liveRegion: 'off',
    children: [rendered2, rendered3, rendered4],
  };

  const expanded = expandVirtualDescendants(node, source);

  assertEqual(expanded.children.length, 10);
  assertEqual(expanded.children[2], rendered2);
  assertEqual(expanded.children[3], rendered3);
  assertEqual(expanded.children[4], rendered4);
  assertEqual(
    expanded.children.map((child) => ({ id: child.id, offscreen: child.state.offscreen === true })),
    [
      { id: 'listA-row-0', offscreen: true },
      { id: 'listA-row-1', offscreen: true },
      { id: 'rendered-2', offscreen: false },
      { id: 'rendered-3', offscreen: false },
      { id: 'rendered-4', offscreen: false },
      { id: 'listA-row-5', offscreen: true },
      { id: 'listA-row-6', offscreen: true },
      { id: 'listA-row-7', offscreen: true },
      { id: 'listA-row-8', offscreen: true },
      { id: 'listA-row-9', offscreen: true },
    ],
  );
});

test('registerVirtualSource exposes the live registry and unregisters cleanly', () => {
  const source = makeVirtualSource();
  const dispose = registerVirtualSource(source);

  assertEqual(getVirtualSources().get('listA'), source);
  dispose();
  assertEqual(getVirtualSources().has('listA'), false);
});

void run();
