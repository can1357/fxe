import { CommandBuffer, type Renderer, type Window, type WindowEventName } from 'fxe';
import { defaultDevToolsAccelerator, installDevToolsShortcut, mount, Text, View } from 'fxe-ui';

import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

type RuntimeGlobals = typeof globalThis & {
  App?: {
    openDevTools(window?: Window): Window | null;
  };
  globalShortcut?: {
    register(accelerator: string, fn: () => void): boolean;
    unregister(accelerator: string): void;
    unregisterAll(): void;
  };
};

const runtime = globalThis as RuntimeGlobals;
const debugPortKey = 'FXE_DEBUG_PORT';

function restoreDebugPort(value: string | undefined): void {
  if (value === undefined) {
    delete process.env[debugPortKey];
  } else {
    process.env[debugPortKey] = value;
  }
}

function makeRenderer(): Renderer {
  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  return renderer;
}

function makeWindow(): Window {
  const listeners = new Map<string, number>();
  return {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => undefined,
    setFrameCallback: () => undefined,
    takeRedrawRequest: () => true,
    on: (event: WindowEventName) => {
      listeners.set(event, (listeners.get(event) ?? 0) + 1);
      return () => listeners.set(event, (listeners.get(event) ?? 1) - 1);
    },
  } as unknown as Window;
}

test('defaultDevToolsAccelerator matches the runtime platform', () => {
  assertEqual(
    defaultDevToolsAccelerator(),
    process.platform === 'darwin' ? 'Cmd+Alt+I' : 'Ctrl+Shift+I',
  );
});

test('installDevToolsShortcut returns null when FXE_DEBUG_PORT is unset', () => {
  const originalPort = process.env[debugPortKey];
  const originalShortcut = runtime.globalShortcut;
  try {
    delete process.env[debugPortKey];
    runtime.globalShortcut = {
      register: () => {
        throw new Error('register should not run without FXE_DEBUG_PORT');
      },
      unregister: () => undefined,
      unregisterAll: () => undefined,
    };
    assertEqual(installDevToolsShortcut(), null);
  } finally {
    restoreDebugPort(originalPort);
    runtime.globalShortcut = originalShortcut;
  }
});

test('installDevToolsShortcut returns null when globalShortcut is unavailable', () => {
  const originalPort = process.env[debugPortKey];
  const originalShortcut = runtime.globalShortcut;
  try {
    process.env[debugPortKey] = '0';
    runtime.globalShortcut = undefined;
    assertEqual(installDevToolsShortcut(), null);
  } finally {
    restoreDebugPort(originalPort);
    runtime.globalShortcut = originalShortcut;
  }
});

test('installDevToolsShortcut honours custom accelerators and catches App.openDevTools errors', () => {
  const originalPort = process.env[debugPortKey];
  const originalShortcut = runtime.globalShortcut;
  const originalApp = runtime.App;
  let registeredAccelerator: string | null = null;
  const callbacks: Array<() => void> = [];
  const unregistered: string[] = [];
  const expectedError = new Error('boom');
  let capturedError: unknown = null;

  try {
    process.env[debugPortKey] = '0';
    const shortcutStub: NonNullable<RuntimeGlobals['globalShortcut']> = {
      register(accelerator: string, fn: () => void) {
        registeredAccelerator = accelerator;
        callbacks.push(fn);
        return true;
      },
      unregister(accelerator: string) {
        unregistered.push(accelerator);
      },
      unregisterAll: () => undefined,
    };
    runtime.globalShortcut = shortcutStub;
    runtime.App = {
      openDevTools() {
        throw expectedError;
      },
    };

    const handle = installDevToolsShortcut({
      accelerator: 'Ctrl+Alt+D',
      onError: (error) => {
        capturedError = error;
      },
    });

    assert(handle !== null, 'expected a shortcut handle');
    assertEqual(handle.accelerator, 'Ctrl+Alt+D');
    assertEqual(registeredAccelerator, 'Ctrl+Alt+D');
    assertEqual(callbacks.length, 1);
    callbacks[0]?.();
    assertEqual(capturedError, expectedError);
    handle.dispose();
    handle.dispose();
    assertDeepEqual(unregistered, ['Ctrl+Alt+D']);
  } finally {
    restoreDebugPort(originalPort);
    runtime.globalShortcut = originalShortcut;
    runtime.App = originalApp;
  }
});

test('installDevToolsShortcut reports registration failures without throwing', () => {
  const originalPort = process.env[debugPortKey];
  const originalShortcut = runtime.globalShortcut;
  const errors: unknown[] = [];

  try {
    process.env[debugPortKey] = '0';
    runtime.globalShortcut = {
      register: () => false,
      unregister: () => {
        throw new Error('dispose should be a no-op when registration fails');
      },
      unregisterAll: () => undefined,
    };

    const handle = installDevToolsShortcut({
      accelerator: 'Ctrl+Shift+Alt+9',
      onError: (error) => {
        errors.push(error);
      },
    });

    assert(handle !== null, 'expected a handle even when registration fails');
    assertEqual(handle.accelerator, 'Ctrl+Shift+Alt+9');
    assertEqual(errors.length, 1);
    assert(errors[0] instanceof Error);
    handle.dispose();
    handle.dispose();
  } finally {
    restoreDebugPort(originalPort);
    runtime.globalShortcut = originalShortcut;
  }
});

test('mount auto-installs devtools shortcuts by default and supports opt-out', () => {
  const originalPort = process.env[debugPortKey];
  const originalShortcut = runtime.globalShortcut;
  const expectedAccelerator = defaultDevToolsAccelerator();
  const registerCalls: string[] = [];
  const unregisterCalls: string[] = [];
  const activeAccelerators = new Set<string>();

  try {
    process.env[debugPortKey] = '0';
    runtime.globalShortcut = {
      register(accelerator) {
        registerCalls.push(accelerator);
        if (activeAccelerators.has(accelerator)) {
          return false;
        }
        activeAccelerators.add(accelerator);
        return true;
      },
      unregister(accelerator) {
        unregisterCalls.push(accelerator);
        activeAccelerators.delete(accelerator);
      },
      unregisterAll() {
        activeAccelerators.clear();
      },
    };

    const disposeA = mount(
      View({
        style: { width: 160, height: 100 },
        children: Text({ children: 'A' }),
      }),
      makeWindow(),
      { renderer: makeRenderer() },
    );
    const disposeB = mount(
      View({
        style: { width: 160, height: 100 },
        children: Text({ children: 'B' }),
      }),
      makeWindow(),
      { renderer: makeRenderer() },
    );
    const disposeC = mount(
      View({
        style: { width: 160, height: 100 },
        children: Text({ children: 'C' }),
      }),
      makeWindow(),
      { renderer: makeRenderer(), devTools: false },
    );

    assertDeepEqual(registerCalls, [expectedAccelerator, expectedAccelerator]);
    disposeB();
    assertDeepEqual(unregisterCalls, []);
    disposeA();
    assertDeepEqual(unregisterCalls, [expectedAccelerator]);
    disposeC();
    assertDeepEqual(unregisterCalls, [expectedAccelerator]);
  } finally {
    restoreDebugPort(originalPort);
    runtime.globalShortcut = originalShortcut;
  }
});

await run();
