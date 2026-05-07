import { assert, assertEqual, run, test } from './ts_harness.ts';

test('App.openWindow constructs and registers closable Windows', () => {
  const before = App.windows();
  assert(Array.isArray(before), 'App.windows should return an array');

  const win = App.openWindow({ visible: false });
  const defaultWin = App.openWindow();
  try {
    assert(win instanceof Window, 'App.openWindow should return a Window');
    assert(defaultWin instanceof Window, 'App.openWindow should work with omitted opts');

    const windows = App.windows();
    assert(Array.isArray(windows), 'App.windows should return an array');
    assert(
      windows.length >= before.length + 2,
      'App.openWindow should register windows in the host registry',
    );
    assert(
      windows.some((candidate) => candidate instanceof Window),
      'App.windows should expose Window wrappers',
    );

    win.close();
    defaultWin.close();
    assertEqual(win.shouldClose(), true, 'App.openWindow result should be closable');
    assertEqual(defaultWin.shouldClose(), true, 'default App.openWindow result should be closable');
  } finally {
    win.close();
    defaultWin.close();
  }
});

await run();
