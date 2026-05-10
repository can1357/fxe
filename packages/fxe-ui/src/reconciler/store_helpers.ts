import type { Database, SQLBindings } from 'fxe:sqlite';
import { useSyncExternalStore } from './external_store.ts';
import { useMemo } from './fiber.ts';

export interface FetchResult<T> {
  data: T | undefined;
  error: unknown;
  loading: boolean;
}

export interface WebSocketStore {
  send: (data: string | ArrayBuffer | ArrayBufferView) => void;
  lastMessage: unknown;
  readyState: number;
}

type Store<T> = {
  getSnapshot: () => T;
  subscribe: (listener: () => void) => () => void;
};

type SqliteChangeDatabase = Database & {
  addEventListener?: (event: 'change', listener: () => void) => void;
  removeEventListener?: (event: 'change', listener: () => void) => void;
  on?: (event: 'change', listener: () => void) => unknown;
  off?: (event: 'change', listener: () => void) => void;
  removeListener?: (event: 'change', listener: () => void) => void;
  update_hook?: (listener: () => void) => unknown;
  updateHook?: (listener: () => void) => unknown;
};

type DisposableLike = { dispose?: () => void; close?: () => void; unsubscribe?: () => void };

function disposeUnknown(value: unknown): boolean {
  if (typeof value === 'function') {
    value();
    return true;
  }
  if (typeof value !== 'object' || value === null) return false;
  const disposable = value as DisposableLike;
  if (typeof disposable.unsubscribe === 'function') {
    disposable.unsubscribe();
    return true;
  }
  if (typeof disposable.dispose === 'function') {
    disposable.dispose();
    return true;
  }
  if (typeof disposable.close === 'function') {
    disposable.close();
    return true;
  }
  return false;
}

function subscribeSqliteChanges(db: SqliteChangeDatabase, listener: () => void): () => void {
  if (typeof db.update_hook === 'function') {
    const dispose = db.update_hook(listener);
    return () => {
      disposeUnknown(dispose);
    };
  }
  if (typeof db.updateHook === 'function') {
    const dispose = db.updateHook(listener);
    return () => {
      disposeUnknown(dispose);
    };
  }
  if (typeof db.addEventListener === 'function') {
    db.addEventListener('change', listener);
    return () => db.removeEventListener?.('change', listener);
  }
  if (typeof db.on === 'function') {
    const dispose = db.on('change', listener);
    return () => {
      if (disposeUnknown(dispose)) return;
      if (typeof db.off === 'function') db.off('change', listener);
      else db.removeListener?.('change', listener);
    };
  }

  // The current fxe:sqlite Database type does not expose sqlite3_update_hook or
  // a change event, so the portable fallback polls the query once per second via
  // the host timer. The poll only notifies when the result rows actually differ.
  const timer = setInterval(listener, 1000);
  return () => clearInterval(timer);
}

function allRows<T>(db: Database, sql: string, params: SQLBindings | undefined): T[] {
  const statement = db.query<T>(sql);
  return params === undefined ? statement.all() : statement.all(params);
}

function bytesEqual(a: ArrayBufferView, b: ArrayBufferView): boolean {
  if (a.byteLength !== b.byteLength) return false;
  const left = new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
  const right = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
  for (let i = 0; i < left.length; ++i) {
    if (left[i] !== right[i]) return false;
  }
  return true;
}

function arrayBufferEqual(a: ArrayBuffer, b: ArrayBuffer): boolean {
  if (a.byteLength !== b.byteLength) return false;
  return bytesEqual(new Uint8Array(a), new Uint8Array(b));
}

function valueEqual(a: unknown, b: unknown): boolean {
  if (Object.is(a, b)) return true;
  if (a instanceof ArrayBuffer && b instanceof ArrayBuffer) return arrayBufferEqual(a, b);
  if (ArrayBuffer.isView(a) && ArrayBuffer.isView(b)) return bytesEqual(a, b);
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; ++i) {
      if (!valueEqual(a[i], b[i])) return false;
    }
    return true;
  }
  if (typeof a !== 'object' || a === null || typeof b !== 'object' || b === null) return false;

  const left = a as Record<string, unknown>;
  const right = b as Record<string, unknown>;
  const leftKeys = Object.keys(left);
  const rightKeys = Object.keys(right);
  if (leftKeys.length !== rightKeys.length) return false;
  for (const key of leftKeys) {
    if (!Object.hasOwn(right, key) || !valueEqual(left[key], right[key])) return false;
  }
  return true;
}

function rowsEqual<T>(a: readonly T[], b: readonly T[]): boolean {
  return valueEqual(a, b);
}

