import { Primitives } from 'fxe';
import { glyphIndexAt, wrapText, xAtGlyphIndex } from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

// Tests rely on a real font so calcText returns non-zero widths.
Font.load('/System/Library/Fonts/Monaco.ttf', 32);

test('wrapText keeps short text on a single line', () => {
  const result = wrapText('hello', { fontSize: 16 }, { maxWidth: 1000 });
  assertEqual(result.lines.length, 1);
  assertEqual(result.lines[0], 'hello');
  assert(result.width > 0, 'width should be measured');
  assert(result.height >= result.lineHeight, 'height should cover one line');
});

test('native text helpers handle ASCII fast path and decline Unicode', () => {
  const ascii = Primitives.wrapTextNative('native text path', 16, 0, 80, undefined, false);
  assert(ascii !== null, 'ASCII text should use native wrapping');
  assert(ascii.lines.length >= 1, 'native wrapper should return at least one line');
  assertEqual(ascii.lines.join(' '), 'native text path');
  assertEqual(Primitives.wrapTextNative('héllo', 16, 0, 80, undefined, false), null);
  assertEqual(Primitives.xAtGlyphIndexNative('héllo', 16, 0, 2), null);
  assertEqual(Primitives.glyphIndexAtNative('héllo', 16, 0, 12), null);
});

test('fxe-ui text helpers call native primitive fast paths', () => {
  const originalWrap = Primitives.wrapTextNative;
  const originalX = Primitives.xAtGlyphIndexNative;
  const originalGlyph = Primitives.glyphIndexAtNative;
  let wrapCalls = 0;
  let xCalls = 0;
  let glyphCalls = 0;
  try {
    Primitives.wrapTextNative = (...args) => {
      wrapCalls++;
      return originalWrap(...args);
    };
    Primitives.xAtGlyphIndexNative = (...args) => {
      xCalls++;
      return originalX(...args);
    };
    Primitives.glyphIndexAtNative = (...args) => {
      glyphCalls++;
      return originalGlyph(...args);
    };

    wrapText('native path through fxe-ui', { fontSize: 16 }, { maxWidth: 120 });
    xAtGlyphIndex('native', { fontSize: 16 }, 3);
    glyphIndexAt('native', { fontSize: 16 }, 12);
  } finally {
    Primitives.wrapTextNative = originalWrap;
    Primitives.xAtGlyphIndexNative = originalX;
    Primitives.glyphIndexAtNative = originalGlyph;
  }
  assertEqual(wrapCalls, 1);
  assertEqual(xCalls, 1);
  assertEqual(glyphCalls, 1);
});

test('wrapText breaks on word boundaries when constrained', () => {
  const text = 'A compact native login surface tuned for keyboard-first workflows.';
  const wide = wrapText(text, { fontSize: 13 }, { maxWidth: 10_000 });
  const narrow = wrapText(text, { fontSize: 13 }, { maxWidth: 110 });
  assertEqual(wide.lines.length, 1);
  assert(narrow.lines.length > 1, 'narrow constraint should produce multiple lines');
  assertEqual(
    narrow.lines.join(' ').replace(/\s+/g, ' '),
    text.replace(/\s+/g, ' '),
    'wrapped lines should reproduce the input prose',
  );
  assert(narrow.height > wide.height, 'wrapped text should be taller than single line');
});

test('wrapText respects explicit lineHeight and reports n*lineHeight', () => {
  const result = wrapText('one two three', { fontSize: 13, lineHeight: 20 }, { maxWidth: 30 });
  assert(result.lines.length >= 2, 'should produce multiple lines');
  assertEqual(result.lineHeight, 20);
  assertEqual(result.height, 20 * result.lines.length);
});

test('wrapText preserves explicit newlines', () => {
  const result = wrapText('first line\nsecond line', { fontSize: 16 }, { maxWidth: 10_000 });
  assertEqual(result.lines.length, 2);
  assertEqual(result.lines[0], 'first line');
  assertEqual(result.lines[1], 'second line');
});

test('wrapText leaves long unbreakable words alone by default', () => {
  const result = wrapText('supercalifragilisticexpialidocious', { fontSize: 16 }, { maxWidth: 10 });
  assertEqual(result.lines.length, 1);
  assertEqual(result.lines[0], 'supercalifragilisticexpialidocious');
});

test('wrapText breaks long words when breakWords is set', () => {
  const result = wrapText(
    'supercalifragilisticexpialidocious',
    { fontSize: 16 },
    { maxWidth: 30, breakWords: true },
  );
  assert(result.lines.length > 1, 'breakWords should split long words');
  assertEqual(result.lines.join(''), 'supercalifragilisticexpialidocious');
});

test('wrapText returns empty single line for empty text', () => {
  const result = wrapText('', { fontSize: 16 });
  assertEqual(result.lines.length, 1);
  assertEqual(result.lines[0], '');
  assertEqual(result.width, 0);
  assert(result.height > 0, 'empty text should still report a baseline line height');
});

test('glyph measurement maps x positions to caret indices', () => {
  const text = 'abcd';
  const style = { fontSize: 16 };
  const x2 = xAtGlyphIndex(text, style, 2);
  assert(x2 > 0, 'index x should advance for visible glyphs');
  assertEqual(xAtGlyphIndex(text, style, -1), 0);
  assertEqual(xAtGlyphIndex(text, style, 99), xAtGlyphIndex(text, style, text.length));
  assertEqual(glyphIndexAt(text, style, x2), 2);
  assertEqual(glyphIndexAt(text, style, -10), 0);
  assertEqual(glyphIndexAt(text, style, Number.POSITIVE_INFINITY), text.length);
});

await run();
