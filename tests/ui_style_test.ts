import { parseColor, STYLE_SHEET_BRAND, StyleSheet, splitStyle } from 'fxe-ui';

import { assert, assertDeepEqual, assertEqual, assertThrows, run, test } from './ts_harness.ts';

test('parseColor accepts canonical numeric rgba', () => {
  assertEqual(parseColor(0x11223344), 0x11223344);
});

test('parseColor accepts rgb hex and adds alpha', () => {
  assertEqual(parseColor('#123456'), 0x123456ff);
});

test('parseColor accepts rgba shorthand hex', () => {
  assertEqual(parseColor('#0f08'), 0x00ff0088);
});

test('parseColor accepts tuple colors', () => {
  assertDeepEqual(parseColor([1, 2, 3, 4]), [1, 2, 3, 4]);
});

test('parseColor rejects malformed hex', () => {
  assertThrows(() => parseColor('#12'), /hex colors/);
});

test('splitStyle separates layout paint and text fields', () => {
  const resolved = splitStyle({
    width: '50%',
    padding: 4,
    backgroundColor: '#ff0000',
    borderColor: [1, 2, 3, 4],
    color: '#00ff00',
    fontSize: 18,
    cursor: 'hand',
  });
  assertDeepEqual(resolved.layout, { width: '50%', padding: 4 });
  assertEqual(resolved.paint.backgroundColor, 0xff0000ff);
  assertDeepEqual(resolved.paint.borderColor, [1, 2, 3, 4]);
  assertEqual(resolved.paint.cursor, 'hand');
  assertEqual(resolved.text.color, 0x00ff00ff);
  assertEqual(resolved.text.fontSize, 18);
});

test('splitStyle routes per-side border paint fields', () => {
  const resolved = splitStyle({
    borderTopWidth: 2,
    borderRightColor: '#ff0000',
    borderStyle: 'dashed',
  });
  assertEqual(resolved.paint.borderTopWidth, 2);
  assertEqual(resolved.paint.borderRightColor, 0xff0000ff);
  assertEqual(resolved.paint.borderStyle, 'dashed');
});

test('splitStyle routes shadow paint fields', () => {
  const resolved = splitStyle({
    shadowColor: '#000',
    shadowOffsetX: 2,
    shadowOffsetY: 4,
    shadowBlur: 8,
    shadowSpread: 1,
  });
  assertEqual(resolved.paint.shadowColor, 0x000000ff);
  assertEqual(resolved.paint.shadowOffsetX, 2);
  assertEqual(resolved.paint.shadowOffsetY, 4);
  assertEqual(resolved.paint.shadowBlur, 8);
  assertEqual(resolved.paint.shadowSpread, 1);
});

test('splitStyle flattens style arrays', () => {
  assertDeepEqual(splitStyle([{ width: 10 }, false, null, { height: 20 }]).layout, {
    width: 10,
    height: 20,
  });
});

test('splitStyle rejects unsupported properties loudly', () => {
  assertThrows(() => splitStyle({ display: 'grid' } as never), /display/);
  assertThrows(() => splitStyle({ unknown: 1 } as never), /unsupported/);
});

test('StyleSheet.create brands styles, preserves identity, and memoizes splitStyle', () => {
  const styles = StyleSheet.create({ card: { width: 10, backgroundColor: 0x01020304 } });
  assertEqual(STYLE_SHEET_BRAND in styles.card, true);
  assert(!Object.isFrozen(styles), 'sheet map extensible for engine use');
  assert(!Object.isFrozen(styles.card), 'entries extensible for symbol side tables');
  assertEqual(styles.card, styles.card);
  const r1 = splitStyle(styles.card);
  const r2 = splitStyle(styles.card);
  assert(r1 === r2, 'splitStyle returns cached result for sheet style ref');
});

await run();
