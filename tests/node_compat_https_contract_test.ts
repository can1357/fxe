// Native TLS/HTTPS/HTTP2 adapters are host-backed without live network I/O here;
// fetch is stubbed so callback/EventEmitter flow stays deterministic.

import http2Default, { connect, constants, createSecureServer } from 'node:http2';
import httpsDefault, { Agent, createServer, get, request } from 'node:https';
import tlsDefault, { createSecureContext, rootCertificates, connect as tlsConnect } from 'node:tls';

import { assert, assertEqual, assertThrows, delay, run, test } from './ts_harness.ts';

type EventSource = {
  on(name: string, fn: (...args: unknown[]) => void): unknown;
};

const originalFetch = globalThis.fetch;

function collectBody(source: EventSource): Promise<string> {
  const { promise, resolve, reject } = Promise.withResolvers<string>();
  const chunks: string[] = [];
  source.on('data', (chunk: unknown) => chunks.push(String(chunk)));
  source.on('end', () => resolve(chunks.join('')));
  source.on('error', reject);
  return promise;
}

function waitForError(source: EventSource): Promise<string> {
  const { promise, resolve } = Promise.withResolvers<string>();
  source.on('error', (error: unknown) => {
    resolve(error instanceof Error ? error.message : String(error));
  });
  return promise;
}

test('node:https exposes Agent and performs callback response flow over fetch', async () => {
  assertEqual(typeof Agent, 'function');
  assert(httpsDefault.globalAgent instanceof Agent, 'https.globalAgent should use the Agent shim');
  let requestedUrl = '';
  let requestedMethod = '';
  globalThis.fetch = async (url: string | URL | Request, init?: RequestInit) => {
    requestedUrl = String(url);
    requestedMethod = String(init?.method);
    return new Response('ok-body', {
      status: 201,
      statusText: 'Created',
      headers: { 'x-contract': 'yes' },
    });
  };

  const { promise, resolve, reject } = Promise.withResolvers<void>();
  const req = request(
    'https://example.test/path?q=1',
    { method: 'POST' },
    (res: EventSource & { statusCode: number; headers: Record<string, string> }) => {
      void collectBody(res).then((body) => {
        assertEqual(res.statusCode, 201);
        assertEqual(res.headers['x-contract'], 'yes');
        assertEqual(body, 'ok-body');
        resolve();
      }, reject);
    },
  );
  req.on('error', reject);
  req.end('payload');
  await promise;
  assertEqual(requestedUrl, 'https://example.test/path?q=1');
  assertEqual(requestedMethod, 'POST');

  const getReq = get('https://example.test/get');
  getReq.on('error', reject);
});

test('node:https server APIs reject truthfully', () => {
  assertThrows(() => createServer({}), /native TLS server implementation|not implemented yet/i);
});

test('node:http2 exposes constants and module surface', () => {
  // Real http2 client+server round-trips are covered in node_compat_http2_test.
  // This contract test only verifies the module shape after the F5 native
  // binding replaced the prior h2-over-fetch fallback.
  assertEqual(constants.HTTP2_HEADER_METHOD, ':method');
  assertEqual(constants.HTTP_STATUS_OK, 200);
  assertEqual(http2Default.connect, connect);
  assert(typeof createSecureServer === 'function', 'createSecureServer should be exported');
});

test('node:tls exposes secure context and asynchronous unavailable connect errors', async () => {
  assertEqual(Array.isArray(rootCertificates), true);
  assertEqual(rootCertificates.length, 0);
  assert(createSecureContext({}) instanceof tlsDefault.SecureContext);
  const socket = tlsConnect({ host: 'example.invalid', port: 443 });
  const message = await waitForError(socket);
  assert(message.includes('native TLS socket backing'));
  await delay(1);
});

try {
  await run();
} finally {
  globalThis.fetch = originalFetch;
}
