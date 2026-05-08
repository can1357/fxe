import dgramDefault, { createSocket } from 'node:dgram';
import dnsDefault, { lookup } from 'node:dns';
import dnsPromises from 'node:dns/promises';
import netDefault, { createConnection } from 'node:net';

import { assert, assertEqual, run, test } from './ts_harness.ts';

type LookupAddress = { address: string; family: number };

function lookupAddress(
  hostname: string,
  options?: { family?: number; all?: false },
): Promise<LookupAddress> {
  const { promise, resolve, reject } = Promise.withResolvers<LookupAddress>();
  lookup(hostname, options ?? {}, (err: Error | null, address?: string, family?: number) => {
    if (err) {
      reject(err);
      return;
    }
    resolve({ address: String(address), family: Number(family) });
  });
  return promise;
}

function lookupAll(hostname: string): Promise<LookupAddress[]> {
  const { promise, resolve, reject } = Promise.withResolvers<LookupAddress[]>();
  dnsDefault.lookup(hostname, { all: true }, (err: Error | null, addresses?: LookupAddress[]) => {
    if (err) {
      reject(err);
      return;
    }
    resolve(addresses ?? []);
  });
  return promise;
}

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
  const { promise: timeoutPromise, reject } = Promise.withResolvers<never>();
  const timeout = setTimeout(() => reject(new Error(`${label} timed out after ${ms}ms`)), ms);
  return Promise.race([promise, timeoutPromise]).finally(() => clearTimeout(timeout));
}

function onceEvent<T>(
  target: { once: (name: string, fn: (...args: unknown[]) => void) => unknown },
  name: string,
): Promise<T> {
  const { promise, resolve } = Promise.withResolvers<T>();
  target.once(name, (...args: unknown[]) => resolve(args as T));
  return promise;
}

function bytesToString(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => String.fromCharCode(byte)).join('');
}

test('node:dns lookup resolves localhost through callback wrapper', async () => {
  const result = await lookupAddress('localhost');
  assert(
    typeof result.address === 'string' && result.address.length > 0,
    'dns.lookup should return an address',
  );
  assert(result.family === 4 || result.family === 6, `unexpected address family: ${result.family}`);
});

test('node:dns default lookup supports all:true shape', async () => {
  const results = await lookupAll('localhost');
  assert(results.length > 0, 'dns.lookup all:true should return at least one localhost address');
  for (const result of results) {
    assert(
      typeof result.address === 'string' && result.address.length > 0,
      'all:true address should be a string',
    );
    assert(
      result.family === 4 || result.family === 6,
      `unexpected all:true family: ${result.family}`,
    );
  }
});

test('node:dns resolve and resolveAny expose address records', async () => {
  const {
    promise: resolvePromise,
    resolve: resolveA,
    reject: rejectA,
  } = Promise.withResolvers<string[]>();
  dnsDefault.resolve('localhost', 'A', (err: Error | null, addresses?: string[]) => {
    if (err) {
      rejectA(err);
      return;
    }
    resolveA(addresses ?? []);
  });
  const resolved = await resolvePromise;
  assert(resolved.includes('127.0.0.1'), 'dns.resolve A localhost should include 127.0.0.1');

  const {
    promise: anyPromise,
    resolve: resolveAnyRecords,
    reject: rejectAnyRecords,
  } = Promise.withResolvers<Array<{ address: string; family: number; type: string }>>();
  dnsDefault.resolveAny(
    'localhost',
    (err: Error | null, records?: Array<{ address: string; family: number; type: string }>) => {
      if (err) {
        rejectAnyRecords(err);
        return;
      }
      resolveAnyRecords(records ?? []);
    },
  );
  const anyRecords = await anyPromise;
  assert(
    anyRecords.some((record) => record.type === 'A' && record.address === '127.0.0.1'),
    'dns.resolveAny localhost should expose A records',
  );
});

test('node:dns/promises lookup and resolve4 use host DNS', async () => {
  const lookupResult = await dnsPromises.lookup('localhost', { family: 4 });
  assertEqual(lookupResult.family, 4);
  assertEqual(lookupResult.address, '127.0.0.1');

  const resolved = await dnsPromises.resolve4('localhost');
  assert(
    resolved.includes('127.0.0.1'),
    'dns/promises.resolve4 localhost should include 127.0.0.1',
  );
});

test('node:net supports localhost TCP echo', async () => {
  const server = netDefault.createServer((socket: any) => {
    socket.on('data', (chunk: Uint8Array) => {
      socket.write(chunk);
      socket.end();
    });
  });

  const { promise: listenPromise, resolve: resolveListen } = Promise.withResolvers<void>();
  server.listen(0, '127.0.0.1', resolveListen);
  await withTimeout(listenPromise, 500, 'tcp listen');
  const rawAddress = server.address();
  assert(rawAddress && typeof rawAddress === 'object', 'server.address must return an object');
  const address = rawAddress;
  assert(address.address === '127.0.0.1', 'server.address should report localhost');
  assert(
    typeof address.port === 'number' && address.port > 0,
    'server.address should report ephemeral port',
  );

  const {
    promise: echoPromise,
    resolve: resolveEcho,
    reject: rejectEcho,
  } = Promise.withResolvers<string>();
  const client = createConnection({ port: address.port, host: '127.0.0.1' }, () => {
    client.write('fxe-tcp');
  });
  const chunks: string[] = [];
  client.on('data', (chunk: Uint8Array) => chunks.push(bytesToString(chunk)));
  client.on('error', rejectEcho);
  client.on('end', () => resolveEcho(chunks.join('')));
  const result = await withTimeout(echoPromise, 500, 'tcp echo');

  assertEqual(result, 'fxe-tcp');
  const { promise: closePromise, resolve: resolveClose } = Promise.withResolvers<void>();
  server.close(resolveClose);
  await withTimeout(closePromise, 500, 'tcp close');
});

