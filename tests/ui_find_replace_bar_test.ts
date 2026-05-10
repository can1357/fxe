import {
  applyFindReplaceEdits,
  buildReplaceAllEdits,
  buildReplaceCurrentEdit,
  dedupeRegexFlags,
  type FindReplaceActiveMatch,
  type FindReplaceSearchState,
  findNextIndexAfterReplacement,
  getActiveMatch,
  nextActiveIndex,
  prevActiveIndex,
  resolveFindReplaceState,
} from '../packages/fxe-ui/src/components/find_replace_logic.ts';
import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

type DriverOptions = {
  initialQuery?: string;
  initialReplacement?: string;
  caseSensitive?: boolean;
  useRegex?: boolean;
  regexFlags?: string;
  searchDeadlineMs?: number;
  searchMaxMatches?: number;
};

function createDriver(text: string, options: DriverOptions = {}) {
  const doc = new TextDocument(text);
  let query = options.initialQuery ?? '';
  let replacement = options.initialReplacement ?? '';
  let caseSensitive = options.caseSensitive ?? false;
  let useRegex = options.useRegex ?? false;
  let regexFlags = options.regexFlags ?? '';
  let activeIndex = 0;
  let cache: FindReplaceSearchState | null = null;
  const activeChanges: Array<FindReplaceActiveMatch | null> = [];
  const replacedCounts: number[] = [];

  const resolve = (): FindReplaceSearchState => {
    cache = resolveFindReplaceState(
      doc,
      {
        query,
        caseSensitive,
        useRegex,
        regexFlags,
        searchDeadlineMs: options.searchDeadlineMs,
        searchMaxMatches: options.searchMaxMatches,
      },
      cache,
    );
    return cache;
  };

  const emit = (): FindReplaceActiveMatch | null => {
    const state = resolve();
    activeIndex = state.ranges.length === 0 ? 0 : Math.min(activeIndex, state.ranges.length - 1);
    const active = getActiveMatch(state, activeIndex);
    activeChanges.push(active);
    return active;
  };

  return {
    doc,
    activeChanges,
    replacedCounts,
    state: () => resolve(),
    setQuery(value: string) {
      query = value;
      cache = null;
      activeIndex = 0;
      return emit();
    },
    setReplacement(value: string) {
      replacement = value;
    },
    setCaseSensitive(value: boolean) {
      caseSensitive = value;
      cache = null;
      activeIndex = 0;
      return emit();
    },
    setUseRegex(value: boolean) {
      useRegex = value;
      cache = null;
      activeIndex = 0;
      return emit();
    },
    setRegexFlags(value: string) {
      regexFlags = value;
      cache = null;
      activeIndex = 0;
      return emit();
    },
    next() {
      const state = resolve();
      if (state.ranges.length > 0) activeIndex = nextActiveIndex(activeIndex, state.ranges.length);
      return emit();
    },
    prev() {
      const state = resolve();
      if (state.ranges.length > 0) activeIndex = prevActiveIndex(activeIndex, state.ranges.length);
      return emit();
    },
    replaceCurrent() {
      const before = resolve();
      const edit = buildReplaceCurrentEdit(before, activeIndex, replacement);
      if (!edit) {
        emit();
        return 0;
      }
      applyFindReplaceEdits(doc, [edit]);
      cache = null;
      const after = resolve();
      activeIndex = findNextIndexAfterReplacement(after, edit.start, replacement);
      replacedCounts.push(1);
      emit();
      return 1;
    },
    replaceAll() {
      const before = resolve();
      const edits = buildReplaceAllEdits(before, replacement);
      if (edits.length === 0) {
        emit();
        return 0;
      }
      applyFindReplaceEdits(doc, edits);
      cache = null;
      const after = resolve();
      activeIndex = after.ranges.length === 0 ? 0 : Math.min(activeIndex, after.ranges.length - 1);
      replacedCounts.push(edits.length);
      emit();
      return edits.length;
    },
    externalApply(edits: Array<{ start: number; removed: number; inserted: string }>) {
      doc.applyBatch(edits);
      cache = null;
      return emit();
    },
  };
}

