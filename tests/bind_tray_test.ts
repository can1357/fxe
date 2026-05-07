import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

test('Tray constructor rejects calls without new', () => {
  const callTray = Tray as unknown as (...args: unknown[]) => unknown;

  assertThrows(() => {
    callTray();
  }, /new/);

  assertThrows(() => {
    callTray(123);
  }, /new/);
});

test('Tray prototype exposes menu and destroy methods', () => {
  assertEqual(typeof Tray, 'function');
  assertEqual(typeof Tray.prototype.setMenu, 'function');
  assertEqual(typeof Tray.prototype.destroy, 'function');
});

test('Tray mutable surface is callable and returns typed results', () => {
  const tray = new Tray('', '');
  try {
    const initialImageResult = tray.setImage('');
    assertEqual(typeof initialImageResult, 'boolean');

    const disposer = tray.on('click', () => {}) as unknown;
    if (process.platform === 'darwin') {
      assertEqual(typeof disposer, 'function');
    }
    if (typeof disposer === 'function') {
      disposer();
    } else {
      assert(
        process.platform !== 'darwin',
        'Tray.on should return a disposer function on supported platforms',
      );
    }

    assertEqual(typeof tray.setTitle('hi'), 'boolean');
    assertEqual(typeof tray.setToolTip('tip'), 'boolean');
    assertEqual(typeof tray.setImage('/tmp/nonexistent'), 'boolean');
  } finally {
    tray.destroy();
  }
});

if (
  (globalThis as typeof globalThis & { __FXE_TYPECHECK_ONLY__?: boolean })
    .__FXE_TYPECHECK_ONLY__ === true
) {
  const tray = new Tray('/tmp/fxe-tray-icon.png', 'FXE tray');
  tray.setMenu([
    { id: 'open', label: 'Open', enabled: true },
    { type: 'separator' },
    {
      id: 'more',
      label: 'More',
      type: 'submenu',
      submenu: [{ id: 'quit', label: 'Quit', accelerator: 'CmdOrCtrl+Q' }],
    },
  ]);
  tray.destroy();
}
