import net from 'node:net';
import {
  assert,
  assertDeepEqual,
  assertEqual,
  assertRejects,
  assertThrows,
  delay,
  test,
} from './ts_harness.ts';

type TestSocket = {
  on(name: string, cb: (chunk?: Uint8Array) => void): void;
  write(chunk: string): void;
  end(chunk?: string): void;
  destroy(): void;
};

async function expectRejectName(fn: () => Promise<unknown>, name: string): Promise<Error> {
  try {
    await fn();
  } catch (error) {
    assert(error instanceof Error, 'fetch should reject with an Error');
    assertEqual(error.name, name);
    return error;
  }
  throw new Error('expected promise to reject');
}

async function withHttpServer(
  handler: (socket: TestSocket, request: string) => void,
  fn: (url: string) => Promise<void>,
): Promise<void> {
  const sockets = new Set<TestSocket>();
  const server = net.createServer((socket: TestSocket) => {
    sockets.add(socket);
    let request = '';
    socket.on('data', (chunk?: Uint8Array) => {
      if (chunk) {
        request += Array.from(chunk, (byte) => String.fromCharCode(byte)).join('');
      }
      if (request.includes('\r\n\r\n')) {
        handler(socket, request);
      }
    });
  });
  const { promise: listenPromise, resolve: resolveListen } = Promise.withResolvers<void>();
  server.listen(0, '127.0.0.1', resolveListen);
  await listenPromise;
  const address = server.address() as { port: number };
  try {
    await fn(`http://127.0.0.1:${address.port}/fetch-test`);
  } finally {
    for (const socket of sockets) {
      socket.destroy();
    }
    server.close();
  }
}

test('Headers normalizes names and preserves combined values case-insensitively', () => {
  const headers = new Headers([
    ['Content-Type', 'text/plain'],
    ['X-Trace', 'first'],
  ]);

  assertEqual(headers.get('content-type'), 'text/plain');
  assertEqual(headers.get('CONTENT-TYPE'), 'text/plain');
  assert(headers.has('x-trace'));
  assert(headers.has('X-TRACE'));

  headers.append('x-trace', 'second');
  assertEqual(headers.get('X-Trace'), 'first, second');

  headers.set('X-TRACE', 'final');
  assertEqual(headers.get('x-trace'), 'final');

  headers.delete('CONTENT-type');
  assertEqual(headers.has('content-type'), false);
  assertEqual(headers.get('content-type'), null);
});

test('fetch.cookieJar exposes set get and clear', () => {
  assert(fetch.cookieJar);
  const jar = fetch.cookieJar();
  jar.clear();
  jar.set('example.test', 'sid', 'abc', '/');
  assertEqual(jar.get('http://example.test/path'), 'sid=abc');
  jar.clear();
  assertEqual(jar.get('http://example.test/path'), '');
});

test('Headers accepts records, arrays, and other Headers without aliasing', () => {
  const fromRecord = new Headers({ Accept: 'application/json', 'X-Mode': 'record' });
  assertEqual(fromRecord.get('accept'), 'application/json');
  assertEqual(fromRecord.get('x-mode'), 'record');

  const copied = new Headers(fromRecord);
  copied.set('accept', 'text/plain');

  assertEqual(fromRecord.get('ACCEPT'), 'application/json');
  assertEqual(copied.get('ACCEPT'), 'text/plain');
});

test('Headers forEach reports lower-case keys, values, and parent', () => {
  const headers = new Headers({ B: '2', A: '1' });
  const seen: string[] = [];
  const parents: Headers[] = [];

  headers.forEach((value, key, parent) => {
    seen.push(`${key}:${value}`);
    parents.push(parent);
  });

  assertDeepEqual(seen, ['b:2', 'a:1']);
  assertEqual(parents.length, 2);
  assert(
    parents.every((parent) => parent === headers),
    'forEach should pass the Headers instance',
  );
});

test('Headers constructor and methods reject/ignore invalid receiver shape predictably', () => {
  assertThrows(() => {
    (Headers as unknown as () => Headers)();
  }, 'Headers must be called with new');

  const detachedGet = Headers.prototype.get as unknown as (name: string) => string | null;
  assertEqual(detachedGet('missing'), undefined as unknown as string | null);
});

