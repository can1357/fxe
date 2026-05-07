import { assertEqual, assertThrows, test } from './ts_harness.ts';

test('Font.builtin returns the default font id', () => {
  assertEqual(Font.builtin('default'), 0);
});

test('Font.dispose accepts the default font id', () => {
  Font.dispose(Font.builtin('default'));
});

test('Font.builtin rejects unknown builtin names', () => {
  assertThrows(() => Font.builtin('missing' as never), /expected 'default'/);
});

test('Font.load reports missing files', () => {
  assertThrows(() => Font.load('__fxe_missing_font__.ttf', 16), /failed to read TTF file/);
});

if (false as boolean) {
  const cb = new CommandBuffer();
  const fontId = Font.builtin('default');
  const bounds: [number, number, number, number] = Primitives.drawText(
    cb,
    [0, 0],
    0,
    'font type check',
    { fontId, size: 12, color: 0xffffffff },
  );
  void bounds;
}
