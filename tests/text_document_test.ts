// Native TextDocument: piece-tree backed editor buffer.

import { assert, assertEqual, test } from './ts_harness.ts';

test('empty document has zero length and one line', () => {
  const d = new TextDocument();
  assertEqual(d.length(), 0);
  assertEqual(d.lineCount(), 1);
  assertEqual(d.text(), '');
  assertEqual(d.revision(), 0);
});

test('seed text loads correctly', () => {
  const seed = 'hello\nworld\n!';
  const d = new TextDocument(seed);
  assertEqual(d.length(), seed.length);
  assertEqual(d.lineCount(), 3);
  assertEqual(d.text(), seed);
  assertEqual(d.lineText(0), 'hello');
  assertEqual(d.lineText(1), 'world');
  assertEqual(d.lineText(2), '!');
});

test('slice returns substring', () => {
  const d = new TextDocument('abcdefgh');
  assertEqual(d.slice(0, 3), 'abc');
  assertEqual(d.slice(2, 6), 'cdef');
  assertEqual(d.slice(5), 'fgh');
  assertEqual(d.slice(0, 0), '');
});

test('replace inserts and bumps revision', () => {
  const d = new TextDocument('hello world');
  const r = d.replace(5, 6, ', great ');
  assertEqual(d.text(), 'hello, great world');
  assertEqual(r.start, 5);
  assertEqual(r.removed, 1);
  assertEqual(r.deleted, ' ');
  assertEqual(r.inserted, ', great ');
  assertEqual(d.revision(), 1);
});

test('replace at end appends', () => {
  const d = new TextDocument('abc');
  d.replace(3, 3, 'def');
  assertEqual(d.text(), 'abcdef');
});

test('replace at start prepends', () => {
  const d = new TextDocument('def');
  d.replace(0, 0, 'abc');
  assertEqual(d.text(), 'abcdef');
});

test('pure delete works', () => {
  const d = new TextDocument('abcdef');
  const r = d.replace(2, 4, '');
  assertEqual(d.text(), 'abef');
  assertEqual(r.deleted, 'cd');
  assertEqual(r.inserted, '');
});

test('lineToOffset / offsetToLine round trip', () => {
  const d = new TextDocument('aaa\nbbb\nccc\nddd');
  assertEqual(d.lineToOffset(0), 0);
  assertEqual(d.lineToOffset(1), 4);
  assertEqual(d.lineToOffset(2), 8);
  assertEqual(d.lineToOffset(3), 12);
  assertEqual(d.offsetToLine(0), 0);
  assertEqual(d.offsetToLine(3), 0);
  assertEqual(d.offsetToLine(4), 1);
  assertEqual(d.offsetToLine(8), 2);
  assertEqual(d.offsetToLine(12), 3);
  assertEqual(d.offsetToLine(15), 3);
});

test('lineRange trims trailing newline', () => {
  const d = new TextDocument('abc\ndef\n');
  const r0 = d.lineRange(0);
  assertEqual(r0.start, 0);
  assertEqual(r0.end, 3);
  const r1 = d.lineRange(1);
  assertEqual(r1.start, 4);
  assertEqual(r1.end, 7);
});

test('lineColToOffset clamps col', () => {
  const d = new TextDocument('hello\nworld');
  assertEqual(d.lineColToOffset(0, 0), 0);
  assertEqual(d.lineColToOffset(0, 3), 3);
  assertEqual(d.lineColToOffset(0, 99), 5); // clamped to end of line 0
  assertEqual(d.lineColToOffset(1, 0), 6);
  assertEqual(d.lineColToOffset(1, 5), 11);
});

test('offsetToLineCol matches lineColToOffset', () => {
  const d = new TextDocument('abc\ndef\nghi');
  for (let off = 0; off <= d.length(); ++off) {
    const lc = d.offsetToLineCol(off);
    const back = d.lineColToOffset(lc.line, lc.col);
    assertEqual(back, off, `offset ${off} round-trip via {${lc.line},${lc.col}}`);
  }
});

