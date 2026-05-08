import type {
  CursorKind,
  MonitorInfo,
  WindowDisposer,
  WindowEventHandler,
  WindowEventMap,
  WindowEventName,
  WindowOptions,
  WindowRunOptions,
} from 'fxe';
import { App, Monitors, Window } from 'fxe';
import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

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
  title: 'bind-window-test',
};

function withHiddenWindow(fn: (win: Window) => void): void {
  const win = new Window(hiddenWindowOptions);
  try {
    fn(win);
  } finally {
    win.close();
  }
}

function assertNumberPair(value: unknown, label: string): asserts value is [number, number] {
  assert(Array.isArray(value), `${label} should return an array`);
  assertEqual(value.length, 2, `${label} should return two values`);
  assertEqual(typeof value[0], 'number', `${label}[0] should be numeric`);
  assertEqual(typeof value[1], 'number', `${label}[1] should be numeric`);
}

function assertBoundsShape(
  value: unknown,
  label: string,
): asserts value is { x: number; y: number; width: number; height: number } {
  assert(value !== null && typeof value === 'object', `${label} should return an object`);
  for (const key of ['x', 'y', 'width', 'height'] as const) {
    assertEqual(
      typeof (value as Record<typeof key, unknown>)[key],
      'number',
      `${label}.${key} should be numeric`,
    );
  }
}

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

test('Window constructor accepts documented hidden options and closes safely', () => {
  withHiddenWindow((win) => {
    assert(win instanceof Window);
    assertEqual(typeof win.shouldClose(), 'boolean');
    assertEqual(win.isVisible(), false);
    win.close();
    assertEqual(win.shouldClose(), true);
  });
});

test('Window constructor requires new and tolerates hidden minimal options', () => {
  assertThrows(() => (Window as unknown as () => Window)(), /new/);

  const defaults = new Window({ visible: false });
  defaults.close();

  const ignored = new Window({ visible: false });
  ignored.close();
});

test('Window isolate modes expose shared vs dedicated runtime ids and validate input', () => {
  const shared = new Window({ visible: false, isolate: 'shared' });
  assertEqual(shared.isolateMode, 'shared');
  assertEqual(shared.isolateId, 0);
  shared.close();

  const ownA = new Window({ visible: false, isolate: 'own' });
  const ownB = new Window({ visible: false, isolate: 'own' });
  assertEqual(ownA.isolateMode, 'own');
  assertEqual(ownB.isolateMode, 'own');
  assert(ownA.isolateId > 0, 'own isolate should expose a dedicated runtime id');
  assert(ownB.isolateId > 0, 'second own isolate should expose a dedicated runtime id');
  assert(ownA.isolateId !== ownB.isolateId, 'own windows should get distinct runtime ids');
  ownA.close();
  ownB.close();

  assertThrows(
    () => new Window({ visible: false, isolate: 'invalid' as 'shared' }),
    /must be 'shared' or 'own'/,
  );
});

test('Window event listener methods validate inputs and return disposers', () => {
  withHiddenWindow((win) => {
    let seen = 0;
    const handler: WindowEventHandler<'resize'> = (ev) => {
      seen += ev.width + ev.height;
    };

    const dispose: WindowDisposer = win.on('resize', handler);
    assertEqual(typeof dispose, 'function');
    dispose();
    dispose();

    win.on('resize', handler);
    win.off('resize', handler);
    win.on('resize', handler);
    win.off('resize');
    win.on('resize', handler);
    win.removeAllListeners('resize');
    win.on('resize', handler);
    win.removeAllListeners();
    assertEqual(seen, 0);

    assertThrows(
      () => win.on('resize', undefined as unknown as WindowEventHandler<'resize'>),
      /on\(event, handler\)/,
    );
    assertThrows(() => win.on('not-an-event' as 'resize', handler), /unknown event/);
    assertThrows(
      () => win.off(undefined as unknown as WindowEventName),
      /off\(event\[, handler\]\)/,
    );
  });
});

test('Window basic methods are callable without running the event loop', () => {
  withHiddenWindow((win) => {
    win.poll();
    win.setTitle('bind-window-test-updated');
    win.setSize(120, 80);
    win.setPosition(10, 20);
    assertNumberPair(win.position(), 'position');
    assertNumberPair(win.framebufferSize(), 'framebufferSize');

    win.setMinSize(64, 48);
    win.setMaxSize(320, 240);
    win.setOpacity(0.85);
    assertEqual(typeof win.opacity(), 'number');
    win.setAlwaysOnTop(false);
    win.setResizable(true);
    win.setDecorated(false);
    win.setVisible(false);
    assertEqual(win.isVisible(), false);

    win.requestRedraw();
    assertEqual(typeof win.takeRedrawRequest(), 'boolean');
    win.setVsync(false);
    win.waitEventsTimeout(0);
  });
});

