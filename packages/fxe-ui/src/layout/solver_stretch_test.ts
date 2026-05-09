import { assertEqual, run, test } from '../../../../tests/ts_harness.ts';
import { solveLayout } from './index.ts';
import type { LayoutNode } from './types.ts';

function measuredNode(style: LayoutNode['style'] = {}): LayoutNode {
  return {
    style,
    measure: () => ({ width: 32, height: 14 }),
  };
}

test('column stretch uses parent cross size when child width is intrinsic', () => {
  const root: LayoutNode = {
    style: { width: 920, height: 720 },
    children: [measuredNode()],
  };

  const result = solveLayout(root);
  assertEqual(result.children[0]?.x, 0);
  assertEqual(result.children[0]?.y, 0);
  assertEqual(result.children[0]?.width, 920);
  assertEqual(result.children[0]?.height, 14);
});

test('column stretch does not override explicit child width', () => {
  const root: LayoutNode = {
    style: { width: 920, height: 720 },
    children: [measuredNode({ width: 100 })],
  };

  const result = solveLayout(root);
  assertEqual(result.children[0]?.width, 100);
  assertEqual(result.children[0]?.height, 14);
});

test('alignSelf flex-start opts out of stretch', () => {
  const root: LayoutNode = {
    style: { width: 920, height: 720 },
    children: [measuredNode({ alignSelf: 'flex-start' })],
  };

  const result = solveLayout(root);
  assertEqual(result.children[0]?.width, 32);
  assertEqual(result.children[0]?.height, 14);
});

await run();
