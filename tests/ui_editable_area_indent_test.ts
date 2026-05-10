import { MultiRangeSelection } from 'fxe-doc';

import {
  indentSelection,
  inferIndent,
  outdentSelection,
} from '../packages/fxe-ui/src/components/editable_area_logic.ts';
import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('inferIndent prefers 2 spaces with full confidence', () => {
  const doc = new TextDocument('  a\n  b\n    c\n');
  assertDeepEqual(inferIndent(doc), { unit: 2, confidence: 1 });
});

test('inferIndent detects tabs with full confidence', () => {
  const doc = new TextDocument('\tfoo\n\tbar\n');
  assertDeepEqual(inferIndent(doc), { unit: 'tab', confidence: 1 });
});

test('inferIndent returns the default when no lines vote', () => {
  const doc = new TextDocument('');
  assertDeepEqual(inferIndent(doc), { unit: 2, confidence: 0 });
});

test('indentSelection prefixes each touched line with 2 spaces', () => {
  const doc = new TextDocument('a\nb\nc\n');
  const sel = new MultiRangeSelection([{ anchor: 0, focus: 5 }]);

  const outcome = indentSelection(doc, sel, 2);
  assertDeepEqual(outcome.edits, [
    { start: 0, removed: 0, inserted: '  ' },
    { start: 2, removed: 0, inserted: '  ' },
    { start: 4, removed: 0, inserted: '  ' },
  ]);

  doc.applyBatch(outcome.edits);
  assertEqual(doc.text(), '  a\n  b\n  c\n');
  assertDeepEqual(outcome.nextSel.ranges, [{ anchor: 2, focus: 11 }]);
});

test('outdentSelection strips 2 leading spaces and leaves flush lines alone', () => {
  const doc = new TextDocument('  a\nb\n    c\n');
  const sel = new MultiRangeSelection([{ anchor: 0, focus: 10 }]);

  const outcome = outdentSelection(doc, sel, 2);
  assertDeepEqual(outcome.edits, [
    { start: 0, removed: 2, inserted: '' },
    { start: 6, removed: 2, inserted: '' },
  ]);

  doc.applyBatch(outcome.edits);
  assertEqual(doc.text(), 'a\nb\n  c\n');
});

test('indentSelection prefixes each touched line with a tab', () => {
  const doc = new TextDocument('a\nb\n');
  const sel = new MultiRangeSelection([{ anchor: 0, focus: 3 }]);

  const outcome = indentSelection(doc, sel, 'tab');
  assertDeepEqual(outcome.edits, [
    { start: 0, removed: 0, inserted: '\t' },
    { start: 2, removed: 0, inserted: '\t' },
  ]);

  doc.applyBatch(outcome.edits);
  assertEqual(doc.text(), '\ta\n\tb\n');
});

await run();
