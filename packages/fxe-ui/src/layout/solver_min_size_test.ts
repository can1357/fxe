import { expect, test } from 'bun:test';

import { solveLayout } from './solver.ts';
import type { LayoutNode } from './types.ts';

test('clamps column child main axis to minHeight after intrinsic sizing', () => {
  const root: LayoutNode = {
    style: { width: 200, height: 200 },
    children: [
      {
        style: { width: 96, height: 30 },
        children: [
          {
            style: { minHeight: 36 },
            children: [{ measure: () => ({ width: 5, height: 14 }) }],
          },
        ],
      },
    ],
  };

  const result = solveLayout(root, { width: 200, height: 200 });
  expect(result.children[0]?.children[0]?.height).toBe(36);
});

test('clamps row child main axis to minWidth after shrink distribution', () => {
  const root: LayoutNode = {
    style: { width: 150, height: 40, flexDirection: 'row' },
    children: [
      { style: { width: 100, flexShrink: 1 } },
      { style: { width: 100, minWidth: 100, flexShrink: 1 } },
    ],
  };

  const result = solveLayout(root, { width: 150, height: 40 });
  expect(result.children[0]?.width).toBe(50);
  expect(result.children[1]?.width).toBe(100);
});

test('keeps cross axis maxHeight clamp in row layout', () => {
  const root: LayoutNode = {
    style: { width: 200, height: 100, flexDirection: 'row' },
    children: [{ style: { width: 20, height: 80, maxHeight: 40 } }],
  };

  const result = solveLayout(root, { width: 200, height: 100 });
  expect(result.children[0]?.height).toBe(40);
});
