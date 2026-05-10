import type { VibrancyCapabilities, WindowOptions } from 'fxe';
import { Window } from 'fxe';
import { assert, assertEqual, run, test } from './ts_harness.ts';

const hiddenWindowOptions: WindowOptions = {
  title: 'bind-window-vibrancy-capabilities-test',
  width: 96,
  height: 72,
  visible: false,
};

function withHiddenWindow(fn: (win: Window) => void): void {
  const win = new Window(hiddenWindowOptions);
  try {
    fn(win);
  } finally {
    win.close();
  }
}

function assertCapabilitiesShape(caps: VibrancyCapabilities): void {
  assertEqual(typeof caps.supported, 'boolean');
  assertEqual(typeof caps.mica, 'boolean');
  assertEqual(typeof caps.acrylic, 'boolean');
  assertEqual(typeof caps.tabbed, 'boolean');
  assertEqual(typeof caps.blurBehind, 'boolean');
  assertEqual(typeof caps.darkMode, 'boolean');
  assertEqual(typeof caps.systemAccent, 'boolean');
}

test('Window vibrancyCapabilities exposes the documented boolean shape', () => {
  withHiddenWindow((win) => {
    const caps = win.vibrancyCapabilities();
    assertCapabilitiesShape(caps);

    if (process.platform === 'darwin') {
      assertEqual(caps.supported, true);
      assertEqual(caps.mica, false);
      assertEqual(caps.acrylic, false);
      assertEqual(caps.tabbed, true);
      assertEqual(caps.blurBehind, true);
      assertEqual(caps.darkMode, true);
      assertEqual(caps.systemAccent, true);
    } else if (process.platform === 'linux') {
      assertEqual(caps.supported, false);
      assertEqual(caps.mica, false);
      assertEqual(caps.acrylic, false);
      assertEqual(caps.tabbed, false);
      assertEqual(caps.blurBehind, false);
      assertEqual(caps.darkMode, false);
      assertEqual(caps.systemAccent, false);
    } else {
      assertEqual(
        caps.supported,
        caps.mica || caps.acrylic || caps.tabbed || caps.blurBehind,
        'Windows supported should reflect available vibrancy backends',
      );
    }

    assertEqual(win.setVibrancy('sidebar'), caps.supported);
    assertEqual(win.setVibrancy(null), caps.supported);
  });
});

await run();
