import { Buffer } from 'node:buffer';
import { Console } from 'node:console';
import { EventEmitter, once } from 'node:events';
import path, { posix, win32 } from 'node:path';
import processModule, { cwd, hrtime, kill, nextTick, release, umask } from 'node:process';
import querystring from 'node:querystring';
import { finished, pipeline, Readable, Transform, Writable } from 'node:stream';
import { setTimeout as sleep } from 'node:timers/promises';
import { fileURLToPath, format as formatUrl, pathToFileURL, URL, URLSearchParams } from 'node:url';
import { format, promisify, types } from 'node:util';

import { assert, assertDeepEqual, assertEqual, assertThrows, run, test } from './ts_harness.ts';

test('node:events implements listener lifecycle and error semantics', async () => {
  const emitter = new EventEmitter();
  let total = 0;
  const listener = (value: number) => {
    total += value;
  };
  emitter.on('value', listener);
  emitter.once('value', listener);
  assertEqual(emitter.emit('value', 2), true);
  assertEqual(emitter.emit('value', 3), true);
  assertEqual(total, 7);
  assertEqual(emitter.listenerCount('value'), 1);
  emitter.removeAllListeners('value');
  assertEqual(emitter.emit('value', 1), false);
  assertThrows(() => emitter.emit('error', new Error('boom')), 'boom');

  const awaited = once(emitter, 'ready');
  emitter.emit('ready', 'ok');
  assertDeepEqual(await awaited, ['ok']);
});

test('node:buffer exposes host Buffer helpers', () => {
  const a = Buffer.from('fx', 'utf8');
  const b = Buffer.alloc(1, 'e');
  const c = Buffer.concat([a, b]);
  assert(Buffer.isBuffer(c));
  assertEqual(c.toString('utf8'), 'fxe');
  assertEqual(Buffer.from('667865', 'hex').toString(), 'fxe');
});

test('node:process re-exports host process surface', async () => {
  assertEqual(processModule.cwd(), cwd());
  assertEqual(typeof release.name, 'string');
  assertEqual(kill(processModule.pid, 0), true);
  const before = umask();
  assertEqual(umask(), before);
  const start = hrtime();
  const ns = hrtime.bigint();
  assertEqual(typeof ns, 'bigint');
  const delta = hrtime(start);
  assert(delta[0] >= 0 && delta[1] >= 0);
  let ticked = false;
  await new Promise<void>((resolve) =>
    nextTick(() => {
      ticked = true;
      resolve();
    }),
  );
  assertEqual(ticked, true);
});

test('node:path exposes host, posix, and win32 namespaces', () => {
  assertEqual(posix.join('/tmp', 'fxe', '..', 'node'), '/tmp/node');
  assertEqual(win32.basename('C:\\tmp\\fxe.txt', '.txt'), 'fxe');
  assertEqual(typeof path.sep, 'string');
  assertEqual(path.parse('/tmp/fxe.txt').ext, '.txt');
});

test('node:url exposes URL globals and legacy helpers', () => {
  const u = new URL('https://example.test/path?a=1');
  const params = new URLSearchParams([['b', '2']]);
  assertEqual(u.hostname, 'example.test');
  assertEqual(params.toString(), 'b=2');
  assertEqual(
    formatUrl({ protocol: 'https:', host: 'example.test', pathname: '/ok', query: { a: '1' } }),
    'https://example.test/ok?a=1',
  );
  const file = pathToFileURL('/tmp/fxe file.txt');
  assertEqual(fileURLToPath(file), '/tmp/fxe file.txt');
});

test('node:querystring parses and stringifies repeated fields', () => {
  const parsed = querystring.parse('a=1&a=2&b=hello+world');
  assertDeepEqual(parsed, { a: ['1', '2'], b: 'hello world' });
  assertEqual(
    querystring.stringify({ a: ['1', '2'], b: 'hello world' }),
    'a=1&a=2&b=hello%20world',
  );
});

test('node:util exposes format, promisify, and types', async () => {
  assertEqual(format('hello %s %d', 'fxe', 7), 'hello fxe 7');
  const add = promisify((a: number, b: number, cb: (err: unknown, value?: number) => void) =>
    cb(null, a + b),
  ) as (a: number, b: number) => Promise<number>;
  assertEqual(await add(2, 5), 7);
  assertEqual(types.isUint8Array(new Uint8Array()), true);
});

test('node:console Console writes formatted output', () => {
  const lines: string[] = [];
  const c = new Console({
    write: (s: string) => {
      lines.push(s);
      return true;
    },
  });
  c.log('value=%d', 3);
  assertEqual(lines[0], 'value=3\n');
});

test('node:timers/promises resolves using host timers', async () => {
  assertEqual(await sleep(1, 'timer-ok'), 'timer-ok');
});

test('node:stream supports Readable, Transform, Writable, pipeline, and finished', async () => {
  const out: string[] = [];
  const readable = Readable.from(['a', 'b']);
  const upper = new Transform({
    transform(chunk: unknown, _encoding: string, cb: (err?: unknown, data?: string) => void) {
      cb(undefined, String(chunk).toUpperCase());
    },
  });
  const writable = new Writable({
    write(chunk: unknown, _encoding: string, cb: () => void) {
      out.push(String(chunk));
      cb();
    },
  });
  await pipeline(readable, upper, writable);
  await finished(writable);
  assertDeepEqual(out, ['A', 'B']);
});

await run();
