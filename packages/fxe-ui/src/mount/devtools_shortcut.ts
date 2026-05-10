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

function hasDebugPort(): boolean {
  const port = process.env.FXE_DEBUG_PORT;
  return typeof port === 'string' && port.length > 0;
}

export function defaultDevToolsAccelerator(): string {
  return process.platform === 'darwin' ? 'Cmd+Alt+I' : 'Ctrl+Shift+I';
}

export function installDevToolsShortcut(
  opts: DevToolsShortcutOptions = {},
): DevToolsShortcutHandle | null {
  if (!hasDebugPort()) return null;

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
      globalShortcut.unregister(accelerator);
    },
  };

  registered = globalShortcut.register(accelerator, () => {
    try {
      App.openDevTools(opts.window);
    } catch (error) {
      if (opts.onError) {
        opts.onError(error);
      } else {
        console.error(error);
      }
    }
  });

  if (!registered) {
    opts.onError?.(new Error(`failed to register DevTools shortcut: ${accelerator}`));
  }

  return handle;
}
