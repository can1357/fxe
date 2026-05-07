import { assertEqual, test } from './ts_harness.ts';

const obscureAccelerator = 'Ctrl+Alt+Shift+F12';

if (
  (globalThis as typeof globalThis & { __FXE_TYPECHECK_ONLY__?: boolean })
    .__FXE_TYPECHECK_ONLY__ === true
) {
  const registered: boolean = globalShortcut.register(obscureAccelerator, () => {
    // Type-only callback coverage; this block must never execute.
  });
  assertEqual(typeof registered, 'boolean');
  globalShortcut.unregister(obscureAccelerator);
}

test('globalShortcut exposes expected functions', () => {
  assertEqual(typeof globalShortcut.register, 'function');
  assertEqual(typeof globalShortcut.unregister, 'function');
  assertEqual(typeof globalShortcut.unregisterAll, 'function');
});

test('globalShortcut rejects invalid accelerators without registering', () => {
  assertEqual(
    globalShortcut.register('', () => {
      throw new Error('empty accelerator callback should not run');
    }),
    false,
  );

  assertEqual(
    globalShortcut.register('NotARealShortcut', () => {
      throw new Error('invalid accelerator callback should not run');
    }),
    false,
  );

  globalShortcut.unregister('');
  globalShortcut.unregister('NotARealShortcut');
  globalShortcut.unregisterAll();
});

test('globalShortcut can unregister safely', () => {
  let registered = false;
  try {
    registered = globalShortcut.register(obscureAccelerator, () => {
      throw new Error('shortcut callback should not run during registration test');
    });
    assertEqual(typeof registered, 'boolean');
  } finally {
    globalShortcut.unregister(obscureAccelerator);
    globalShortcut.unregisterAll();
  }
});
