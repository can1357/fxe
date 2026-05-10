// fxe-doc helpers — History, MultiRangeSelection, Decorations.

import { Decorations, History, MultiRangeSelection } from 'fxe-doc';
import { assert, assertDeepEqual, assertEqual, assertThrows, run, test } from './ts_harness.ts';

test('History undo/redo round-trips a single edit', () => {
  const doc = new TextDocument('hello');
  const h = new History(doc);
  h.dispatch([{ start: 5, removed: 0, inserted: ' world' }], { origin: 'paste' });
  assertEqual(doc.text(), 'hello world');
  assert(h.canUndo());
  assert(!h.canRedo());
  h.undo();
  assertEqual(doc.text(), 'hello');
  assert(!h.canUndo());
  assert(h.canRedo());
  h.redo();
  assertEqual(doc.text(), 'hello world');
});

test('History merges same-origin edits within window', () => {
  const doc = new TextDocument('');
  const h = new History(doc, { mergeWindowMs: 5_000 });
  h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
  h.dispatch([{ start: 1, removed: 0, inserted: 'b' }], { origin: 'type' });
  h.dispatch([{ start: 2, removed: 0, inserted: 'c' }], { origin: 'type' });
  assertEqual(doc.text(), 'abc');
  h.undo();
  // Merged: one undo reverts the whole group.
  assertEqual(doc.text(), '');
});

test('History keeps cross-origin edits as separate steps', () => {
  const doc = new TextDocument('');
  const h = new History(doc);
  h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
  h.dispatch([{ start: 1, removed: 0, inserted: 'X' }], { origin: 'paste' });
  h.undo();
  assertEqual(doc.text(), 'a', 'paste undone first');
  h.undo();
  assertEqual(doc.text(), '', 'type undone second');
});

test('History.break forces a new undo step', () => {
  const doc = new TextDocument('');
  const h = new History(doc, { mergeWindowMs: 60_000 });
  h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
  h.dispatch([{ start: 1, removed: 0, inserted: 'b' }], { origin: 'type', break: true });
  assertEqual(doc.text(), 'ab');
  h.undo();
  assertEqual(doc.text(), 'a', 'break: undo only the second edit');
});

test('History.transact groups multiple dispatches into one undo step', () => {
  const doc = new TextDocument('ab');
  const h = new History(doc);
  h.transact(() => {
    h.dispatch([{ start: 1, removed: 0, inserted: 'X' }], { origin: 'type' });
    h.dispatch([{ start: 2, removed: 0, inserted: 'Y' }], { origin: 'type' });
  });
  assertEqual(doc.text(), 'aXYb');
  h.undo();
  assertEqual(doc.text(), 'ab');
  assert(!h.canUndo());
});

test('History.transact handles shifted offsets for multi-caret style edits', () => {
  const doc = new TextDocument('abcde');
  const h = new History(doc);
  h.transact(() => {
    h.dispatch([{ start: 0, removed: 0, inserted: 'X' }], { origin: 'multi-cursor' });
    h.dispatch([{ start: 6, removed: 0, inserted: 'Y' }], { origin: 'multi-cursor' });
  });
  assertEqual(doc.text(), 'XabcdeY');
  h.undo();
  assertEqual(doc.text(), 'abcde');
});

test('History.transact flattens nested transactions', () => {
  const doc = new TextDocument('');
  const h = new History(doc);
  h.transact(() => {
    h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
    h.transact(() => {
      h.dispatch([{ start: 1, removed: 0, inserted: 'b' }], { origin: 'type' });
    });
    h.dispatch([{ start: 2, removed: 0, inserted: 'c' }], { origin: 'type' });
  });
  assertEqual(doc.text(), 'abc');
  h.undo();
  assertEqual(doc.text(), '');
  assert(!h.canUndo());
});

test('History.transact commits partial edits when the callback throws', () => {
  const doc = new TextDocument('');
  const h = new History(doc);
  assertThrows(() => {
    h.transact(() => {
      h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
      h.dispatch([{ start: 1, removed: 0, inserted: 'b' }], { origin: 'type' });
      throw new Error('boom');
    });
  }, /boom/);
  assertEqual(doc.text(), 'ab');
  assert(h.canUndo());
  h.undo();
  assertEqual(doc.text(), '');
});

