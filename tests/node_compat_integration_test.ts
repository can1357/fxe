/// <reference path="./node_compat_pure_types.d.ts" />

import { Buffer } from 'node:buffer';
import { createHash, randomBytes } from 'node:crypto';
import { EventEmitter } from 'node:events';
import * as fsPromises from 'node:fs/promises';
import pathDefault, { basename, join } from 'node:path';
import * as workerThreads from 'node:worker_threads';

import { assertEqual, run, test } from './ts_harness.ts';

test('node compat integration resolves pure and host-backed modules together', () => {
  assertEqual(pathDefault.basename('/tmp/fxe/integration.txt'), 'integration.txt');
  assertEqual(basename('/tmp/fxe/integration.txt', '.txt'), 'integration');
  assertEqual(join('tmp', 'fxe', '..', 'node'), 'tmp/node');

  const emitter = new EventEmitter();
  let seen = '';
  emitter.on('value', (value: unknown) => {
    seen = String(value);
  });
  assertEqual(emitter.emit('value', 'events-ok'), true);
  assertEqual(seen, 'events-ok');

  const buf = Buffer.from('fxe', 'utf8');
  assertEqual(buf.toString('hex'), '667865');
});

test('host-backed adapters expose representative API shape', () => {
  assertEqual(typeof fsPromises.readFile, 'function');
  assertEqual(typeof fsPromises.writeFile, 'function');

  assertEqual(randomBytes(4).length, 4);
  assertEqual(
    createHash('sha256').update('abc').digest('hex'),
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
  );

  assertEqual(workerThreads.isMainThread, true);
  assertEqual(workerThreads.threadId, 0);
  assertEqual(workerThreads.parentPort, null);
  assertEqual(typeof workerThreads.MessageChannel, 'function');
  assertEqual(typeof workerThreads.Worker, 'function');
});

await run();
