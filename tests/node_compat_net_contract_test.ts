// Native TCP/UDP host adapters are wired for basic socket IO. This offline
// contract keeps factory exports and address helpers observable without network connections.
// @ts-ignore FXE host-backed builtin

// @ts-ignore FXE host-backed builtin
import dgramDefault, { createSocket } from 'node:dgram';
import netDefault, { connect, createConnection, createServer, isIP, Server } from 'node:net';

import { assertEqual, run, test } from './ts_harness.ts';

test('node:net address helpers remain pure', () => {
  assertEqual(isIP('127.0.0.1'), 4);
  assertEqual(isIP('::1'), 6);
  assertEqual(isIP('not an address'), 0);
  assertEqual(netDefault.isIP('127.0.0.1'), 4);
});

test('node:net connection factories are exposed by host adapter', () => {
  assertEqual(typeof connect, 'function');
  assertEqual(typeof createConnection, 'function');
  assertEqual(typeof netDefault.connect, 'function');
  assertEqual(typeof netDefault.createConnection, 'function');
});

test('node:net server factories are exposed by host adapter', () => {
  assertEqual(typeof createServer, 'function');
  assertEqual(typeof netDefault.createServer, 'function');
  assertEqual(typeof Server, 'function');
});

test('node:dgram socket factory is exposed by host adapter', () => {
  assertEqual(typeof createSocket, 'function');
  assertEqual(typeof dgramDefault.createSocket, 'function');
});

await run();
