import type { Window } from 'fxe';

export interface DevToolsShortcutOptions {
  accelerator?: string;
  window?: Window;
  onError?: (err: unknown) => void;
}

export interface DevToolsShortcutHandle {
  dispose(): void;
  accelerator: string;
}

type RuntimeGlobals = typeof globalThis & {
  App?: {
    openDevTools(window?: Window): Window | null;
  };
  console?: {
    error?: (...args: unknown[]) => void;
  };
  globalShortcut?: {
    register(accelerator: string, fn: () => void): boolean;
    unregister(accelerator: string): void;
  };
  process?: {
    env?: Record<string, string | undefined>;
    platform?: string;
  };
};

function runtimeGlobals(): RuntimeGlobals {
  return globalThis as RuntimeGlobals;
}

function hasDebugPort(globals: RuntimeGlobals): boolean {
  const port = globals.process?.env?.FXE_DEBUG_PORT;
  return typeof port === 'string' && port.length > 0;
}

export function defaultDevToolsAccelerator(): string {
  return runtimeGlobals().process?.platform === 'darwin' ? 'Cmd+Alt+I' : 'Ctrl+Shift+I';
}

export function installDevToolsShortcut(
  opts: DevToolsShortcutOptions = {},
): DevToolsShortcutHandle | null {
  const globals = runtimeGlobals();
  const shortcuts = globals.globalShortcut;
  if (!shortcuts || !hasDebugPort(globals)) {
    return null;
  }

  const accelerator = opts.accelerator ?? defaultDevToolsAccelerator();
  let registered = false;
  let disposed = false;

  const handle: DevToolsShortcutHandle = {
    accelerator,
    dispose() {
      if (disposed) return;
      disposed = true;
      if (!registered) return;
      registered = false;
      shortcuts.unregister(accelerator);
    },
  };

  registered = shortcuts.register(accelerator, () => {
    try {
      globals.App?.openDevTools(opts.window);
    } catch (error) {
      if (opts.onError) {
        opts.onError(error);
      } else {
        globals.console?.error?.(error);
      }
    }
  });

  if (!registered) {
    opts.onError?.(new Error(`failed to register DevTools shortcut: ${accelerator}`));
  }

  return handle;
}
