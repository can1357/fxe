import { assert, assertEqual, test } from './ts_harness.ts';

test('powerMonitor exposes expected API shape', () => {
  assert(App.powerMonitor === powerMonitor, 'global powerMonitor should mirror App.powerMonitor');
  assertEqual(typeof powerMonitor.on, 'function');
  assertEqual(typeof powerMonitor.isOnBattery, 'function');
  assertEqual(typeof powerMonitor.isOnline, 'function');
  assertEqual(typeof powerMonitor.systemIdleSeconds, 'function');
});

test('App.power exposes sleep inhibition API shape', () => {
  assert(App.power && typeof App.power === 'object', 'App.power should exist');
  assertEqual(typeof App.power.inhibitSleep, 'function');
});

test('App.recentDocuments exposes expected API shape', () => {
  assert(
    App.recentDocuments && typeof App.recentDocuments === 'object',
    'App.recentDocuments should exist',
  );
  assertEqual(typeof App.recentDocuments.add, 'function');
  assertEqual(typeof App.recentDocuments.list, 'function');
  assertEqual(typeof App.recentDocuments.clear, 'function');
  assert(Array.isArray(App.recentDocuments.list()), 'recentDocuments.list should return an array');
});

test('powerMonitor query methods return primitive values', () => {
  assertEqual(typeof powerMonitor.isOnline(), 'boolean');
  assertEqual(typeof powerMonitor.isOnBattery(), 'boolean');
  const idleSeconds = powerMonitor.systemIdleSeconds();
  assertEqual(typeof idleSeconds, 'number');
  assert(idleSeconds >= 0, 'systemIdleSeconds should not be negative');
});

test('powerMonitor.on returns a disposer', () => {
  const dispose = powerMonitor.on('resume', () => {
    throw new Error('resume callback should not run during contract test');
  });
  assertEqual(typeof dispose, 'function');
  assertEqual(dispose(), undefined);
  assertEqual(dispose(), undefined);
});
