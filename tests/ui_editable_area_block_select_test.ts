import { blockSelectionFromAnchorFocus } from '../packages/fxe-ui/src/components/editable_area_block_select.ts';
import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

type DocumentLike = InstanceType<typeof TextDocument>;

test('forward block selection preserves direction across rows', () => {
  const doc = new TextDocument('abcd\nefgh\nijkl');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 0, col: 1 },
    focus: { line: 2, col: 3 },
  });

  assertEqual(sel.primary, 2);
  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 1 }, focus: { line: 0, col: 3 } },
    { anchor: { line: 1, col: 1 }, focus: { line: 1, col: 3 } },
    { anchor: { line: 2, col: 1 }, focus: { line: 2, col: 3 } },
  ]);
});

test('reverse block selection still covers the rectangle', () => {
  const doc = new TextDocument('abcd\nefgh\nijkl');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 2, col: 3 },
    focus: { line: 0, col: 1 },
  });

  assertEqual(sel.primary, 0);
  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 3 }, focus: { line: 0, col: 1 } },
    { anchor: { line: 1, col: 3 }, focus: { line: 1, col: 1 } },
    { anchor: { line: 2, col: 3 }, focus: { line: 2, col: 1 } },
  ]);
});

test('block selection clamps columns to each line length', () => {
  const doc = new TextDocument('abcdef\n\nxy');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 0, col: 4 },
    focus: { line: 2, col: 5 },
  });

  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 4 }, focus: { line: 0, col: 5 } },
    { anchor: { line: 1, col: 0 }, focus: { line: 1, col: 0 } },
    { anchor: { line: 2, col: 2 }, focus: { line: 2, col: 2 } },
  ]);
});

test('single-line block selection reduces to one range', () => {
  const doc = new TextDocument('abcdef');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 0, col: 4 },
    focus: { line: 0, col: 1 },
  });

  assertEqual(sel.primary, 0);
  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 4 }, focus: { line: 0, col: 1 } },
  ]);
});

test('anchor equals focus yields a single cursor', () => {
  const doc = new TextDocument('abcdef');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 0, col: 2 },
    focus: { line: 0, col: 2 },
  });

  assertEqual(sel.primary, 0);
  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 2 }, focus: { line: 0, col: 2 } },
  ]);
});

test('empty document yields a single empty cursor at 0,0', () => {
  const doc = new TextDocument();
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 0, col: 0 },
    focus: { line: 0, col: 0 },
  });

  assertEqual(sel.primary, 0);
  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 0 }, focus: { line: 0, col: 0 } },
  ]);
});

test('focus side stays on the focus column for every row', () => {
  const doc = new TextDocument('abcd\nxyz\nmnop');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 2, col: 4 },
    focus: { line: 0, col: 1 },
  });

  assertDeepEqual(
    lineCols(doc, sel).map((range) => range.focus.col),
    [1, 1, 1],
  );
});

test('zero-width block drops non-focus empty rows when other rows have text', () => {
  const doc = new TextDocument('ab\n\ncd');
  const sel = blockSelectionFromAnchorFocus(doc, {
    anchor: { line: 0, col: 1 },
    focus: { line: 2, col: 1 },
  });

  assertEqual(sel.primary, 1);
  assertDeepEqual(lineCols(doc, sel), [
    { anchor: { line: 0, col: 1 }, focus: { line: 0, col: 1 } },
    { anchor: { line: 2, col: 1 }, focus: { line: 2, col: 1 } },
  ]);
});

function lineCols(
  doc: DocumentLike,
  sel: { ranges: ReadonlyArray<{ anchor: number; focus: number }> },
) {
  return sel.ranges.map((range) => ({
    anchor: doc.offsetToLineCol(range.anchor),
    focus: doc.offsetToLineCol(range.focus),
  }));
}

await run();
