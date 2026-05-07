import { assert, assertEqual, delay, run, test } from './ts_harness.ts';

type StdinLike = {
  fd: number;
  isTTY: boolean;
  readable?: boolean;
  readableEncoding?: string | null;
  setEncoding(enc: string): StdinLike;
  on(event: 'data' | 'end' | 'error', fn: (...args: unknown[]) => void): StdinLike;
  once(event: 'data' | 'end' | 'error', fn: (...args: unknown[]) => void): StdinLike;
  off(event: 'data' | 'end' | 'error', fn: (...args: unknown[]) => void): StdinLike;
  removeListener(event: 'data' | 'end' | 'error', fn: (...args: unknown[]) => void): StdinLike;
  resume(): StdinLike;
  pause(): StdinLike;
  [Symbol.asyncIterator]?: () => AsyncIterator<unknown>;
};

function stdin(): StdinLike {
  return process.stdin as unknown as StdinLike;
}

test('process.stdin exposes minimal truthful stream shape', () => {
  const input = stdin();
  assert(input && typeof input === 'object', 'stdin should be an object');
  assertEqual(input.fd, 0, 'stdin fd should be 0');
  assertEqual(typeof input.isTTY, 'boolean', 'stdin isTTY should be boolean');
  assertEqual(typeof input.setEncoding, 'function');
  assertEqual(typeof input.on, 'function');
  assertEqual(typeof input.once, 'function');
  assertEqual(typeof input.off, 'function');
  assertEqual(typeof input.removeListener, 'function');
  assertEqual(typeof input.resume, 'function');
  assertEqual(typeof input.pause, 'function');
});

test('process.stdin control methods are chainable and non-blocking', async () => {
  const input = stdin();
  const listener = () => {};

  assertEqual(input.setEncoding('utf8'), input, 'setEncoding should be chainable');
  assertEqual(input.readableEncoding, 'utf8', 'setEncoding should record requested encoding');
  assertEqual(input.on('data', listener), input, 'on should be chainable');
  assertEqual(input.once('end', listener), input, 'once should be chainable');
  assertEqual(input.off('data', listener), input, 'off should be chainable');
  assertEqual(input.removeListener('end', listener), input, 'removeListener should be chainable');
  assertEqual(input.pause(), input, 'pause should be chainable');
  assertEqual(input.resume(), input, 'resume should be chainable');

  await delay(0);
  input.pause();
});

test('process.stdin flowing mode emits only observed chunks or end', async () => {
  const input = stdin();
  let ended = false;
  input.setEncoding('utf8');
  input.on('data', (chunk) => {
    assertEqual(typeof chunk, 'string', 'encoded stdin data should be delivered as text');
    assert((chunk as string).length > 0, 'stdin data event must not fabricate empty chunks');
  });
  input.once('end', () => {
    ended = true;
  });

  assertEqual(input.resume(), input, 'resume should be chainable');
  await delay(20);
  assertEqual(input.pause(), input, 'pause should be chainable');
  if (ended) {
    assertEqual(input.readable, false, 'ended stdin should no longer be readable');
  }
});

test('process.stdin async iterator is present when supported and does not read data', async () => {
  const input = stdin();
  const asyncIterator = input[Symbol.asyncIterator];
  if (asyncIterator === undefined) {
    return;
  }

  const iterator = asyncIterator.call(input);
  assert(iterator && typeof iterator.next === 'function', 'async iterator should expose next()');
  const next = await iterator.next();
  assertEqual(next.done, true, 'stub iterator should finish without fabricating chunks');
  assertEqual(next.value, undefined, 'stub iterator should not fabricate a value');
});

await run();