test('Request exposes url and method from string input and init', () => {
  const getRequest = new Request('http://127.0.0.1/resource');
  assertEqual(getRequest.url, 'http://127.0.0.1/resource');
  assertEqual(getRequest.method, 'GET');

  const postRequest = new Request('http://127.0.0.1/post', {
    method: 'POST',
    headers: { 'X-Test': 'yes' },
    body: 'payload',
  });
  assertEqual(postRequest.url, 'http://127.0.0.1/post');
  assertEqual(postRequest.method, 'POST');
});

test('Request validates construction and unsupported stream bodies', () => {
  assertThrows(() => {
    (Request as unknown as () => Request)();
  }, 'Request must be called with new');

  assertThrows(() => {
    new Request('http://127.0.0.1/upload', {
      body: { getReader() {} } as unknown as ArrayBuffer,
    });
  }, 'ReadableStream body is not supported');
});

test('AbortController exposes signal state and fires abort listeners once', () => {
  const controller = new AbortController();
  const signal = controller.signal;
  const calls: string[] = [];

  assertEqual(signal.aborted, false);
  assertEqual(signal.reason, undefined);

  signal.addEventListener('abort', () => calls.push(`first:${signal.reason}`));
  signal.addEventListener('abort', () => calls.push(`second:${signal.aborted}`));
  signal.addEventListener('ignored' as 'abort', () => calls.push('ignored'));

  controller.abort('test-reason');
  controller.abort('second-reason');

  assertEqual(signal.aborted, true);
  assertEqual(signal.reason, 'test-reason');
  assertDeepEqual(calls, ['first:test-reason', 'second:true']);
});

test('AbortSignal cannot be directly constructed and default abort reason is stable', () => {
  assertThrows(() => {
    new (AbortSignal as unknown as { new (): AbortSignal })();
  }, 'AbortSignal cannot be constructed directly');

  const controller = new AbortController();
  controller.abort();
  assertEqual(controller.signal.aborted, true);
  assertEqual(controller.signal.reason, 'aborted');
});

test('fetch rejects missing and empty URLs with TypeError messages', async () => {
  await assertRejects(() => (fetch as unknown as () => Promise<Response>)(), 'fetch: missing url');
  await assertRejects(() => fetch(''), 'fetch: empty url');
});

test('fetch streams ReadableStream request bodies through libcurl upload callbacks', async () => {
  const sockets = new Set<TestSocket>();
  const received = Promise.withResolvers<number>();
  const server = net.createServer((socket: TestSocket) => {
    sockets.add(socket);
    let request = '';
    let responded = false;
    socket.on('data', (chunk?: Uint8Array) => {
      if (!chunk || responded) {
        return;
      }
      request += Array.from(chunk, (byte) => String.fromCharCode(byte)).join('');
      const headerEnd = request.indexOf('\r\n\r\n');
      if (headerEnd < 0) {
        return;
      }
      const headers = request.slice(0, headerEnd);
      const body = request.slice(headerEnd + 4);
      const lengthMatch = /\r\ncontent-length:\s*(\d+)/i.exec(`\r\n${headers}`);
      let total = 0;
      let complete = false;
      if (lengthMatch) {
        total = Number(lengthMatch[1]);
        complete = body.length >= total;
      } else if (/\r\ntransfer-encoding:\s*chunked/i.test(`\r\n${headers}`)) {
        let offset = 0;
        complete = true;
        while (true) {
          const lineEnd = body.indexOf('\r\n', offset);
          if (lineEnd < 0) {
            complete = false;
            break;
          }
          const size = Number.parseInt(body.slice(offset, lineEnd).split(';', 1)[0] || '0', 16);
          if (!Number.isFinite(size)) {
            complete = false;
            break;
          }
          const dataStart = lineEnd + 2;
          const dataEnd = dataStart + size;
          if (body.length < dataEnd + 2) {
            complete = false;
            break;
          }
          total += size;
          if (body.slice(dataEnd, dataEnd + 2) !== '\r\n') {
            complete = false;
            break;
          }
          offset = dataEnd + 2;
          if (size === 0) {
            complete = body.length >= offset + 2 && body.slice(offset, offset + 2) === '\r\n';
            break;
          }
        }
      }
      if (!complete) {
        return;
      }
      responded = true;
      received.resolve(total);
      const payload = `${total}`;
      socket.end(
        `HTTP/1.1 200 OK\r\nContent-Length: ${payload.length}\r\nConnection: close\r\n\r\n${payload}`,
      );
    });
  });
  const { promise: listenPromise, resolve: resolveListen } = Promise.withResolvers<void>();
  server.listen(0, '127.0.0.1', resolveListen);
  await listenPromise;
  const address = server.address() as { port: number };
  try {
    let pulls = 0;
    const stream = new ReadableStream({
      pull(controller) {
        controller.enqueue(new Uint8Array(100_000).fill(97));
        pulls += 1;
        if (pulls >= 10) {
          controller.close();
        }
      },
    });
    const response = await fetch(`http://127.0.0.1:${address.port}/upload`, {
      method: 'POST',
      body: stream,
    });
    assertEqual(await response.text(), '1000000');
    assertEqual(await received.promise, 1_000_000);
  } finally {
    for (const socket of sockets) {
      socket.destroy();
    }
    server.close();
  }
});

