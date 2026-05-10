import { MultiRangeSelection } from 'fxe-doc';

import {
  addNextOccurrence,
  applyEditsAtRanges,
  expandLines,
  expandToWord,
  indentSelection,
} from '../packages/fxe-ui/src/components/editable_area_logic.ts';
import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('applyEditsAtRanges inserts at multiple cursors in ascending order', () => {
  const doc = new TextDocument('abcdef');
  const sel = new MultiRangeSelection([
    { anchor: 2, focus: 2 },
    { anchor: 5, focus: 5 },
  ]);

  const { edits, nextSel } = applyEditsAtRanges(doc, sel, 'x', 'type');
  assertDeepEqual(edits, [
    { start: 2, removed: 0, inserted: 'x' },
    { start: 5, removed: 0, inserted: 'x' },
  ]);

  doc.applyBatch(edits);
  assertEqual(doc.text(), 'abxcdexf');
  assertDeepEqual(nextSel.ranges, [
    { anchor: 3, focus: 3 },
    { anchor: 7, focus: 7 },
  ]);
});

test('applyEditsAtRanges backspaces at multiple cursors', () => {
  const doc = new TextDocument('abcdef');
  const sel = new MultiRangeSelection([
    { anchor: 2, focus: 2 },
    { anchor: 5, focus: 5 },
  ]);

  const { edits, nextSel } = applyEditsAtRanges(doc, sel, '', 'delete-backward');
  assertDeepEqual(edits, [
    { start: 1, removed: 1, inserted: '' },
    { start: 4, removed: 1, inserted: '' },
  ]);

  doc.applyBatch(edits);
  assertEqual(doc.text(), 'acdf');
  assertDeepEqual(nextSel.ranges, [
    { anchor: 1, focus: 1 },
    { anchor: 3, focus: 3 },
  ]);
});

test('expandToWord grows a cursor to the surrounding word', () => {
  const doc = new TextDocument('say hello world');
  const sel = MultiRangeSelection.cursor(5);

  const nextSel = expandToWord(doc, sel);
  assertDeepEqual(nextSel.primaryRange(), { anchor: 4, focus: 9 });
});

test('addNextOccurrence adds the next match of the primary selection', () => {
  const doc = new TextDocument('foo bar foo baz foo');
  const sel = new MultiRangeSelection([{ anchor: 0, focus: 3 }]);

  const nextSel = addNextOccurrence(doc, sel);
  assertDeepEqual(nextSel.ranges, [
    { anchor: 0, focus: 3 },
    { anchor: 8, focus: 11 },
  ]);
  assertEqual(nextSel.primary, 1);
});

test('expandLines selects whole lines for each range', () => {
  const doc = new TextDocument('one\ntwo\nthree\nfour');
  const sel = new MultiRangeSelection([
    { anchor: 1, focus: 1 },
    { anchor: 10, focus: 10 },
  ]);

  const nextSel = expandLines(doc, sel);
  assertDeepEqual(nextSel.ranges, [
    { anchor: 0, focus: 4 },
    { anchor: 8, focus: 14 },
  ]);
});

test('indentSelection emits ascending edits for multi-range line selections', () => {
  const doc = new TextDocument('aa\nbb\ncc\n');
  const sel = new MultiRangeSelection([
    { anchor: 0, focus: 5 },
    { anchor: 6, focus: 8 },
  ]);

  const { edits, nextSel } = indentSelection(doc, sel, 2);
  assertDeepEqual(edits, [
    { start: 0, removed: 0, inserted: '  ' },
    { start: 3, removed: 0, inserted: '  ' },
    { start: 6, removed: 0, inserted: '  ' },
  ]);

  doc.applyBatch(edits);
  assertEqual(doc.text(), '  aa\n  bb\n  cc\n');
  assertDeepEqual(nextSel.ranges, [
    { anchor: 2, focus: 9 },
    { anchor: 12, focus: 14 },
  ]);
});

await run();