test('literal find returns ranges and navigation wraps', () => {
  const driver = createDriver('foo bar foo bar foo');
  assertDeepEqual(driver.setQuery('foo'), { start: 0, end: 3, index: 0, total: 3 });
  assertDeepEqual(driver.state().ranges, [
    { start: 0, end: 3 },
    { start: 8, end: 11 },
    { start: 16, end: 19 },
  ]);
  assertDeepEqual(driver.next(), { start: 8, end: 11, index: 1, total: 3 });
  assertDeepEqual(driver.next(), { start: 16, end: 19, index: 2, total: 3 });
  assertDeepEqual(driver.next(), { start: 0, end: 3, index: 0, total: 3 });
  assertDeepEqual(driver.prev(), { start: 16, end: 19, index: 2, total: 3 });
});

test('case-insensitive literal search uses the bound TextDocument API', () => {
  const driver = createDriver('Hello hello HELLO');
  assertEqual(driver.setQuery('hello')?.total, 3);
  assertEqual(driver.setCaseSensitive(true)?.total, 1);
  assertDeepEqual(driver.state().ranges, [{ start: 6, end: 11 }]);
});

test('replace-current and replace-all update text and report counts', () => {
  const driver = createDriver('one two one two', {
    initialQuery: 'two',
    initialReplacement: 'TWO',
  });
  assertDeepEqual(driver.setQuery('two'), { start: 4, end: 7, index: 0, total: 2 });
  assertEqual(driver.replaceCurrent(), 1);
  assertEqual(driver.doc.text(), 'one TWO one two');
  assertDeepEqual(driver.replacedCounts, [1]);
  driver.setReplacement('2');
  assertEqual(driver.replaceAll(), 2);
  assertEqual(driver.doc.text(), 'one 2 one 2');
  assertDeepEqual(driver.replacedCounts, [1, 2]);
});

test('regex flags are deduped, g is enforced, and invalid regex is reported', () => {
  assertEqual(dedupeRegexFlags('ggiimg'), 'gim');
  const driver = createDriver('alpha ALPHA beta', {
    initialQuery: 'alpha',
    useRegex: true,
    regexFlags: 'ggi',
  });
  assertEqual(driver.setUseRegex(true)?.total, 2);
  const invalid = createDriver('abc', { initialQuery: '(' });
  invalid.setUseRegex(true);
  assertEqual(invalid.state().error !== null, true);
  assertDeepEqual(invalid.state().ranges, []);
});

test('cached matches are invalidated after external document edits', () => {
  const doc = new TextDocument('dog cat');
  const first = resolveFindReplaceState(doc, { query: 'dog' });
  assertDeepEqual(first.ranges, [{ start: 0, end: 3 }]);
  doc.applyBatch([{ start: 4, removed: 3, inserted: 'dog' }]);
  const second = resolveFindReplaceState(doc, { query: 'dog' }, first);
  assertEqual(second.revision > first.revision, true);
  assertDeepEqual(second.ranges, [
    { start: 0, end: 3 },
    { start: 4, end: 7 },
  ]);
});

test('active match payload tracks each navigation step', () => {
  const driver = createDriver('aa bb aa bb aa');
  driver.setQuery('aa');
  driver.next();
  driver.prev();
  assertDeepEqual(driver.activeChanges, [
    { start: 0, end: 2, index: 0, total: 3 },
    { start: 6, end: 8, index: 1, total: 3 },
    { start: 0, end: 2, index: 0, total: 3 },
  ]);
});

test('empty queries clear matches', () => {
  const driver = createDriver('hello');
  assertEqual(driver.setQuery(''), null);
  assertDeepEqual(driver.state().ranges, []);
});

await run();