test('fetch rejects pre-aborted signals before network submission', async () => {
  const signal = (AbortSignal as unknown as { abort(reason?: string): AbortSignal }).abort(
    'already-done',
  );

  const error = await expectRejectName(
    () => fetch('http://127.0.0.1/pre-aborted', { signal }),
    'AbortError',
  );
  assertEqual(error.message, 'already-done');
});

test('fetch accepts invalid local URLs for pre-submit rejection paths without network dependency', async () => {
  const controller = new AbortController();
  controller.abort('invalid-local-url');

  const error = await expectRejectName(
    () => fetch('http://127.0.0.1:0/', { signal: controller.signal }),
    'AbortError',
  );
  assertEqual(error.message, 'invalid-local-url');
});

test('fetch rejects mid-flight abort exactly once with AbortError', async () => {
  await withHttpServer(
    (socket) => {
      setTimeout(() => {
        socket.end('HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nlate');
      }, 250);
    },
    async (url) => {
      const controller = new AbortController();
      const pending = fetch(url, { signal: controller.signal });
      await delay(10);
      controller.abort('stop-now');
      const error = await expectRejectName(() => pending, 'AbortError');
      assertEqual(error.message, 'stop-now');
    },
  );
});

test('fetch timeout rejects with TimeoutError', async () => {
  await withHttpServer(
    (socket) => {
      setTimeout(() => socket.end(), 500);
    },
    async (url) => {
      await expectRejectName(
        () => fetch(url, { timeout_ms: 100 } as unknown as RequestInit),
        'TimeoutError',
      );
    },
  );
});

test('Response constructor state and body methods are verified when constructible', async () => {
  let response: Response;
  try {
    response = new (
      Response as unknown as {
        new (body?: string, init?: { status?: number; headers?: HeadersInit }): Response;
      }
    )('{"ok":true}', { status: 201, headers: [['Content-Type', 'application/json']] });
  } catch (error) {
    assert(error instanceof Error, 'Response constructor should throw an Error when unavailable');
    assert(
      error.message.includes('Response constructor is not implemented in v0'),
      `unexpected Response constructor error: ${error.message}`,
    );
    return;
  }

  assertEqual(response.status, 201);
  assertEqual(response.ok, true);
  assertEqual(response.bodyUsed, false);
  assertEqual(response.headers.get('content-type'), 'application/json');

  const text = await response.text();
  assertEqual(text, '{"ok":true}');
  assertEqual(response.bodyUsed, true);

  const jsonResponse = new (Response as unknown as { new (body?: string): Response })(
    '{"answer":42}',
  );
  assertDeepEqual(await jsonResponse.json(), { answer: 42 });

  const bufferResponse = new (Response as unknown as { new (body?: string): Response })('abc');
  const buffer = await bufferResponse.arrayBuffer();
  assertDeepEqual(Array.from(new Uint8Array(buffer)), [97, 98, 99]);
});