test('many edits stay consistent vs reference string', () => {
  const seed = 'The quick brown fox jumps over the lazy dog.\n'.repeat(20);
  const d = new TextDocument(seed);
  let ref = seed;
  // Deterministic LCG so the test is reproducible.
  let s = 0x12345678;
  const rnd = () => {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    return s;
  };
  for (let i = 0; i < 200; ++i) {
    const len = ref.length;
    const start = len > 0 ? rnd() % len : 0;
    const end = start + (rnd() % Math.min(8, len - start + 1));
    const insertLen = rnd() % 6;
    let ins = '';
    for (let k = 0; k < insertLen; ++k) {
      ins += String.fromCharCode(0x61 + (rnd() % 26));
    }
    if (rnd() % 7 === 0) ins += '\n';
    d.replace(start, end, ins);
    ref = ref.slice(0, start) + ins + ref.slice(end);
  }
  assertEqual(d.length(), ref.length);
  assertEqual(d.text(), ref);
  // Line agreement.
  const refLines = ref.split('\n').length;
  assertEqual(d.lineCount(), refLines);
});

test('subscribe fires on edit', () => {
  const d = new TextDocument('abc');
  const seen: string[] = [];
  const id = d.subscribe((edits) => {
    for (const e of edits) seen.push(`${e.start}:${e.removed}:${e.inserted}`);
  });
  d.replace(1, 2, 'XX');
  assertEqual(seen.length, 1);
  assertEqual(seen[0], '1:1:XX');
  d.unsubscribe(id);
  d.replace(0, 0, 'Y');
  assertEqual(seen.length, 1, 'unsubscribed listener should not fire');
});

test('applyBatch runs N edits as one revision', () => {
  const d = new TextDocument('aaaa');
  const before = d.revision();
  d.applyBatch([
    { start: 0, removed: 0, inserted: 'X' },
    { start: 2, removed: 0, inserted: 'Y' },
    { start: 4, removed: 0, inserted: 'Z' },
  ]);
  assertEqual(d.text(), 'XaaYaaZ');
  assertEqual(d.revision(), before + 1, 'one revision per batch');
});

test('applyBatch rejects overlapping edits', () => {
  const d = new TextDocument('abcdef');
  let threw = false;
  try {
    d.applyBatch([
      { start: 0, removed: 3, inserted: 'X' },
      { start: 2, removed: 0, inserted: 'Y' },
    ]);
  } catch {
    threw = true;
  }
  assert(threw, 'overlapping edits must throw');
});

test('searchLiteral finds matches', () => {
  const d = new TextDocument('the cat sat on the mat');
  const r = d.searchLiteral('the');
  assertEqual(r.length, 2);
  assertEqual(r[0].start, 0);
  assertEqual(r[0].end, 3);
  assertEqual(r[1].start, 15);
  assertEqual(r[1].end, 18);
});

test('searchLiteral honours caseInsensitive', () => {
  const d = new TextDocument('Hello hello HELLO');
  const ci = d.searchLiteral('hello', { caseInsensitive: true });
  assertEqual(ci.length, 3);
  const cs = d.searchLiteral('hello');
  assertEqual(cs.length, 1);
});

test('searchLiteral honours from + limit', () => {
  const d = new TextDocument('aa aa aa aa');
  const r = d.searchLiteral('aa', { from: 3, limit: 2 });
  assertEqual(r.length, 2);
  assertEqual(r[0].start, 3);
  assertEqual(r[1].start, 6);
});

test('utf-16 surrogate pair counts as two code units', () => {
  // 𝕏 (U+1D54F) — single codepoint, two UTF-16 code units.
  const d = new TextDocument('a𝕏b');
  assertEqual(d.length(), 4);
  assertEqual(d.charCodeAt(0), 'a'.charCodeAt(0));
  assertEqual(d.charCodeAt(1), 0xd835);
  assertEqual(d.charCodeAt(2), 0xdd4f);
  assertEqual(d.charCodeAt(3), 'b'.charCodeAt(0));
});