test('History.transact origin override does not merge with later dispatches', () => {
  const doc = new TextDocument('');
  const h = new History(doc, { mergeWindowMs: 60_000 });
  h.transact(
    () => {
      h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
    },
    { origin: 'multi-cursor' },
  );
  const undo = (h as unknown as { undo_: Array<{ origin: string }> }).undo_;
  assertEqual(undo[undo.length - 1].origin, 'multi-cursor');
  h.dispatch([{ start: 1, removed: 0, inserted: 'b' }], { origin: 'multi-cursor' });
  h.undo();
  assertEqual(doc.text(), 'a');
  h.undo();
  assertEqual(doc.text(), '');
});

test('History.breakCoalescing still splits later edits after a transaction', () => {
  const doc = new TextDocument('');
  const h = new History(doc, { mergeWindowMs: 60_000 });
  h.transact(() => {
    h.dispatch([{ start: 0, removed: 0, inserted: 'a' }], { origin: 'type' });
  });
  h.dispatch([{ start: 1, removed: 0, inserted: 'b' }], { origin: 'type' });
  h.breakCoalescing();
  h.dispatch([{ start: 2, removed: 0, inserted: 'c' }], { origin: 'type' });
  assertEqual(doc.text(), 'abc');
  h.undo();
  assertEqual(doc.text(), 'ab');
  h.undo();
  assertEqual(doc.text(), 'a');
  h.undo();
  assertEqual(doc.text(), '');
});

test('MultiRangeSelection.cursor creates a single zero-width range', () => {
  const sel = MultiRangeSelection.cursor(7);
  assertEqual(sel.ranges.length, 1);
  assertDeepEqual(sel.primaryRange(), { anchor: 7, focus: 7 });
});

test('MultiRangeSelection merges overlapping ranges', () => {
  const sel = new MultiRangeSelection(
    [
      { anchor: 0, focus: 5 },
      { anchor: 4, focus: 8 },
      { anchor: 12, focus: 15 },
    ],
    1,
  );
  assertEqual(sel.ranges.length, 2);
  assertDeepEqual(sel.ranges[0], { anchor: 0, focus: 8 });
  assertDeepEqual(sel.ranges[1], { anchor: 12, focus: 15 });
});

test('MultiRangeSelection.map shifts cursors through edits', () => {
  const doc = new TextDocument('hello world');
  const sel = MultiRangeSelection.cursor(11); // end of doc
  const applied = doc.applyBatch([{ start: 5, removed: 0, inserted: ', great' }]);
  const next = sel.map(applied);
  assertDeepEqual(next.primaryRange(), { anchor: 18, focus: 18 });
});

test('MultiRangeSelection.map with bias=left holds at left edge', () => {
  const doc = new TextDocument('abc');
  const sel = new MultiRangeSelection([{ anchor: 1, focus: 1 }]);
  const applied = doc.applyBatch([{ start: 1, removed: 0, inserted: 'XY' }]);
  const right = sel.map(applied, 'right');
  const left = sel.map(applied, 'left');
  assertEqual(right.primaryRange().anchor, 3, 'right bias -> after insertion');
  assertEqual(left.primaryRange().anchor, 1, 'left bias -> before insertion');
});

test('Decorations.intersecting returns ranges in [start, end)', () => {
  const d = new Decorations<string>().add(0, 5, 'a').add(4, 9, 'b').add(20, 25, 'c');
  const hits = d.intersecting(3, 8);
  assertEqual(hits.length, 2);
  assertDeepEqual(hits.map((h) => h.payload).sort(), ['a', 'b']);
});

test('Decorations.map drops fully-deleted ranges', () => {
  const doc = new TextDocument('hello world');
  const d = new Decorations<string>().add(6, 11, 'world');
  const applied = doc.applyBatch([{ start: 5, removed: 6, inserted: '' }]);
  const after = d.map(applied);
  assertEqual(after.size, 0);
});

test('Decorations.map shifts ranges past edits', () => {
  const doc = new TextDocument('aaa BBB ccc');
  const d = new Decorations<string>().add(4, 7, 'mid');
  const applied = doc.applyBatch([{ start: 0, removed: 0, inserted: 'XX ' }]);
  const after = d.map(applied);
  assertEqual(after.size, 1);
  assertDeepEqual({ start: after.all()[0].start, end: after.all()[0].end }, { start: 7, end: 10 });
});

await run();