function createSqliteQueryStore<T>(
  db: Database,
  sql: string,
  params: SQLBindings | undefined,
): Store<T[]> {
  let snapshot = allRows<T>(db, sql, params);

  const refresh = (listener: () => void): void => {
    const next = allRows<T>(db, sql, params);
    if (rowsEqual(snapshot, next)) return;
    snapshot = next;
    listener();
  };

  return {
    getSnapshot: () => snapshot,
    subscribe: (listener) => {
      const onChange = (): void => refresh(listener);
      const unsubscribe = subscribeSqliteChanges(db as SqliteChangeDatabase, onChange);
      onChange();
      return unsubscribe;
    },
  };
}

export function useSqliteQuery<T>(db: Database, sql: string, params?: SQLBindings): T[] {
  const store = useMemo(() => createSqliteQueryStore<T>(db, sql, params), [db, sql, params]);
  return useSyncExternalStore(store.subscribe, store.getSnapshot);
}

type AbortSignalWithRemove = AbortSignal & {
  removeEventListener?: (type: 'abort', listener: () => void) => void;
};

function createFetchStore<T>(url: string, init: RequestInit | undefined): Store<FetchResult<T>> {
  let snapshot: FetchResult<T> = { data: undefined, error: undefined, loading: true };
  let controller: AbortController | null = null;
  let started = false;
  let cleanupExternalAbort: (() => void) | undefined;
  const listeners = new Set<() => void>();

  const emit = (): void => {
    for (const listener of listeners) listener();
  };
  const setSnapshot = (next: FetchResult<T>): void => {
    snapshot = next;
    emit();
  };
  const start = (): void => {
    if (started) return;
    started = true;
    controller = new AbortController();

    const externalSignal = init?.signal as AbortSignalWithRemove | undefined;
    if (externalSignal) {
      const abortFromExternal = (): void => {
        controller?.abort(externalSignal.reason ?? 'aborted');
      };
      if (externalSignal.aborted) {
        abortFromExternal();
      } else {
        externalSignal.addEventListener('abort', abortFromExternal);
        cleanupExternalAbort = () =>
          externalSignal.removeEventListener?.('abort', abortFromExternal);
      }
    }

    void fetch(url, { ...(init ?? {}), signal: controller.signal })
      .then((response) => response.json() as Promise<T>)
      .then(
        (data) => {
          if (controller?.signal.aborted) return;
          setSnapshot({ data, error: undefined, loading: false });
        },
        (error) => {
          if (controller?.signal.aborted && listeners.size === 0) return;
          setSnapshot({ data: undefined, error, loading: false });
        },
      )
      .finally(() => {
        cleanupExternalAbort?.();
        cleanupExternalAbort = undefined;
      });
  };

  return {
    getSnapshot: () => snapshot,
    subscribe: (listener) => {
      listeners.add(listener);
      start();
      return () => {
        listeners.delete(listener);
        if (listeners.size === 0) {
          controller?.abort('unmounted');
          cleanupExternalAbort?.();
          cleanupExternalAbort = undefined;
        }
      };
    },
  };
}

export function useFetch<T>(url: string, init?: RequestInit): FetchResult<T> {
  const store = useMemo(() => createFetchStore<T>(url, init), [url, init]);
  return useSyncExternalStore(store.subscribe, store.getSnapshot);
}

function websocketConnectingState(): number {
  return WebSocket.CONNECTING;
}

function createWebSocketStore(url: string): Store<WebSocketStore> {
  let socket: WebSocket | null = null;
  let snapshot: WebSocketStore;
  const listeners = new Set<() => void>();
  const send = (data: string | ArrayBuffer | ArrayBufferView): void => {
    socket?.send(data);
  };
  snapshot = { send, lastMessage: undefined, readyState: websocketConnectingState() };

  const emit = (): void => {
    for (const listener of listeners) listener();
  };
  const update = (patch?: Partial<Omit<WebSocketStore, 'send'>>): void => {
    snapshot = {
      send,
      lastMessage:
        patch && Object.hasOwn(patch, 'lastMessage') ? patch.lastMessage : snapshot.lastMessage,
      readyState: socket?.readyState ?? snapshot.readyState,
    };
    emit();
  };
  const start = (): void => {
    if (socket !== null) return;
    socket = new WebSocket(url);
    update();
    socket.addEventListener('open', () => update());
    socket.addEventListener('message', (event: WebSocketMessageEvent) => {
      update({ lastMessage: event.data });
    });
    socket.addEventListener('error', () => update());
    socket.addEventListener('close', () => update());
  };

  return {
    getSnapshot: () => snapshot,
    subscribe: (listener) => {
      listeners.add(listener);
      start();
      return () => {
        listeners.delete(listener);
        if (listeners.size === 0 && socket !== null) {
          const closing =
            socket.readyState === WebSocket.CLOSING || socket.readyState === WebSocket.CLOSED;
          if (!closing) socket.close();
          socket = null;
        }
      };
    },
  };
}

export function useWebSocket(url: string): WebSocketStore {
  const store = useMemo(() => createWebSocketStore(url), [url]);
  return useSyncExternalStore(store.subscribe, store.getSnapshot);
}