test('Window cursor, icon, drag region, and fullscreen methods validate safe inputs', () => {
  withHiddenWindow((win) => {
    for (const cursor of ['arrow', 'ibeam', 'crosshair', 'hand', 'hidden'] as CursorKind[]) {
      win.setCursor(cursor);
    }
    win.setCursorVisible(true);
    win.setCursorPos(1, 2);
    assertNumberPair(win.cursorPos(), 'cursorPos');
    win.setCursorLock(false);
    win.setFullscreen(false);
    assertEqual(win.isFullscreen(), false);

    win.setIcon(new Uint8Array([255, 0, 0, 255]), 1, 1);
    win.setIcon(new Uint8ClampedArray([0, 0, 0, 0]), 1, 1);
    win.setDragRegion([[0, 0, 16, 16], [4, 4, 8, 8] as const]);
    win.setDragRegion([{ x: 1, y: 2, width: 3, height: 4 }]);
    win.setDragRegion([]);

    assertThrows(() => win.setCursor('bogus' as CursorKind), /unknown kind/);
    assertThrows(() => win.setCursor(1 as unknown as CursorKind), /expected string/);
    assertThrows(
      () => win.setIcon(new Uint16Array(4) as unknown as Uint8Array, 1, 1),
      /Uint8Array/,
    );
    assertThrows(() => win.setIcon(new Uint8Array(3), 1, 1), /length too small/);
  });
});

test('Window state query and no-op methods are callable on hidden windows', () => {
  withHiddenWindow((win) => {
    assertEqual(typeof win.isFocused(), 'boolean');
    assertEqual(typeof win.isMinimized(), 'boolean');
    assertEqual(typeof win.isMaximized(), 'boolean');
    win.minimize();
    win.restore();
    win.maximize();
    win.restore();
    win.requestAttention();
    win.center();
  });
});

test('Window state getter methods return documented shapes', () => {
  withHiddenWindow((win) => {
    assertEqual(typeof win.title(), 'string');
    assertNumberPair(win.size(), 'size');

    const bounds = win.bounds();
    for (const key of ['x', 'y', 'width', 'height'] as const) {
      assertEqual(typeof bounds[key], 'number', `bounds.${key} should be numeric`);
    }

    assertEqual(typeof win.isResizable(), 'boolean');
    assertEqual(typeof win.isDecorated(), 'boolean');
    assertEqual(typeof win.isAlwaysOnTop(), 'boolean');
    assertEqual(typeof win.isTransparent(), 'boolean');

    const min = win.minSize();
    assert(min === null || Array.isArray(min), 'minSize should be null or an array');
    if (min !== null) assertNumberPair(min, 'minSize');

    const max = win.maxSize();
    assert(max === null || Array.isArray(max), 'maxSize should be null or an array');
    if (max !== null) assertNumberPair(max, 'maxSize');
  });
});

test('Window getter aliases return documented shapes', () => {
  withHiddenWindow((win) => {
    assertEqual(typeof win.getTitle(), 'string');
    assertBoundsShape(win.getBounds(), 'getBounds');

    const min = win.getMinSize();
    assert(min === null || Array.isArray(min), 'getMinSize should be null or an array');
    if (min !== null) assertNumberPair(min, 'getMinSize');

    const max = win.getMaxSize();
    assert(max === null || Array.isArray(max), 'getMaxSize should be null or an array');
    if (max !== null) assertNumberPair(max, 'getMaxSize');

    assertEqual(typeof win.isVisible(), 'boolean');
    assertEqual(typeof win.isResizable(), 'boolean');
    assertEqual(typeof win.isFocused(), 'boolean');
    assertEqual(typeof win.isMinimized(), 'boolean');
    assertEqual(typeof win.isMaximized(), 'boolean');
    assertEqual(typeof win.isFullscreen(), 'boolean');
    assertEqual(typeof win.isTransparent(), 'boolean');
  });
});

test('Monitors and App namespaces expose window-related surfaces', () => {
  const primary = Monitors.primary();
  assertMonitorShape(primary);

  const monitors = Monitors.list();
  assert(Array.isArray(monitors));
  for (const mon of monitors) {
    assertMonitorShape(mon);
  }

  withHiddenWindow(() => {
    const windows = App.windows();
    assert(Array.isArray(windows));
    assert(windows.some((candidate) => candidate instanceof Window));
    assert(windows.length >= 1);
    assertEqual(typeof Window.exit, 'function');
    Window.exit();
  });

  App.quit();
});

if (false as boolean) {
  const opts: WindowOptions = hiddenWindowOptions;
  const win = new Window(opts);
  const runOpts: WindowRunOptions = { fps: 60, animate: true };
  const handler: WindowEventHandler<'keydown'> = (ev) => {
    const key: number = ev.key;
    const scancode: number = ev.scancode;
    const modifiers: number = ev.modifiers;
    void key;
    void scancode;
    void modifiers;
  };
  const dispose = win.on('keydown', handler);
  const allEvents: keyof WindowEventMap = 'drop';
  const eventName: WindowEventName = allEvents;
  const windows: Window[] = App.windows();
  const monitor: MonitorInfo = Monitors.primary();
  void runOpts;
  void dispose;
  void eventName;
  void windows;
  void monitor;
}
