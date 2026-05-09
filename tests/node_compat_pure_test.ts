/// <reference path="./node_compat_pure_types.d.ts" />

import assertStrict from 'node:assert/strict';
import { AsyncLocalStorage } from 'node:async_hooks';
import { Buffer } from 'node:buffer';
import { EventEmitter } from 'node:events';
import { createRequire } from 'node:module';
import pathDefault, { basename, join } from 'node:path';
import querystring from 'node:querystring';
import { Readable } from 'node:stream';
import { text } from 'node:stream/consumers';
import { setTimeout as sleep } from 'node:timers/promises';
import { URL, URLSearchParams } from 'node:url';

import { assert, assertDeepEqual, assertEqual, assertThrows, run, test } from './ts_harness.ts';

test('events EventEmitter emits and removes listeners', () => {
  const emitter = new EventEmitter();
  let total = 0;
  const listener = (value: unknown) => {
    assert(typeof value === 'number', `expected numeric event value, got ${typeof value}`);
    total += value;
  };

  emitter.on('add', listener);
  assertEqual(emitter.emit('add', 2), true);
  emitter.off('add', listener);
  assertEqual(emitter.emit('add', 4), false);
  assertEqual(total, 2);
});

test('node:path supports default and named imports', () => {
  assertEqual(pathDefault.basename('/tmp/fxe/node.txt'), 'node.txt');
  assertEqual(basename('/tmp/fxe/node.txt', '.txt'), 'node');
  assertEqual(join('alpha', 'beta', '..', 'gamma.txt'), 'alpha/gamma.txt');
});

test('Buffer supports utf8 to hex and hex round-trip', () => {
  const hex = Buffer.from('hi', 'utf8').toString('hex');
  assertEqual(hex, '6869');
  assertEqual(Buffer.from(hex, 'hex').toString('utf8'), 'hi');
});

test('node:assert/strict throws assertion errors', () => {
  const strictEqual =
    (assertStrict as { strictEqual?: typeof assertStrict.equal }).strictEqual ?? assertStrict.equal;
  strictEqual(2 + 2, 4);

  let thrown: unknown;
  try {
    strictEqual('actual', 'expected');
  } catch (error) {
    thrown = error;
  }

  assert(thrown !== undefined, 'expected strictEqual to throw');
  const error = thrown as { code?: unknown; name?: unknown; constructor?: { name?: unknown } };
  assert(
    error.code === 'ERR_ASSERTION' ||
      error.name === 'AssertionError' ||
      error.constructor?.name === 'AssertionError',
    `expected assertion error object, got ${String(error.name ?? error.constructor?.name ?? error.code)}`,
  );
});

test('node:url URL and URLSearchParams parse and mutate', () => {
  const url = new URL('https://example.test/search?q=fxe#top');
  assertEqual(url.hostname, 'example.test');
  assertEqual(url.searchParams.get('q'), 'fxe');

  const params = new URLSearchParams([['a', '1']]);
  params.append('a', '2');
  params.set('b', 'space value');
  assertEqual(params.getAll('a').join(','), '1,2');
  assertEqual(params.toString(), 'a=1&a=2&b=space+value');
});

test('querystring parses and stringifies values', () => {
  const parsed = querystring.parse('a=1&a=2&b=space%20value');
  assertDeepEqual(parsed, { a: ['1', '2'], b: 'space value' });
  assertEqual(
    querystring.stringify({ a: ['1', '2'], b: 'space value' }),
    'a=1&a=2&b=space%20value',
  );
});

test('node:timers/promises setTimeout resolves with a value', async () => {
  assertEqual(await sleep(0, 'done'), 'done');
});

test('AsyncLocalStorage supports synchronous run and enterWith stores', () => {
  const storage = new AsyncLocalStorage<{ traceId: string }>();
  assertEqual(storage.getStore(), undefined);

  const value = storage.run({ traceId: 'run' }, () => {
    assertEqual(storage.getStore()?.traceId, 'run');
    return storage.getStore()?.traceId;
  });

  assertEqual(value, 'run');
  assertEqual(storage.getStore(), undefined);

  const storageWithEnter = storage as AsyncLocalStorage<{ traceId: string }> & {
    enterWith(store: { traceId: string }): void;
  };
  storageWithEnter.enterWith({ traceId: 'entered' });
  assertEqual(storage.getStore()?.traceId, 'entered');
});

test('AsyncLocalStorage preserves stores across await', async () => {
  const storage = new AsyncLocalStorage<{ id: string }>();
  await storage.run({ id: 'A' }, async () => {
    await new Promise((resolve) => {
      void sleep(10).then(resolve);
    });
    const current = storage.getStore();
    if (!current || current.id !== 'A') {
      throw new Error(`ALS lost across await: ${JSON.stringify(current)}`);
    }
  });
});

test('Readable.from surface is available and consumers.text reports not implemented', () => {
  const stream = Readable.from(['hello', ' ', Buffer.from('world')]);
  assert(stream !== undefined, 'expected Readable.from to return a stream');
  assertThrows(() => {
    void text(stream);
  }, /stream\.consumers\.text|not implemented/i);
});

test('createRequire returns a function and unsupported require throws clearly', () => {
  const require = createRequire(import.meta.url);
  assertEqual(typeof require, 'function');
  assertThrows(
    () => require('definitely-unsupported-fxe-module'),
    /require|unsupported|not implemented|Cannot find/i,
  );
});

await run();
