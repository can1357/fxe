import type { MonitorChangeListener, MonitorEventName, MonitorInfo, WindowDisposer } from 'fxe';
import { Monitors } from 'fxe';
import { assert, assertEqual, test } from './ts_harness.ts';

function assertMonitorShape(mon: MonitorInfo): void {
  assertEqual(typeof mon.name, 'string');
  for (const key of [
    'x',
    'y',
    'width',
    'height',
    'workX',
    'workY',
    'workWidth',
    'workHeight',
    'scaleX',
    'scaleY',
    'refreshHz',
  ] as const) {
    assertEqual(typeof mon[key], 'number', `monitor.${key} should be numeric`);
  }
  assertEqual(typeof mon.primary, 'boolean');
}

test('Monitors change listeners expose disposer and support targeted removal', () => {
  const seen: string[] = [];
  const handler: MonitorChangeListener = () => {
    seen.push('change');
  };

  const dispose: WindowDisposer = Monitors.on('change', handler);
  assertEqual(typeof dispose, 'function');

  Monitors.off('change', handler);
  Monitors.off('change', handler);

  const disposeAgain = Monitors.on('change', handler);
  disposeAgain();
  disposeAgain();

  assertEqual(seen.length, 0);
});

test('Monitors change listeners support remove-all and preserve query methods', () => {
  const eventName: MonitorEventName = 'change';
  const handlerA: MonitorChangeListener = () => {};
  const handlerB: MonitorChangeListener = () => {};

  const disposeA = Monitors.on(eventName, handlerA);
  const disposeB = Monitors.on(eventName, handlerB);
  Monitors.off(eventName);
  Monitors.off(eventName);
  disposeA();
  disposeB();

  const primary = Monitors.primary();
  assertMonitorShape(primary);

  const monitors = Monitors.list();
  assert(Array.isArray(monitors), 'Monitors.list should return an array');
  for (const monitor of monitors) {
    assertMonitorShape(monitor);
  }
});
