import { assert, assertEqual, test } from './ts_harness.ts';

// Markdown.highlight wraps fxe::highlight (tree-sitter). Guard against
// builds without tree-sitter: in that case `highlightLanguages()` returns
// an empty array and `highlight()` returns null. Asserts run on whichever
// path applies, so the test exercises both build configurations.

const supported = Markdown.highlightLanguages();

test('highlightLanguages returns an array', () => {
  assert(Array.isArray(supported));
});

test('every supported language tokenizes some source', () => {
  for (const lang of supported) {
    const sample =
      lang === 'json'
        ? '{"a": 1, "b": null}'
        : 'const x: number = 1; // hi';
    const r = Markdown.highlight(sample, lang);
    assert(r !== null, `highlight(${lang}) returned null`);
    assertEqual(typeof r!.language, 'string');
    assert(Array.isArray(r!.tokens));
    assert(r!.tokens.length > 0, `${lang} produced no tokens`);
    // Tokens must be sorted, non-overlapping, in-bounds.
    let cursor = 0;
    for (const tok of r!.tokens) {
      assert(tok.end > tok.start, 'empty token');
      assert(tok.start >= cursor, 'overlapping or unsorted token');
      assert(tok.end <= sample.length, 'token end out of bounds');
      assertEqual(typeof tok.name, 'string');
      assert(tok.name.length > 0, 'capture name empty');
      cursor = tok.end;
    }
  }
});

test('typescript highlights pull keyword + string + number', () => {
  if (!supported.includes('typescript')) return;
  const r = Markdown.highlight('const s = "hello"; const n = 42;', 'typescript');
  assert(r);
  const names = new Set(r!.tokens.map((t) => t.name));
  assert(names.has('keyword'), `expected keyword in ${[...names].join(',')}`);
  assert(names.has('string'), `expected string in ${[...names].join(',')}`);
  assert(names.has('number'), `expected number in ${[...names].join(',')}`);
});

test('json highlights tag pair keys distinctly', () => {
  if (!supported.includes('json')) return;
  const r = Markdown.highlight('{"name": "fxe", "v": 1}', 'json');
  assert(r);
  const names = new Set(r!.tokens.map((t) => t.name));
  assert(names.has('property'));
  assert(names.has('string'));
  assert(names.has('number'));
});

test('aliases route to the canonical grammar', () => {
  if (!supported.includes('typescript')) return;
  const r = Markdown.highlight('const x = 1;', 'ts');
  assert(r);
  assertEqual(r!.language, 'typescript');
});

test('unknown languages return null', () => {
  const r = Markdown.highlight('whatever', 'klingon');
  assertEqual(r, null);
});

test('non-string args throw TypeError', () => {
  let threw = false;
  try {
    // @ts-expect-error: missing language
    Markdown.highlight('x');
  } catch (e) {
    threw = e instanceof TypeError;
  }
  assert(threw);
});
