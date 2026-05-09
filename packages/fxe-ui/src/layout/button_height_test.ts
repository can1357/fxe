import { expect, test } from 'bun:test';

import { solveLayout } from './index.ts';
import type { LayoutNode } from './types.ts';

function textNode(): LayoutNode {
  return {
    measure: () => ({ width: 32, height: 14 }),
  };
}

function buttonLikeNode(): LayoutNode {
  return {
    style: {
      minHeight: 36,
      height: '100%',
      paddingY: 8,
      paddingX: 16,
      alignItems: 'center',
      justifyContent: 'center',
    },
    children: [textNode()],
  };
}

test('button-like child keeps minHeight in short wrapper and fills taller wrapper', () => {
  const root: LayoutNode = {
    style: { width: 200, height: 200 },
    children: [
      {
        style: { width: 96, height: 30 },
        children: [buttonLikeNode()],
      },
      {
        style: { width: 96, height: 50 },
        children: [buttonLikeNode()],
      },
    ],
  };

  const result = solveLayout(root, { width: 200, height: 200 });
  expect(result.children[0]?.children[0]?.height).toBe(36);
  expect(result.children[1]?.children[0]?.height).toBe(50);
});
