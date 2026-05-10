import { MultiRangeSelection } from 'fxe-doc';

import {
  applyEditsAtRanges,
  autocloseTyped,
  findMatchingBracket,
} from '../packages/fxe-ui/src/components/editable_area_logic.ts';
import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('autocloseTyped inserts parens at a collapsed cursor', () => {
  const doc = new TextDocument('ab');
  const sel = MultiRangeSelection.cursor(1);

  const outcome = autocloseTyped(doc, sel, '(');
  assertEqual(outcome.handled, true);
  assertDeepEqual(outcome.edits, [{ start: 1, removed: 0, inserted: '()' }]);

  doc.applyBatch(outcome.edits);
  assertEqual(doc.text(), 'a()b');
  assertDeepEqual(outcome.nextSel.ranges, [{ anchor: 2, focus: 2 }]);
});

test('autocloseTyped wraps a non-empty selection with parens', () => {
  const doc = new TextDocument('abcd');
  const sel = new MultiRangeSelection([{ anchor: 1, focus: 3 }]);

  const outcome = autocloseTyped(doc, sel, '(');
  assertEqual(outcome.handled, true);
  assertDeepEqual(outcome.edits, [{ start: 1, removed: 2, inserted: '(bc)' }]);

  doc.applyBatch(outcome.edits);
  assertEqual(doc.text(), 'a(bc)d');
  assertDeepEqual(outcome.nextSel.ranges, [{ anchor: 2, focus: 4 }]);
});

test('autocloseTyped steps over an existing closing paren', () => {
  const doc = new TextDocument('()');
  const sel = MultiRangeSelection.cursor(1);

  const outcome = autocloseTyped(doc, sel, ')');
  assertEqual(outcome.handled, true);
  assertDeepEqual(outcome.edits, []);
  assertDeepEqual(outcome.nextSel.ranges, [{ anchor: 2, focus: 2 }]);
  assertEqual(doc.text(), '()');
});

test('autocloseTyped only autocloses quotes at boundaries', () => {
  const inlineDoc = new TextDocument('word');
  const inlineSel = MultiRangeSelection.cursor(2);
  const inlineOutcome = autocloseTyped(inlineDoc, inlineSel, '"');
  assertEqual(inlineOutcome.handled, false);

  const fallback = applyEditsAtRanges(inlineDoc, inlineSel, '"', 'type');
  inlineDoc.applyBatch(fallback.edits);
  assertEqual(inlineDoc.text(), 'wo"rd');

  const boundaryDoc = new TextDocument(' word');
  const boundarySel = MultiRangeSelection.cursor(0);
  const boundaryOutcome = autocloseTyped(boundaryDoc, boundarySel, '"');
  assertEqual(boundaryOutcome.handled, true);
  assertDeepEqual(boundaryOutcome.edits, [{ start: 0, removed: 0, inserted: '""' }]);

  boundaryDoc.applyBatch(boundaryOutcome.edits);
  assertEqual(boundaryDoc.text(), '"" word');
  assertDeepEqual(boundaryOutcome.nextSel.ranges, [{ anchor: 1, focus: 1 }]);
});

test('findMatchingBracket walks nested pairs', () => {
  const doc = new TextDocument('({[]})');
  assertDeepEqual(findMatchingBracket(doc, 0), { open: 0, close: 5 });
  assertDeepEqual(findMatchingBracket(doc, 4), { open: 1, close: 4 });
});

test('findMatchingBracket returns null for an unbalanced bracket', () => {
  const doc = new TextDocument('(()');
  assertEqual(findMatchingBracket(doc, 0), null);
});

test('BracketContextProvider suppression disables autoclose and matching', () => {
  const provider = { isStringOrComment: (off: number) => off === 0 };

  const typed = autocloseTyped(new TextDocument(''), MultiRangeSelection.cursor(0), '(', provider);
  assertEqual(typed.handled, false);

  const doc = new TextDocument('()');
  assertEqual(findMatchingBracket(doc, 0, provider), null);
});

await run();
