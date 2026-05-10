// FXE resolves these package names through the host loader rather than node_modules.

import { Database } from 'fxe:sqlite';
import { CommandBuffer } from 'fxe';
import {
  Component,
  Draw,
  Layer,
  type Node,
  render,
  setRenderTarget,
  useFetch,
  useSqliteQuery,
  useWebSocket,
} from 'fxe-ui';

import { assert, assertDeepEqual, assertEqual, delay, run, test } from './ts_harness.ts';

function root(key: string, children: readonly Node[]): Node {
  return Layer({ key, children });
}

function noopDraw(): Node {
  return Draw(() => undefined);
}

test('useSqliteQuery re-renders after db.run update via fallback poll', () => {
  const originalSetInterval = globalThis.setInterval;
  const originalClearInterval = globalThis.clearInterval;
  let intervalCallback: (() => void) | undefined;
  let intervalMs = 0;
  let cleared = 0;
  globalThis.setInterval = (fn: () => void, ms?: number): number => {
    intervalCallback = fn;
    intervalMs = ms ?? 0;
    return 7;
  };
  globalThis.clearInterval = (id: number): void => {
    cleared = id;
  };

  const db = new Database(':memory:');
  let redraws = 0;
  const seen: number[] = [];
  const QueryProbe = Component(() => {
    const rows = useSqliteQuery<{ value: number }>(db, 'SELECT value FROM t WHERE id = 1');
    seen.push(rows[0]?.value ?? -1);
    return noopDraw();
  }, 'SqliteQueryProbe');

  setRenderTarget({ requestRedraw: () => ++redraws } as unknown as Parameters<
    typeof setRenderTarget
  >[0]);
  try {
    db.run('CREATE TABLE t (id INTEGER PRIMARY KEY, value INTEGER)');
    db.run('INSERT INTO t (id, value) VALUES (1, 1)');

    render(root('sqlite-store-root', [QueryProbe({ key: 'query' })]), new CommandBuffer());
    assertEqual(intervalMs, 1000);
    assertEqual(seen.join(','), '1');

    db.run('UPDATE t SET value = 2 WHERE id = 1');
    intervalCallback?.();
    assertEqual(redraws, 1);

    render(root('sqlite-store-root', [QueryProbe({ key: 'query' })]), new CommandBuffer());
    assertEqual(seen.join(','), '1,2');
  } finally {
    render(root('sqlite-store-root', [noopDraw()]), new CommandBuffer());
    db.close();
    setRenderTarget(null);
    globalThis.setInterval = originalSetInterval;
    globalThis.clearInterval = originalClearInterval;
  }

  assertEqual(cleared, 7);
});

test('useFetch resolves data on mount and aborts on unmount', async () => {
  const originalFetch = globalThis.fetch;
  const first = Promise.withResolvers<Response>();
  const second = Promise.withResolvers<Response>();
  const pending = [first, second];
  const capturedSignals: AbortSignal[] = [];
  let fetchCalls = 0;
  const mockFetch: typeof fetch = Object.assign(
    (_input: string | Request, init?: RequestInit): Promise<Response> => {
      assert(init?.signal !== undefined, 'fetch should receive AbortController signal');
      capturedSignals.push(init.signal);
      const entry = pending[fetchCalls];
      ++fetchCalls;
      return entry.promise;
    },
    { cookieJar: originalFetch.cookieJar },
  );
  globalThis.fetch = mockFetch;

  type Payload = { ok: boolean };
  const states: Array<{ data: Payload | undefined; error: boolean; loading: boolean }> = [];
  const FetchProbe = Component((props: { url: string }) => {
    const state = useFetch<Payload>(props.url);
    states.push({ data: state.data, error: state.error !== undefined, loading: state.loading });
    return noopDraw();
  }, 'FetchProbe');

  let redraws = 0;
  setRenderTarget({ requestRedraw: () => ++redraws } as unknown as Parameters<
    typeof setRenderTarget
  >[0]);
  try {
    render(
      root('fetch-store-root', [FetchProbe({ key: 'fetch', url: 'https://example.test/one' })]),
      new CommandBuffer(),
    );
    assertEqual(fetchCalls, 1);
    assertDeepEqual(states[0], { data: undefined, error: false, loading: true });

    first.resolve(new Response('{"ok":true}'));
    await first.promise;
    await delay(0);
    assertEqual(redraws, 1);

    render(
      root('fetch-store-root', [FetchProbe({ key: 'fetch', url: 'https://example.test/one' })]),
      new CommandBuffer(),
    );
    assertDeepEqual(states[1], { data: { ok: true }, error: false, loading: false });

    render(
      root('fetch-store-root', [FetchProbe({ key: 'fetch', url: 'https://example.test/two' })]),
      new CommandBuffer(),
    );
    assertEqual(fetchCalls, 2);
    assertEqual(capturedSignals[1].aborted, false);

    render(root('fetch-store-root', [noopDraw()]), new CommandBuffer());
    assertEqual(capturedSignals[1].aborted, true);
    second.reject(new Error('aborted by test'));
    await second.promise.catch(() => undefined);
  } finally {
    render(root('fetch-store-root', [noopDraw()]), new CommandBuffer());
    setRenderTarget(null);
    globalThis.fetch = originalFetch;
  }
});

