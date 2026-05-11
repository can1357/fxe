import type { WindowOptions } from 'fxe';
import { Window } from 'fxe';
import { assertEqual, assertThrows, test } from './ts_harness.ts';

const hiddenWindowOptions: WindowOptions = {
  width: 96,
  height: 64,
  x: 0,
  y: 0,
  visible: false,
  resizable: true,
  decorated: false,
  transparent: false,
  alwaysOnTop: false,
  maximized: false,
  minWidth: 32,
  minHeight: 32,
  maxWidth: 512,
  maxHeight: 512,
  title: 'bind-window-linux-chrome-test',
};

test('Window.setGtkFrameExtents validates extent objects', () => {
  const win = new Window(hiddenWindowOptions);
  try {
    assertEqual(
      typeof win.setGtkFrameExtents({ left: 28, right: 28, top: 28, bottom: 28 }),
      'boolean',
    );
    assertEqual(typeof win.setGtkFrameExtents({ left: 0, right: 0, top: 0, bottom: 0 }), 'boolean');

    assertThrows(
      // @ts-expect-error: invalid shape under test
      () => win.setGtkFrameExtents(null),
      /setGtkFrameExtents/,
    );
    assertThrows(
      // @ts-expect-error: invalid shape under test
      () => win.setGtkFrameExtents([28, 28, 28, 28]),
      /setGtkFrameExtents/,
    );
    assertThrows(
      // @ts-expect-error: invalid shape under test
      () => win.setGtkFrameExtents({ left: 28, right: 28, top: 28 }),
      /extents\.bottom must be a number/,
    );
    assertThrows(
      // @ts-expect-error: invalid shape under test
      () => win.setGtkFrameExtents({ left: '28', right: 28, top: 28, bottom: 28 }),
      /extents\.left must be a number/,
    );
  } finally {
    win.close();
  }
});
