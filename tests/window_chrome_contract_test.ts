import { assert, assertEqual, assertThrows, run, test } from './ts_harness.ts';

const unsupportedChromePattern = /unsupported|not implemented/i;
const supportsMacChrome = process.platform === 'darwin';
const supportsWinChrome = process.platform === 'win32';
const supportsTrafficLightPosition = supportsMacChrome || supportsWinChrome;
const supportsWindowControlsOverlay = supportsWinChrome;

test('invisible App window supports chrome API contract and closes normally', () => {
  const beforeWindows = App.windows();
  assert(Array.isArray(beforeWindows), 'App.windows should return an array');
  const win = App.openWindow({
    title: 'window-chrome-contract-test',
    width: 160,
    height: 100,
    visible: false,
  });

  try {
    assert(win instanceof Window, 'App.openWindow should return a Window');
    assertEqual(win.isVisible(), false, 'contract test must not require a visible window');
    assertEqual(win.shouldClose(), false, 'new hidden window should start open');
    const vibrancyCapabilities = win.vibrancyCapabilities();

    for (const style of ['default', 'hidden', 'hiddenInset', 'customButtons'] as const) {
      win.setTitleBarStyle(style);
    }
    if (supportsTrafficLightPosition) {
      win.setTrafficLightPosition(12, 8);
    } else {
      assertThrows(() => win.setTrafficLightPosition(12, 8), unsupportedChromePattern);
    }
    assertEqual(win.setWindowControlsOverlay(false), supportsWindowControlsOverlay);
    assertEqual(win.setWindowControlsOverlay(true), supportsWindowControlsOverlay);
    assertEqual(win.setVibrancy('sidebar'), vibrancyCapabilities.supported);
    assertEqual(win.setVibrancy(null), vibrancyCapabilities.supported);
    assertEqual(
      win.setBlurBehind(true),
      supportsMacChrome || process.platform === 'linux' || vibrancyCapabilities.blurBehind,
    );
    assertEqual(
      win.setBlurBehind(false),
      supportsMacChrome ||
        supportsWinChrome ||
        process.platform === 'linux' ||
        vibrancyCapabilities.blurBehind,
    );
    win.setDragRegion([[0, 0, 80, 28]]);
    win.setDragRegion([{ x: 4, y: 4, width: 32, height: 16 }]);
    win.setDragRegion([]);

    win.setVisible(false);
    assertEqual(win.isVisible(), false, 'chrome calls should not force hidden windows visible');

    const windows = App.windows();
    assert(Array.isArray(windows), 'App.windows should return an array');
    assert(
      windows.length >= beforeWindows.length,
      'chrome calls should not remove existing App window registry entries',
    );

    win.close();
    assertEqual(win.shouldClose(), true, 'hidden chrome test window should still close');
  } finally {
    win.close();
  }
});

await run();
