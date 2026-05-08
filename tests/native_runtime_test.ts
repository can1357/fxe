import { spawn } from 'node:child_process';
import { mkdirSync, rmSync, watch, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { assert, assertEqual, run, test } from './ts_harness.ts';

type ChildLike = {
  stdin: { write(chunk: string): boolean; end(chunk?: string): boolean };
  stdout: {
    on(event: 'data' | 'end', fn: (chunk?: Uint8Array | string) => void): unknown;
    setEncoding?(encoding: string): unknown;
  };
  on(event: 'close' | 'exit' | 'error', fn: (...args: unknown[]) => void): unknown;
};

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
  const { promise: timeoutPromise, reject } = Promise.withResolvers<never>();
  const timer = setTimeout(() => reject(new Error(`${label} timed out`)), ms);
  return Promise.race([promise, timeoutPromise]).finally(() => clearTimeout(timer));
}

function chunkString(chunk: Uint8Array | string | undefined): string {
  if (chunk === undefined) return '';
  if (typeof chunk === 'string') return chunk;
  return Array.from(chunk, (byte) => String.fromCharCode(byte)).join('');
}

test('native child_process round-trips stdin to stdout', async () => {
  if (process.platform === 'win32') return;
  const child = spawn('/bin/sh', ['-c', 'cat']) as ChildLike;
  let stdout = '';
  child.stdout.on('data', (chunk) => {
    stdout += chunkString(chunk);
  });
  const { promise: closed, resolve, reject } = Promise.withResolvers<number | null>();
  child.on('error', reject);
  child.on('close', (code) => resolve(code as number | null));
  child.stdin.write('fxe-native-stdin');
  child.stdin.end();
  assertEqual(await withTimeout(closed, 1000, 'child stdin round-trip'), 0);
  assertEqual(stdout, 'fxe-native-stdin');
});

test('native fs.watch reports file changes', async () => {
  const dir = join(process.cwd(), `.fxe-native-watch-${process.pid}`);
  const file = join(dir, 'watched.txt');
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  writeFileSync(file, 'before');
  try {
    const { promise: event, resolve } = Promise.withResolvers<[string, string]>();
    const watcher = watch(file, (eventType: string, filename: string) => {
      watcher.close();
      resolve([eventType, filename]);
    }) as { close(): void };
    setTimeout(() => writeFileSync(file, 'after'), 25);
    const [eventType, filename] = await withTimeout(event, 1500, 'fs.watch change');
    assert(
      eventType === 'change' || eventType === 'rename',
      `unexpected fs.watch event ${eventType}`,
    );
    assertEqual(filename, 'watched.txt');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('native child_process propagates exit code and stdout', async () => {
  if (process.platform === 'win32') return;
  const child = spawn('/bin/sh', ['-c', 'printf fxe-child; exit 7']) as ChildLike;
  let stdout = '';
  child.stdout.on('data', (chunk) => {
    stdout += chunkString(chunk);
  });
  const { promise: closed, resolve, reject } = Promise.withResolvers<number | null>();
  child.on('error', reject);
  child.on('close', (exitCode) => resolve(exitCode as number | null));
  const code = await withTimeout(closed, 1000, 'child exit');
  assertEqual(stdout, 'fxe-child');
  assertEqual(code, 7);
});

await run();
