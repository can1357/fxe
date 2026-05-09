import assert from 'node:assert/strict';

import { solveLayout } from './solver.ts';
import type { LayoutNode } from './types.ts';

function textNode(width: number, height: number): LayoutNode {
  return {
    measure: () => ({ width, height }),
  };
}

{
  const root: LayoutNode = {
    style: { width: 920, height: 720 },
    children: [
      {
        children: [
          {
            style: { flexDirection: 'row' },
            children: [textNode(32, 14)],
          },
        ],
      },
    ],
  };

  const result = solveLayout(root, { width: 920, height: 720 });
  const outer = result.children[0];
  const inner = outer.children[0];
  const text = inner.children[0];

  assert.equal(outer.height, 14);
  assert.equal(inner.height, 14);
  assert.equal(text.height, 14);
}

{
  const root: LayoutNode = {
    style: { width: 200, height: 200 },
    children: [
      {
        style: { height: 50 },
        children: [{ style: { flexDirection: 'row' }, children: [textNode(32, 60)] }],
      },
    ],
  };

  const result = solveLayout(root, { width: 200, height: 200 });
  assert.equal(result.children[0]?.height, 50);
}

{
  const root: LayoutNode = {
    style: { width: 200, height: 200 },
    children: [{}],
  };

  const result = solveLayout(root, { width: 200, height: 200 });
  assert.equal(result.children[0]?.height, 0);
}
