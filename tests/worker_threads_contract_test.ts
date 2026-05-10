import * as workerThreads from 'node:worker_threads';
import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

const {
  BroadcastChannel,
  MessageChannel,
  MessagePort,
  Worker,
  capabilities,
  isMainThread,
  parentPort,
  threadId,
  workerData,
} = workerThreads;

const nativeWorker = globalThis.__fxe_native?.worker;

test('node:worker_threads exposes honest main-thread shape', () => {
  assertEqual(isMainThread, true);
  assertEqual(threadId, 0);
  assertEqual(parentPort, null);
  assertEqual(workerData, undefined);
  assertEqual(typeof Worker, 'function');
  assertEqual(typeof MessageChannel, 'function');
  assertEqual(typeof MessagePort, 'function');
  if (BroadcastChannel !== undefined) {
    assertEqual(typeof BroadcastChannel, 'function');
  }
});

test('__fxe_native.worker exposes available native worker runtime boundary', () => {
  assert(nativeWorker !== undefined, 'missing __fxe_native.worker namespace');
  assertEqual(nativeWorker.notImplemented, false);
  assertEqual(nativeWorker.available, true);
  assertEqual(nativeWorker.isMainThread, true);
  assertEqual(nativeWorker.threadId, 0);
  for (const name of [
    'start',
    'createWorker',
    'postMessage',
    'drainMessages',
    'terminate',
    'ref',
    'unref',
  ] as const) {
    const method = nativeWorker[name];
    assert(method !== undefined, `missing __fxe_native.worker.${name}`);
    assertEqual(typeof method, 'function');
    assertEqual(method.notImplemented, undefined);
  }
});

test('node:worker_threads reports adapter capabilities without claiming cross-worker channels', () => {
  assertEqual(capabilities.worker, true);
  assertEqual(capabilities.sameIsolateMessageChannel, typeof MessageChannel === 'function');
  assertEqual(capabilities.sameIsolateBroadcastChannel, typeof BroadcastChannel === 'function');
  assertEqual(capabilities.native, nativeWorker);
});

test('MessageChannel delivers same-isolate messages when available', async () => {
  if (typeof MessageChannel !== 'function') {
    return;
  }

  const channel = new MessageChannel();
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const timeoutPromise = new Promise<never>((_, reject) => {
    timeout = setTimeout(() => reject(new Error('MessageChannel did not deliver message')), 50);
  });
  const received = new Promise<unknown>((resolve) => {
    channel.port2.onmessage = (event: { data: unknown }) => resolve(event.data);
    channel.port2.start?.();
  });

  channel.port1.postMessage({ kind: 'contract', value: 5 });
  const result = await Promise.race([received, timeoutPromise]);
  if (timeout !== undefined) {
    clearTimeout(timeout);
  }

  assertDeepEqual(result, { kind: 'contract', value: 5 });
  channel.port1.close?.();
  channel.port2.close?.();
});

test('BroadcastChannel delivers same-isolate broadcasts when available', async () => {
  if (typeof BroadcastChannel !== 'function') {
    return;
  }

  const left = new BroadcastChannel('fxe-worker-contract');
  const right = new BroadcastChannel('fxe-worker-contract');
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const timeoutPromise = new Promise<never>((_, reject) => {
    timeout = setTimeout(() => reject(new Error('BroadcastChannel did not deliver message')), 50);
  });
  const received = new Promise<unknown>((resolve) => {
    right.onmessage = (event: { data: unknown }) => resolve(event.data);
  });

  left.postMessage({ kind: 'broadcast', value: 7 });
  const result = await Promise.race([received, timeoutPromise]);
  if (timeout !== undefined) {
    clearTimeout(timeout);
  }

  assertDeepEqual(result, { kind: 'broadcast', value: 7 });
  left.close();
  right.close();
});

test('Worker runs a separate worker isolate and exchanges serialized messages', async () => {
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const timeoutPromise = new Promise<never>((_, reject) => {
    timeout = setTimeout(() => reject(new Error('Worker did not deliver smoke message')), 500);
  });
  const workerFile = decodeURIComponent(import.meta.url.replace(/^file:\/\//, '')).replace(
    /worker_threads_contract_test\.ts$/,
    'worker_threads_smoke_worker.ts',
  );
  const worker = new Worker(workerFile, {
    workerData: { hello: 'worker' },
  });
  const received = new Promise<unknown>((resolve, reject) => {
    worker.on('message', (data: unknown) => resolve(data));
    worker.on('error', (error: Error) => reject(error));
  });

  const result = await Promise.race([received, timeoutPromise]);
  if (timeout !== undefined) {
    clearTimeout(timeout);
  }

  assertDeepEqual(result, {
    kind: 'fxe-worker-smoke',
    threadId: 1,
    workerData: { hello: 'worker' },
  });
  await worker.terminate();
});

await run();
