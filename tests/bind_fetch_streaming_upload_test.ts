import net from 'node:net';
import { assertEqual, test } from './ts_harness.ts';

type TestSocket = {
  on(name: string, cb: (chunk?: Uint8Array) => void): void;
  end(chunk?: string): void;
  destroy(): void;
};

type StreamingRequestInit = RequestInit & { duplex: 'half' };

type CompletedRequest = {
  headers: string;
  body: string;
  totalBytes: number;
};

function chunkToText(chunk?: Uint8Array): string {
  return chunk ? Array.from(chunk, (byte) => String.fromCharCode(byte)).join('') : '';
}

async function withHttpServer(
  handler: (socket: TestSocket, request: CompletedRequest) => void,
  fn: (url: string) => Promise<void>,
  options: { captureBody?: boolean } = {},
): Promise<void> {
  const sockets = new Set<TestSocket>();
  const server = net.createServer((socket: TestSocket) => {
    sockets.add(socket);
    let headers = '';
    let headerBuffer = '';
    let bodyBuffer = '';
    let contentLength: number | null = null;
    let chunked = false;
    let totalBytes = 0;
    let capturedBody = '';
    let completed = false;
    const captureBody = options.captureBody ?? true;

    const maybeComplete = () => {
      if (completed) {
        return;
      }
      if (contentLength !== null) {
        if (bodyBuffer.length < contentLength) {
          return;
        }
        if (captureBody) {
          capturedBody = bodyBuffer.slice(0, contentLength);
        }
        totalBytes = contentLength;
        completed = true;
      } else if (chunked) {
        while (true) {
          const lineEnd = bodyBuffer.indexOf('\r\n');
          if (lineEnd < 0) {
            return;
          }
          const size = Number.parseInt(bodyBuffer.slice(0, lineEnd).split(';', 1)[0] || '0', 16);
          if (!Number.isFinite(size)) {
            throw new Error('invalid chunked request body');
          }
          const dataStart = lineEnd + 2;
          const dataEnd = dataStart + size;
          if (bodyBuffer.length < dataEnd + 2) {
            return;
          }
          if (captureBody) {
            capturedBody += bodyBuffer.slice(dataStart, dataEnd);
          }
          totalBytes += size;
          if (bodyBuffer.slice(dataEnd, dataEnd + 2) !== '\r\n') {
            throw new Error('invalid chunk terminator');
          }
          bodyBuffer = bodyBuffer.slice(dataEnd + 2);
          if (size === 0) {
            if (!bodyBuffer.startsWith('\r\n')) {
              return;
            }
            bodyBuffer = bodyBuffer.slice(2);
            completed = true;
            break;
          }
        }
      } else {
        throw new Error('expected content-length or chunked request body');
      }

      handler(socket, { headers, body: capturedBody, totalBytes });
    };

    socket.on('data', (chunk?: Uint8Array) => {
      const text = chunkToText(chunk);
      if (completed) {
        return;
      }
      if (!headers) {
        headerBuffer += text;
        const headerEnd = headerBuffer.indexOf('\r\n\r\n');
        if (headerEnd < 0) {
          return;
        }
        headers = headerBuffer.slice(0, headerEnd);
        bodyBuffer = headerBuffer.slice(headerEnd + 4);
        headerBuffer = '';
        const lengthMatch = /\r\ncontent-length:\s*(\d+)/i.exec(`\r\n${headers}`);
        if (lengthMatch) {
          contentLength = Number(lengthMatch[1]);
        } else if (/\r\ntransfer-encoding:\s*chunked/i.test(`\r\n${headers}`)) {
          chunked = true;
        }
        maybeComplete();
        return;
      }
      bodyBuffer += text;
      maybeComplete();
    });
  });
  const { promise: listenPromise, resolve: resolveListen } = Promise.withResolvers<void>();
  server.listen(0, '127.0.0.1', resolveListen);
  await listenPromise;
  const address = server.address() as { port: number };
  try {
    await fn(`http://127.0.0.1:${address.port}/fetch-stream-upload-test`);
  } finally {
    for (const socket of sockets) {
      socket.destroy();
    }
    server.close();
  }
}

test('fetch uploads a streamed body with chunked transfer encoding', async () => {
  const expected = Array.from({ length: 64 }, (_, index) =>
    String.fromCharCode(97 + (index % 26)).repeat(1024),
  ).join('');
  let responded = false;

  await withHttpServer(
    (socket, request) => {
      if (responded) {
        return;
      }
      responded = true;
      assertEqual(/\r\ntransfer-encoding:\s*chunked/i.test(`\r\n${request.headers}`), true);
      assertEqual(request.totalBytes, expected.length);
      socket.end(
        `HTTP/1.1 200 OK\r\nContent-Length: ${request.body.length}\r\nConnection: close\r\n\r\n${request.body}`,
      );
    },
    async (url) => {
      let index = 0;
      const stream = new ReadableStream<Uint8Array>({
        pull(controller) {
          if (index >= 64) {
            controller.close();
            return;
          }
          const chunk = String.fromCharCode(97 + (index % 26)).repeat(1024);
          controller.enqueue(new TextEncoder().encode(chunk));
          index += 1;
        },
      });

      const response = await fetch(url, {
        method: 'POST',
        body: stream,
        duplex: 'half',
      } as StreamingRequestInit);

      assertEqual(response.ok, true);
      assertEqual(await response.text(), expected);
    },
  );
});

test('fetch uploads 100 MiB from a stream without requiring a buffered body', async () => {
  const totalBytes = 100 * 1024 * 1024;
  const chunkSize = 1024;
  const totalChunks = totalBytes / chunkSize;
  const chunk = new Uint8Array(chunkSize).fill(98);
  let responded = false;

  await withHttpServer(
    (socket, request) => {
      if (responded) {
        return;
      }
      responded = true;
      assertEqual(request.totalBytes, totalBytes);
      socket.end('HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n');
    },
    async (url) => {
      let sentChunks = 0;
      const stream = new ReadableStream<Uint8Array>({
        pull(controller) {
          if (sentChunks >= totalChunks) {
            controller.close();
            return;
          }
          controller.enqueue(chunk);
          sentChunks += 1;
        },
      });

      const response = await fetch(url, {
        method: 'POST',
        body: stream,
        duplex: 'half',
      } as StreamingRequestInit);

      assertEqual(response.status, 204);
      assertEqual(sentChunks, totalChunks);
    },
    { captureBody: false },
  );
});