test('useWebSocket reports CONNECTING then OPEN', () => {
  class MockWebSocket {
    static readonly CONNECTING = 0;
    static readonly OPEN = 1;
    static readonly CLOSING = 2;
    static readonly CLOSED = 3;

    readonly url: string;
    readonly bufferedAmount = 0;
    readonly protocol = '';
    readonly extensions = '';
    binaryType: 'arraybuffer' = 'arraybuffer';
    readyState: 0 | 1 | 2 | 3 = MockWebSocket.CONNECTING;
    onopen: ((ev: WebSocketEvent) => void) | null = null;
    onmessage: ((ev: WebSocketMessageEvent) => void) | null = null;
    onerror: ((ev: WebSocketErrorEvent) => void) | null = null;
    onclose: ((ev: WebSocketCloseEvent) => void) | null = null;

    private readonly listeners = new Map<string, Set<(ev: unknown) => void>>();

    constructor(url: string) {
      this.url = url;
      sockets.push(this);
    }

    send(_data: string | ArrayBuffer | ArrayBufferView): void {}

    close(): void {
      this.readyState = MockWebSocket.CLOSED;
      const event = { type: 'close', code: 1000, reason: '', wasClean: true };
      this.onclose?.(event);
      this.dispatch('close', event);
    }

    addEventListener(
      type: 'open' | 'message' | 'error' | 'close',
      listener: (ev: unknown) => void,
    ): void {
      let listeners = this.listeners.get(type);
      if (!listeners) {
        listeners = new Set();
        this.listeners.set(type, listeners);
      }
      listeners.add(listener);
    }

    removeEventListener(
      type: 'open' | 'message' | 'error' | 'close',
      listener: (ev: unknown) => void,
    ): void {
      this.listeners.get(type)?.delete(listener);
    }

    open(): void {
      this.readyState = MockWebSocket.OPEN;
      const event = { type: 'open' };
      this.onopen?.(event);
      this.dispatch('open', event);
    }

    private dispatch(type: string, event: unknown): void {
      for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
  }

  const originalWebSocket = globalThis.WebSocket;
  const sockets: MockWebSocket[] = [];
  globalThis.WebSocket = MockWebSocket as unknown as typeof WebSocket;

  const readyStates: number[] = [];
  const SocketProbe = Component(() => {
    const socket = useWebSocket('ws://example.test/socket');
    readyStates.push(socket.readyState);
    return noopDraw();
  }, 'SocketProbe');

  let redraws = 0;
  setRenderTarget({ requestRedraw: () => ++redraws } as unknown as Parameters<
    typeof setRenderTarget
  >[0]);
  try {
    render(root('websocket-store-root', [SocketProbe({ key: 'socket' })]), new CommandBuffer());
    assertEqual(readyStates.join(','), '0');
    assertEqual(sockets.length, 1);

    redraws = 0;
    sockets[0].open();
    assertEqual(redraws, 1);
    render(root('websocket-store-root', [SocketProbe({ key: 'socket' })]), new CommandBuffer());
    assertEqual(readyStates.join(','), '0,1');
  } finally {
    render(root('websocket-store-root', [noopDraw()]), new CommandBuffer());
    setRenderTarget(null);
    globalThis.WebSocket = originalWebSocket;
  }
});

await run();
