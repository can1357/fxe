import http2Default, { connect, constants, createSecureServer } from 'node:http2';

import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

const CERT_PEM = `-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIULWllllk5MThgFlNnEn673iafmOcwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDUwNzE1MDc1OVoXDTM2MDUw
NDE1MDc1OVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAsptDxbszD+poOpDTx4PMJNNWpL3vHNTKdhqzvhC4i50m
+uPXsVlQV0gBFLX5DPF1pYJfVzdgwiomosi7LuPzyVku9RpLSCWIOksSunScdyIH
rCSG4+G+R3C1PC7GXwrgdngZg5icKFkulW6xc+3jeK+2Mthn6URz2sdq5GDIq28s
/K+FsXc35INInCbYcGApuGKzDc++laENQbuAKaZm2zQRKqQI0pSXo2H6V2TAl19x
f1pYF8chwDXqJ8Nl7MtIWtuYMOo53WKUC5U8mDYoP0VU2He8d4ad5HB8wDVEpZK5
28P/AySm+vhKWw6kU680AyJsFeZ1H8yLwbGZYNj4rwIDAQABo28wbTAdBgNVHQ4E
FgQUu7jsaydOCb+RE3NlBHTP+sOx3skwHwYDVR0jBBgwFoAUu7jsaydOCb+RE3Nl
BHTP+sOx3skwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBAABEPEI0idJ9uCXPam2Gj7kVNW5jijjm
YJTj/UHu3bC+RFew2CZijn6xklBJFfgx60ov6XQng6LqcusjX5qKP/jhd4tLhBmx
l7cz46OSdbh+ET4EajZSr+NXSP1o6E2P9bH3A5VkfEPJHtJEQMgk3hvhxSxXSk2Z
YQzX2PnBVZ7Mdq923WVOzQ4noamQtlSi3MBFiLgtR8frpMYB94H24yBRUD3guThc
f+8+7SIAJUaC50tgqhdcWov0u1pJgvdrcaj348o7ehLzqdIBpe9L+uDJFdNdmT5z
Pojw9zXzus17rnwk3nOeJ0Hf/8oVMlh/ESDhdzMftgIwHyBiLycWy/k=
-----END CERTIFICATE-----`;

const KEY_PEM = `-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCym0PFuzMP6mg6
kNPHg8wk01akve8c1Mp2GrO+ELiLnSb649exWVBXSAEUtfkM8XWlgl9XN2DCKiai
yLsu4/PJWS71GktIJYg6SxK6dJx3IgesJIbj4b5HcLU8LsZfCuB2eBmDmJwoWS6V
brFz7eN4r7Yy2GfpRHPax2rkYMirbyz8r4Wxdzfkg0icJthwYCm4YrMNz76VoQ1B
u4AppmbbNBEqpAjSlJejYfpXZMCXX3F/WlgXxyHANeonw2Xsy0ha25gw6jndYpQL
lTyYNig/RVTYd7x3hp3kcHzANUSlkrnbw/8DJKb6+EpbDqRTrzQDImwV5nUfzIvB
sZlg2PivAgMBAAECggEAGXAPqPvOe/fQvHagEwxsaNpIvtHmWl7cLxICg5FyF0Bc
quMEd1fXH3c74C1CuVsyfE4jMhLLDxxdwFWCg10n/YdcLsB99FqUGmlS04eEOVt5
aEUTiSU/qoEc7uNikWrFKVpVl+6GXyDEh7fqQi6hdTDhbEByHEEJlyFL0hcOvYus
5rVqauhGdIuxZ1ZUi33dgOPS59Q/K/fdy9AyopzvqRDY0UqJmRi6E9V0DoF2eiMI
FftyMNEOs0ZazfquxsG0YDdfVXFfpO+d75sVZeImrr8zSVKJ2CmQOHtq2Q8w4mE0
70B6C76WqtQQIiDynVwINuNJVQ8AqKzio1Jnci0iYQKBgQDoj215zHEbLymljVXA
gmDJsNsGdlpdW8qoMJ2htB2+YS1124TA3/qv3OTLgX9KA4l69ROlwEYRXPLtkql4
s5skaWXKS8DclgHzVhscfrd4+sJJJrhR7Vy7qFYMQIf6HwQVzwBJgro9vPr6ecOO
lUUZOaSOgYxN6oRGNUwsvtpoGQKBgQDEm7XBgToTXF/IKk43h69fL5deKToOVjkV
C3eKrQ6aKPBW5z90m7RsEOIBlS7GZmA97mVo4toaotzK/Gdp3YPexoRaTAv88rRP
U/mTphiWVvIfX07hVSMEPhaCJf2nEyS88M40iR6zzb1hs7xIFzRAHeypu9ZZPjxj
KdnpWmUgBwKBgDrSDB6CVxlJFH+K/+VxFInu8Xbw+GokjV187mG37M36RkVJAIrI
G9/fPv86Abf2rQ8sbYu+1foOSGNOdQ7SXqsW/WftQRqJ1nR1kuXiJwWyZvGZmYUf
RBUyvpDawYnBzoa1lJ0DM5fp9JDlu1CU8KUwry5cFeCfMFWRpXKr0xIBAoGAd9F9
X0RmJE5zgQVnTag/VH8ofJYbb4lUmGK4o6b78y9n6U5c+a+6sPFJCzXjn73cgWG8
I8O8r+b5MCvKylXZe/b3yh/2Xl17Ta0buMPM0DKEtGHdLK45/Ofpx79nal7cUNlg
kdvO/j0wYU6sPDMIANs70+VJqHGpU7W5u+D/KBkCgYAsmitwmlWNAJMuEcjlSHKR
QENLXORjrGTuwyCiCmAv0pnteiiPCTjMWKe1kGUQ49whBUPkRItJ7CazX5zooEzu
/GJp5L33/lzKAx7KZy+HtAoUqSrcfixDR5mZ9lEZyzghIW5Ad0rTGXuz8uCHcG/B
76/sAfgC3tu8K6G+hHhJgw==
-----END PRIVATE KEY-----`;