test('node:dgram supports udp4 loopback send/receive', async () => {
  const socket = createSocket('udp4');
  assert(
    dgramDefault.createSocket === createSocket,
    'default dgram export should expose createSocket',
  );

  const { promise: bindPromise, resolve: resolveBind } = Promise.withResolvers<void>();
  socket.bind(0, '127.0.0.1', resolveBind);
  await withTimeout(bindPromise, 500, 'udp bind');
  const address = socket.address();
  assert(address && address.address === '127.0.0.1', 'udp address should report localhost');
  assert(
    typeof address.port === 'number' && address.port > 0,
    'udp address should report ephemeral port',
  );

  const received = withTimeout(
    onceEvent<[Uint8Array, { address: string; port: number }]>(socket, 'message'),
    500,
    'udp message',
  );
  const {
    promise: sendPromise,
    resolve: resolveSend,
    reject: rejectSend,
  } = Promise.withResolvers<void>();
  socket.send('fxe-udp', address.port, '127.0.0.1', (err: Error | null) => {
    if (err) rejectSend(err);
    else resolveSend();
  });
  await withTimeout(sendPromise, 500, 'udp send');

  const [message, rinfo] = await received;
  assertEqual(bytesToString(message), 'fxe-udp');
  assertEqual(rinfo.address, '127.0.0.1');
  socket.close();
});

test('node:dgram supports hostname resolution and bind options', async () => {
  const socket = createSocket('udp4');

  const { promise: bindPromise, resolve: resolveBind } = Promise.withResolvers<void>();
  socket.bind({ port: 0, address: 'localhost' } as any, resolveBind);
  await withTimeout(bindPromise, 500, 'udp hostname bind');
  const address = socket.address();
  assert(address && address.address === '127.0.0.1', 'udp4 localhost bind should prefer IPv4');
  assertEqual(address.family, 'IPv4');

  const received = withTimeout(
    onceEvent<[Uint8Array, { address: string; family: string; port: number; size: number }]>(
      socket,
      'message',
    ),
    500,
    'udp hostname message',
  );
  const {
    promise: sendPromise,
    resolve: resolveSend,
    reject: rejectSend,
  } = Promise.withResolvers<void>();
  socket.send(
    Uint8Array.from([102, 120, 101, 45, 104, 111, 115, 116, 110, 97, 109, 101]),
    address.port,
    'localhost',
    (err: Error | null) => {
      if (err) rejectSend(err);
      else resolveSend();
    },
  );
  await withTimeout(sendPromise, 500, 'udp hostname send');

  const [message, rinfo] = await received;
  assertEqual(bytesToString(message), 'fxe-hostname');
  assertEqual(rinfo.address, '127.0.0.1');
  assertEqual(rinfo.family, 'IPv4');
  assertEqual(rinfo.size, 'fxe-hostname'.length);

  const { promise: closePromise, resolve: resolveClose } = Promise.withResolvers<void>();
  socket.close(resolveClose);
  await withTimeout(closePromise, 500, 'udp hostname close');
  const { promise: secondClosePromise, resolve: resolveSecondClose } =
    Promise.withResolvers<void>();
  socket.close(resolveSecondClose);
  await withTimeout(secondClosePromise, 500, 'udp second close');

  const {
    promise: closedSendPromise,
    resolve: resolveClosedSend,
    reject: rejectClosedSend,
  } = Promise.withResolvers<void>();
  socket.send('closed', address.port, '127.0.0.1', (err: Error | null) => {
    if (err) resolveClosedSend();
    else rejectClosedSend(new Error('closed UDP send should report an error'));
  });
  await withTimeout(closedSendPromise, 500, 'udp closed send callback');
});

test('node:dgram supports udp6 loopback send/receive', async () => {
  const socket = createSocket('udp6');

  const { promise: bindPromise, resolve: resolveBind } = Promise.withResolvers<void>();
  socket.bind(0, '::1', resolveBind);
  await withTimeout(bindPromise, 500, 'udp6 bind');
  const address = socket.address();
  assert(address && address.address === '::1', 'udp6 address should report IPv6 loopback');
  assertEqual(address.family, 'IPv6');
  assert(
    typeof address.port === 'number' && address.port > 0,
    'udp6 address should report ephemeral port',
  );

  const received = withTimeout(
    onceEvent<[Uint8Array, { address: string; family: string; port: number; size: number }]>(
      socket,
      'message',
    ),
    500,
    'udp6 message',
  );
  const {
    promise: sendPromise,
    resolve: resolveSend,
    reject: rejectSend,
  } = Promise.withResolvers<void>();
  socket.send('fxe-udp6', address.port, '::1', (err: Error | null) => {
    if (err) rejectSend(err);
    else resolveSend();
  });
  await withTimeout(sendPromise, 500, 'udp6 send');

  const [message, rinfo] = await received;
  assertEqual(bytesToString(message), 'fxe-udp6');
  assertEqual(rinfo.address, '::1');
  assertEqual(rinfo.family, 'IPv6');
  assertEqual(rinfo.size, 'fxe-udp6'.length);
  socket.close();
});

await run();
