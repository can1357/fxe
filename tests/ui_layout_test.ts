// @ts-ignore FXE synthetic package
import { type LayoutNode, type LayoutResult, layout } from 'fxe-ui';

import { assertEqual, run, test } from './ts_harness.ts';

type Expected = { x: number; y: number; width: number; height: number; children?: Expected[] };

function pick(result: LayoutResult): Expected {
  return {
    x: result.x,
    y: result.y,
    width: result.width,
    height: result.height,
    children: result.children.map(pick),
  };
}

function assertLayout(
  name: string,
  node: LayoutNode,
  expected: Expected,
  available = { width: 100, height: 100 },
): void {
  test(name, () => {
    assertEqual(JSON.stringify(pick(layout(node, available))), JSON.stringify(expected));
  });
}

assertLayout(
  'column default stacks children vertically',
  { children: [{ style: { height: 20 } }, { style: { height: 30 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 100, height: 20, children: [] },
      { x: 0, y: 20, width: 100, height: 30, children: [] },
    ],
  },
);
assertLayout(
  'row lays out along horizontal axis',
  {
    style: { flexDirection: 'row' },
    children: [{ style: { width: 20 } }, { style: { width: 30 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 20, height: 100, children: [] },
      { x: 20, y: 0, width: 30, height: 100, children: [] },
    ],
  },
);
assertLayout(
  'padding offsets content box',
  { style: { padding: 10 }, children: [{ style: { height: 20 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 10, y: 10, width: 80, height: 20, children: [] }],
  },
);
assertLayout(
  'margin separates siblings',
  {
    children: [{ style: { height: 10, marginBottom: 5 } }, { style: { height: 10, marginTop: 3 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 100, height: 10, children: [] },
      { x: 0, y: 18, width: 100, height: 10, children: [] },
    ],
  },
);
assertLayout(
  'row gap separates row children',
  {
    style: { flexDirection: 'row', gap: 4 },
    children: [{ style: { width: 10 } }, { style: { width: 10 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 10, height: 100, children: [] },
      { x: 14, y: 0, width: 10, height: 100, children: [] },
    ],
  },
);
assertLayout(
  'column gap separates column children',
  { style: { gap: 4 }, children: [{ style: { height: 10 } }, { style: { height: 10 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 100, height: 10, children: [] },
      { x: 0, y: 14, width: 100, height: 10, children: [] },
    ],
  },
);
assertLayout(
  'flex grow distributes positive free space',
  {
    style: { flexDirection: 'row' },
    children: [{ style: { width: 10, flexGrow: 1 } }, { style: { width: 10, flexGrow: 3 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 30, height: 100, children: [] },
      { x: 30, y: 0, width: 70, height: 100, children: [] },
    ],
  },
);
assertLayout(
  'flex shrink distributes negative free space',
  {
    style: { flexDirection: 'row' },
    children: [{ style: { width: 80, flexShrink: 1 } }, { style: { width: 80, flexShrink: 1 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 50, height: 100, children: [] },
      { x: 50, y: 0, width: 50, height: 100, children: [] },
    ],
  },
);
assertLayout(
  'justify center offsets packed row',
  {
    style: { flexDirection: 'row', justifyContent: 'center' },
    children: [{ style: { width: 20 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 40, y: 0, width: 20, height: 100, children: [] }],
  },
);
assertLayout(
  'justify flex-end offsets packed row',
  {
    style: { flexDirection: 'row', justifyContent: 'flex-end' },
    children: [{ style: { width: 20 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 80, y: 0, width: 20, height: 100, children: [] }],
  },
);
assertLayout(
  'space-between distributes row children',
  {
    style: { flexDirection: 'row', justifyContent: 'space-between' },
    children: [{ style: { width: 10 } }, { style: { width: 10 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 10, height: 100, children: [] },
      { x: 90, y: 0, width: 10, height: 100, children: [] },
    ],
  },
);
assertLayout(
  'align center centers child on cross axis',
  {
    style: { flexDirection: 'row', alignItems: 'center' },
    children: [{ style: { width: 10, height: 20 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 40, width: 10, height: 20, children: [] }],
  },
);
assertLayout(
  'align flex-end places child at cross end',
  {
    style: { flexDirection: 'row', alignItems: 'flex-end' },
    children: [{ style: { width: 10, height: 20 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 80, width: 10, height: 20, children: [] }],
  },
);
assertLayout(
  'align self overrides align items',
  {
    style: { flexDirection: 'row', alignItems: 'flex-start' },
    children: [{ style: { width: 10, height: 20, alignSelf: 'flex-end' } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 80, width: 10, height: 20, children: [] }],
  },
);
assertLayout(
  'percent width resolves against parent',
  { children: [{ style: { width: '50%', height: 10 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 50, height: 10, children: [] }],
  },
);
assertLayout(
  'percent height resolves against parent',
  { children: [{ style: { height: '50%' } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 100, height: 50, children: [] }],
  },
);
assertLayout(
  'min width clamps child',
  { style: { flexDirection: 'row' }, children: [{ style: { width: 10, minWidth: 30 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 30, height: 100, children: [] }],
  },
);
assertLayout(
  'max width clamps child',
  { style: { flexDirection: 'row' }, children: [{ style: { width: 40, maxWidth: 25 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 25, height: 100, children: [] }],
  },
);
assertLayout(
  'absolute positions against parent padding box',
  {
    style: { padding: 5 },
    children: [{ style: { position: 'absolute', left: 10, top: 15, width: 20, height: 25 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 15, y: 20, width: 20, height: 25, children: [] }],
  },
);
assertLayout(
  'absolute right bottom positions child',
  { children: [{ style: { position: 'absolute', right: 10, bottom: 15, width: 20, height: 25 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 70, y: 60, width: 20, height: 25, children: [] }],
  },
);
assertLayout(
  'wrap creates second row',
  {
    style: { flexDirection: 'row', flexWrap: 'wrap' },
    children: [{ style: { width: 60, height: 10 } }, { style: { width: 60, height: 10 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 60, height: 10, children: [] },
      { x: 0, y: 10, width: 60, height: 10, children: [] },
    ],
  },
);
assertLayout(
  'wrap row gap separates lines',
  {
    style: { flexDirection: 'row', flexWrap: 'wrap', rowGap: 5 },
    children: [{ style: { width: 60, height: 10 } }, { style: { width: 60, height: 10 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 0, width: 60, height: 10, children: [] },
      { x: 0, y: 15, width: 60, height: 10, children: [] },
    ],
  },
);
assertLayout(
  'row reverse reverses visual order',
  {
    style: { flexDirection: 'row-reverse' },
    children: [{ style: { width: 20 } }, { style: { width: 30 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 30, y: 0, width: 20, height: 100, children: [] },
      { x: 0, y: 0, width: 30, height: 100, children: [] },
    ],
  },
);
assertLayout(
  'column reverse reverses visual order',
  {
    style: { flexDirection: 'column-reverse' },
    children: [{ style: { height: 20 } }, { style: { height: 30 } }],
  },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      { x: 0, y: 30, width: 100, height: 20, children: [] },
      { x: 0, y: 0, width: 100, height: 30, children: [] },
    ],
  },
);
assertLayout(
  'measure callback supplies intrinsic size',
  { style: { flexDirection: 'row' }, children: [{ measure: () => ({ width: 12, height: 8 }) }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 12, height: 8, children: [] }],
  },
);
assertLayout(
  'explicit root size may be smaller than available',
  { style: { width: 40, height: 50 }, children: [{ style: { height: 10 } }] },
  {
    x: 0,
    y: 0,
    width: 40,
    height: 50,
    children: [{ x: 0, y: 0, width: 40, height: 10, children: [] }],
  },
);
assertLayout(
  'padding x and y apply independently',
  { style: { paddingX: 10, paddingY: 5 }, children: [{ style: { height: 10 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 10, y: 5, width: 80, height: 10, children: [] }],
  },
);
assertLayout(
  'flex basis participates in growth',
  { style: { flexDirection: 'row' }, children: [{ style: { flexBasis: 20, flexGrow: 1 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 100, height: 100, children: [] }],
  },
);
assertLayout(
  'display none skips child layout',
  { children: [{ style: { display: 'none', height: 20 } }, { style: { height: 10 } }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [{ x: 0, y: 0, width: 100, height: 10, children: [] }],
  },
);
assertLayout(
  'nested containers recurse',
  { children: [{ style: { height: 50, padding: 5 }, children: [{ style: { height: 10 } }] }] },
  {
    x: 0,
    y: 0,
    width: 100,
    height: 100,
    children: [
      {
        x: 0,
        y: 0,
        width: 100,
        height: 50,
        children: [{ x: 5, y: 5, width: 90, height: 10, children: [] }],
      },
    ],
  },
);

await run();
