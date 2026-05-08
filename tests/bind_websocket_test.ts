import { createHash } from 'node:crypto';
import { createServer } from 'node:net';

import { assert, assertDeepEqual, assertEqual, assertThrows, test } from './ts_harness.ts';

type AnyWebSocketConstructor = typeof WebSocket & {
  new (...args: unknown[]): WebSocket;
  (...args: unknown[]): WebSocket;
};

const WebSocketAny = WebSocket as AnyWebSocketConstructor;

function makeUnsupportedSocket(): WebSocket {
  return new WebSocket('wss://example.invalid/socket', ['chat', 'superchat']);
}

function runEventLoopUntilQuit(timeoutMs = 50): void {
  const window = new Window({ visible: false, width: 1, height: 1, title: 'bind_websocket_test' });
  setTimeout(() => {
    window.close();
    App.quit();
  }, timeoutMs);
  App.run({ fps: 120 });
}

type EchoServer = {
  port: number;
  close: () => void;
};

type ByteArray = Uint8Array<ArrayBufferLike>;

function appendBytes(a: ByteArray, b: ArrayBufferView): ByteArray {
  const view = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
  const out = new Uint8Array(a.byteLength + view.byteLength);
  out.set(a, 0);
  out.set(view, a.byteLength);
  return out;
}

function asciiFromBytes(bytes: ByteArray): string {
  let out = '';
  for (const byte of bytes) {
    out += String.fromCharCode(byte);
  }
  return out;
}

function headerEnd(bytes: ByteArray): number {
  for (let i = 0; i + 3 < bytes.byteLength; i += 1) {
    if (bytes[i] === 13 && bytes[i + 1] === 10 && bytes[i + 2] === 13 && bytes[i + 3] === 10) {
      return i;
    }
  }
  return -1;
}

function encodeServerBinaryFrame(payload: ByteArray): ByteArray {
  assert(payload.byteLength < 126, 'test websocket frame must stay in small-frame encoding');
  const out = new Uint8Array(2 + payload.byteLength);
  out[0] = 0x82;
  out[1] = payload.byteLength;
  out.set(payload, 2);
  return out;
}

function decodeMaskedClientFrame(bytes: ByteArray): { payload: Uint8Array; used: number } | null {
  if (bytes.byteLength < 6) {
    return null;
  }
  const length = bytes[1] & 0x7f;
  assert(length < 126, 'test websocket frame must stay in small-frame encoding');
  const masked = (bytes[1] & 0x80) !== 0;
  assert(masked, 'client websocket frame must be masked');
  const used = 2 + 4 + length;
  if (bytes.byteLength < used) {
    return null;
  }
  const mask = bytes.slice(2, 6);
  const payload = new Uint8Array(length);
  for (let i = 0; i < length; i += 1) {
    payload[i] = bytes[6 + i] ^ mask[i % 4];
  }
  return { payload, used };
}

