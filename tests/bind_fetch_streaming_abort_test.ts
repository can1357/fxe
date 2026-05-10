import net from 'node:net';
import { assert, assertEqual, assertRejects, delay, test } from './ts_harness.ts';

type TestSocket = {
  on(name: string, cb: (chunk?: Uint8Array) => void): void;
  write(chunk: string): void;
  end(chunk?: string): void;
  destroy(): void;
};

type StreamingRequestInit = RequestInit & { duplex: 'half' };

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
    await fn(`http://127.0.0.1:${address.port}/fetch-stream-abort-test`);
  } finally {
    for (const socket of sockets) {
      socket.destroy();
    }
    server.close();
  }
}

function parseCompletedRequestBodySize(request: string): number | null {
  const headerEnd = request.indexOf('\r\n\r\n');
  if (headerEnd < 0) {
    return null;
  }
  const headers = request.slice(0, headerEnd);
  const body = request.slice(headerEnd + 4);
  const lengthMatch = /\r\ncontent-length:\s*(\d+)/i.exec(`\r\n${headers}`);
  if (lengthMatch) {
    const total = Number(lengthMatch[1]);
    return body.length >= total ? total : null;
  }
  if (!/\r\ntransfer-encoding:\s*chunked/i.test(`\r\n${headers}`)) {
    return null;
  }

  let offset = 0;
  let total = 0;
  while (true) {
    const lineEnd = body.indexOf('\r\n', offset);
    if (lineEnd < 0) {
      return null;
    }
    const size = Number.parseInt(body.slice(offset, lineEnd).split(';', 1)[0] || '0', 16);
    if (!Number.isFinite(size)) {
      return null;
    }
    const dataStart = lineEnd + 2;
    const dataEnd = dataStart + size;
    if (body.length < dataEnd + 2) {
      return null;
    }
    total += size;
    if (body.slice(dataEnd, dataEnd + 2) !== '\r\n') {
      return null;
    }
    offset = dataEnd + 2;
    if (size === 0) {
      return body.length >= offset + 2 && body.slice(offset, offset + 2) === '\r\n' ? total : null;
    }
  }
}

test('fetch with stream body rejects with AbortError when aborted mid-flight', async () => {
  const firstBodyChunk = Promise.withResolvers<void>();
  let sawBody = false;

  await withHttpServer(
    (_socket, request) => {
      const headerEnd = request.indexOf('\r\n\r\n');
      if (headerEnd < 0 || sawBody) {
        return;
      }
      if (request.slice(headerEnd + 4).length === 0) {
        return;
      }
      sawBody = true;
      firstBodyChunk.resolve();
    },
    async (url) => {
      let pulls = 0;
      let wasCanceled = false;
      let cancelReason: unknown;
      const controller = new AbortController();
      const stream = new ReadableStream<Uint8Array>({
        pull(streamController) {
          pulls += 1;
          streamController.enqueue(new Uint8Array(4096).fill(97));
        },
        cancel(reason) {
          wasCanceled = true;
          cancelReason = reason;
        },
      });

      const pending = fetch(url, {
        method: 'POST',
        body: stream,
        duplex: 'half',
        signal: controller.signal,
      } as StreamingRequestInit);

      await firstBodyChunk.promise;
      await delay(50);
      controller.abort('user requested');

      await assertRejects(() => pending, /user requested|abort/i);
      const error = await expectRejectName(() => pending, 'AbortError');
      assertEqual(error.message, 'user requested');
      assertEqual(wasCanceled, true);
      assertEqual(cancelReason, 'user requested');

      await delay(50);
      const pullsAfterAbort = pulls;
      await delay(50);
      assertEqual(pulls, pullsAfterAbort);
    },
  );
});

test('fetch with pre-aborted signal rejects without pulling stream', async () => {
  let pulls = 0;
  let wasCanceled = false;
  const controller = new AbortController();
  controller.abort('preempted');
  const stream = new ReadableStream<Uint8Array>({
    pull() {
      pulls += 1;
    },
    cancel() {
      wasCanceled = true;
    },
  });

  const pending = fetch('http://127.0.0.1:0/pre-aborted-stream', {
    method: 'POST',
    body: stream,
    duplex: 'half',
    signal: controller.signal,
  } as StreamingRequestInit);

  await assertRejects(() => pending, /preempted|abort/i);
  const error = await expectRejectName(() => pending, 'AbortError');
  assertEqual(error.message, 'preempted');
  assertEqual(pulls, 0);
  // Pre-aborted signals short-circuit before stream consumption, so no cancel() is expected.
  assertEqual(wasCanceled, false);
});

test('fetch with stream body completes normally without abort', async () => {
  const received = Promise.withResolvers<number>();
  let responded = false;

  await withHttpServer(
    (socket, request) => {
      if (responded) {
        return;
      }
      const total = parseCompletedRequestBodySize(request);
      if (total === null) {
        return;
      }
      responded = true;
      received.resolve(total);
      socket.end('HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n');
    },
    async (url) => {
      let pulled = false;
      const stream = new ReadableStream<Uint8Array>({
        pull(controller) {
          if (pulled) {
            controller.close();
            return;
          }
          pulled = true;
          controller.enqueue(new Uint8Array([111, 107]));
          controller.close();
        },
      });

      const response = await fetch(url, {
        method: 'POST',
        body: stream,
        duplex: 'half',
      } as StreamingRequestInit);

      assertEqual(response.ok, true);
      assertEqual(await received.promise, 2);
    },
  );
});