type EventSource = {
  on(name: string, fn: (...args: unknown[]) => void): unknown;
};

type Http2ServerLike = EventSource & {
  listen(port: number, host?: string | (() => void), callback?: () => void): Http2ServerLike;
  address(): { port: number } | null;
  close(callback?: () => void): void;
};

type CreateSecureServer = (
  options: unknown,
  listener?: (
    stream: EventSource & {
      respond(headers: Record<string, string | number>): void;
      end(chunk?: string): void;
    },
    headers: Record<string, string>,
  ) => void,
) => Http2ServerLike;

type Connect = (
  url: string,
  options?: unknown,
) => {
  request(
    headers: Record<string, string>,
    options?: { signal?: AbortSignal; timeout?: number; timeoutMs?: number },
  ): EventSource & { end(): void };
  close(): void;
};

const nativeCreateSecureServer = createSecureServer as unknown as CreateSecureServer;
const nativeConnect = connect as unknown as Connect;

function collectBody(source: EventSource): Promise<string> {
  const { promise, resolve, reject } = Promise.withResolvers<string>();
  const chunks: string[] = [];
  source.on('data', (chunk: unknown) => chunks.push(String(chunk)));
  source.on('end', () => resolve(chunks.join('')));
  source.on('error', reject);
  return promise;
}

function listen(server: Http2ServerLike): Promise<void> {
  const { promise, resolve, reject } = Promise.withResolvers<void>();
  server.on('error', reject);
  server.listen(0, resolve);
  return promise;
}

test('node:http2 exposes native constants and default export', () => {
  assertEqual(constants.HTTP2_HEADER_METHOD, ':method');
  assertEqual(constants.HTTP2_HEADER_PATH, ':path');
  assertEqual(constants.HTTP2_HEADER_STATUS, ':status');
  assertEqual(constants.HTTP_STATUS_OK, 200);
  assertEqual(http2Default.connect, connect);
  assertEqual(http2Default.createSecureServer, createSecureServer);
});

test('node:http2 performs localhost TLS HTTP/2 GET roundtrip', async () => {
  const received: Record<string, string> = {};
  const server = nativeCreateSecureServer(
    { cert: CERT_PEM, key: KEY_PEM },
    (
      stream: EventSource & {
        respond(headers: Record<string, string | number>): void;
        end(chunk?: string): void;
      },
      headers: Record<string, string>,
    ) => {
      received.method = headers[constants.HTTP2_HEADER_METHOD] ?? '';
      received.path = headers[constants.HTTP2_HEADER_PATH] ?? '';
      stream.respond({
        [constants.HTTP2_HEADER_STATUS]: constants.HTTP_STATUS_OK,
        'content-type': 'text/plain',
      });
      stream.end('h2-resource-body');
    },
  ) as Http2ServerLike;

  await listen(server);
  const address = server.address();
  assert(
    address && typeof address.port === 'number' && address.port > 0,
    'server should listen on an ephemeral port',
  );

  const session = nativeConnect(`https://127.0.0.1:${address.port}`, { ca: CERT_PEM });
  const stream = session.request({
    [constants.HTTP2_HEADER_METHOD]: 'GET',
    [constants.HTTP2_HEADER_PATH]: '/resource',
  });

  const { promise, resolve, reject } = Promise.withResolvers<void>();
  let status = 0;
  stream.on('response', (headers: unknown) => {
    if (headers && typeof headers === 'object') {
      const statusHeader = (headers as Record<string, unknown>)[constants.HTTP2_HEADER_STATUS];
      status = typeof statusHeader === 'number' ? statusHeader : 0;
    }
  });
  void collectBody(stream).then((body) => {
    assertEqual(status, constants.HTTP_STATUS_OK);
    assertEqual(body, 'h2-resource-body');
    assertDeepEqual(received, { method: 'GET', path: '/resource' });
    resolve();
  }, reject);
  stream.on('error', reject);
  stream.end();
  await promise;
  session.close();
  server.close();
});

test('node:http2 aborts a native request via AbortSignal', async () => {
  const server = nativeCreateSecureServer(
    { cert: CERT_PEM, key: KEY_PEM },
    (
      stream: EventSource & {
        respond(headers: Record<string, string | number>): void;
        end(chunk?: string): void;
      },
    ) => {
      setTimeout(() => {
        stream.respond({
          [constants.HTTP2_HEADER_STATUS]: constants.HTTP_STATUS_OK,
          'content-type': 'text/plain',
        });
        stream.end('late-body');
      }, 200);
    },
  ) as Http2ServerLike;
  await listen(server);
  const address = server.address();
  assert(address && typeof address.port === 'number' && address.port > 0);

  const session = nativeConnect(`https://127.0.0.1:${address.port}`, { ca: CERT_PEM });
  const controller = new AbortController();
  const stream = session.request(
    {
      [constants.HTTP2_HEADER_METHOD]: 'GET',
      [constants.HTTP2_HEADER_PATH]: '/large',
    },
    { signal: controller.signal },
  );

  const { promise, resolve, reject } = Promise.withResolvers<void>();
  stream.on('response', () => reject(new Error('request should abort before response')));
  stream.on('error', (error: unknown) => {
    const abort = error as { name?: string; code?: string };
    assert(abort?.name === 'AbortError' || abort?.code === 'ABORT_ERR');
    resolve();
  });
  stream.end();
  await Promise.resolve();
  controller.abort();
  await promise;
  session.close();
  server.close();
});

await run();