async function startBinaryEchoWebSocketServer(): Promise<EchoServer> {
  const server = createServer((socket: any) => {
    let pending: ByteArray = new Uint8Array(0);
    let handshaken = false;

    socket.on('data', (chunk: ArrayBufferView) => {
      pending = appendBytes(pending, chunk);
      if (!handshaken) {
        const end = headerEnd(pending);
        if (end < 0) {
          return;
        }
        const headers = asciiFromBytes(pending.slice(0, end));
        const key = headers.match(/Sec-WebSocket-Key:\\s*([^\\r\\n]+)/i)?.[1]?.trim();
        assert(key, 'client handshake must include Sec-WebSocket-Key');
        const hash = createHash('sha1').update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`);
        const accept = (hash.digest as unknown as (encoding: 'base64') => string)('base64');
        socket.write(
          'HTTP/1.1 101 Switching Protocols\\r\\n' +
            'Upgrade: websocket\\r\\n' +
            'Connection: Upgrade\\r\\n' +
            `Sec-WebSocket-Accept: ${accept}\\r\\n\\r\\n`,
        );
        pending = pending.slice(end + 4);
        handshaken = true;
      }

      const frame = decodeMaskedClientFrame(pending);
      if (frame) {
        socket.write(encodeServerBinaryFrame(frame.payload));
        pending = pending.slice(frame.used);
      }
    });
  });

  const listening = Promise.withResolvers<void>();
  server.on('error', listening.reject);
  server.listen(0, '127.0.0.1', listening.resolve);
  await listening.promise;
  const rawAddress = server.address();
  assert(
    rawAddress && typeof rawAddress === 'object' && typeof rawAddress.port === 'number',
    'echo server must bind a TCP port',
  );
  const address = rawAddress;
  return {
    port: address.port,
    close: () => {
      try {
        server.close();
      } catch (_) {
        // Already closed by the test cleanup path.
      }
    },
  };
}

test('WebSocket constructor validates required call shape', () => {
  assertThrows(() => {
    WebSocketAny('wss://example.invalid/socket');
  }, /new/);

  assertThrows(() => {
    new WebSocketAny();
  }, /missing url/);
});

test('WebSocket exposes readyState constants', () => {
  assertEqual(WebSocket.CONNECTING, 0);
  assertEqual(WebSocket.OPEN, 1);
  assertEqual(WebSocket.CLOSING, 2);
  assertEqual(WebSocket.CLOSED, 3);
});

test('WebSocket initializes deterministic properties for unsupported wss', () => {
  const ws = makeUnsupportedSocket();

  assertEqual(ws.url, 'wss://example.invalid/socket');
  assertEqual(ws.readyState, WebSocket.CLOSED);
  assertEqual(ws.bufferedAmount, 0);
  assertEqual(ws.protocol, '');
  assertEqual(ws.extensions, '');
  assertEqual(ws.binaryType, 'arraybuffer');
  assertEqual(ws.onopen, null);
  assertEqual(ws.onmessage, null);
  assertEqual(ws.onerror, null);
  assertEqual(ws.onclose, null);
});

test('WebSocket handler properties accept functions and clear on non-functions', () => {
  const ws = makeUnsupportedSocket();

  const onopen = (_ev: WebSocketEvent): void => undefined;
  const onmessage = (_ev: WebSocketMessageEvent): void => undefined;
  const onerror = (_ev: WebSocketErrorEvent): void => undefined;
  const onclose = (_ev: WebSocketCloseEvent): void => undefined;

  ws.onopen = onopen;
  ws.onmessage = onmessage;
  ws.onerror = onerror;
  ws.onclose = onclose;

  assertEqual(ws.onopen, onopen);
  assertEqual(ws.onmessage, onmessage);
  assertEqual(ws.onerror, onerror);
  assertEqual(ws.onclose, onclose);

  ws.onopen = null;
  ws.onmessage = null;
  ws.onerror = null;
  ws.onclose = null;

  assertEqual(ws.onopen, null);
  assertEqual(ws.onmessage, null);
  assertEqual(ws.onerror, null);
  assertEqual(ws.onclose, null);
});

test('WebSocket addEventListener and removeEventListener dispatch supported events', () => {
  const ws = makeUnsupportedSocket();
  const calls: string[] = [];

  const removed = (ev: WebSocketErrorEvent): void => {
    calls.push(`removed:${ev.type}`);
  };
  const keptError = (ev: WebSocketErrorEvent): void => {
    calls.push(`listener:${ev.type}:${ev.message}`);
  };
  const keptClose = (ev: WebSocketCloseEvent): void => {
    calls.push(`listener:${ev.type}:${ev.code}:${ev.wasClean}`);
  };

  ws.addEventListener('error', removed);
  ws.addEventListener('error', keptError);
  ws.removeEventListener('error', removed);
  ws.addEventListener('close', keptClose);
  ws.addEventListener('open', () => calls.push('unexpected-open'));
  ws.addEventListener('message', () => calls.push('unexpected-message'));

  runEventLoopUntilQuit();

  assertEqual(calls.length, 2);
  assert(calls[0].startsWith('listener:error:wss:// unavailable:'), calls[0]);
  assertDeepEqual(calls.slice(1), ['listener:close:1006:false']);
});

test('WebSocket onerror and onclose receive synchronous unsupported wss events', () => {
  const ws = makeUnsupportedSocket();
  const calls: string[] = [];

  ws.onopen = () => calls.push('unexpected-open');
  ws.onmessage = () => calls.push('unexpected-message');
  ws.onerror = (ev) => calls.push(`onerror:${ev.type}:${ev.message}`);
  ws.onclose = (ev) => calls.push(`onclose:${ev.type}:${ev.code}:${ev.reason}:${ev.wasClean}`);

  runEventLoopUntilQuit();

  assertEqual(calls.length, 2);
  assert(calls[0].startsWith('onerror:error:wss:// unavailable:'), calls[0]);
  assertDeepEqual(calls.slice(1), ['onclose:close:1006::false']);
});

test('WebSocket binaryType blob receives a binary echo as Blob', async () => {
  const server = await startBinaryEchoWebSocketServer();
  const payload = Uint8Array.from([0, 1, 2, 250, 255]);
  const done = Promise.withResolvers<Uint8Array>();
  const window = new Window({
    visible: false,
    width: 1,
    height: 1,
    title: 'bind_websocket_blob_test',
  });
  let cleaned = false;

  const cleanup = (): void => {
    if (cleaned) {
      return;
    }
    cleaned = true;
    server.close();
    window.close();
    App.quit();
  };

  const fail = (error: unknown): void => {
    cleanup();
    done.reject(error);
  };

  const timeout = setTimeout(() => {
    fail(new Error('timed out waiting for websocket blob echo'));
  }, 1000);

  const ws = new WebSocket(`ws://127.0.0.1:${server.port}/echo`);
  ws.binaryType = 'blob';
  assertEqual(ws.binaryType, 'blob');
  ws.onopen = () => {
    ws.send(payload);
  };
  ws.onerror = (ev) => {
    fail(new Error(ev.message));
  };
  ws.onmessage = (ev) => {
    const data = ev.data;
    try {
      assert(data instanceof Blob, 'binaryType=blob must deliver Blob message data');
      assertEqual(data.size, payload.byteLength);
      assertEqual(data.type, '');
      void data.arrayBuffer().then((ab) => {
        clearTimeout(timeout);
        cleanup();
        done.resolve(new Uint8Array(ab));
      }, fail);
    } catch (error) {
      fail(error);
    }
  };

  App.run({ fps: 120 });
  const echoed = await done.promise;
  assertDeepEqual(Array.from(echoed), Array.from(payload));
});

test('WebSocket close before open transitions a pending socket to closing', () => {
  const ws = new WebSocket('');

  assertEqual(ws.readyState, WebSocket.CONNECTING);
  ws.close(1000, 'client shutdown');
  assertEqual(ws.readyState, WebSocket.CLOSING);
});
