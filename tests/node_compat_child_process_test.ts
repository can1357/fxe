// Native node:child_process compatibility tests intentionally import FXE host-backed builtins.
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import { lookup, resolve4 } from 'node:dns';

import { assert, assertEqual, run, test } from './ts_harness.ts';

type ChildLike = {
  pid: number;
  killed: boolean;
  stdout: {
    setEncoding(encoding: string): unknown;
    on(event: 'data' | 'end', listener: (chunk?: string) => void): unknown;
  };
  stderr: {
    setEncoding(encoding: string): unknown;
    on(event: 'data' | 'end', listener: (chunk?: string) => void): unknown;
  };
  on(event: 'close' | 'exit' | 'error', listener: (...args: unknown[]) => void): unknown;
  kill(signal?: string): boolean;
};

function closeOf(
  child: ChildLike,
): Promise<{ code: number | null; signal: number | string | null }> {
  const { promise, resolve, reject } = Promise.withResolvers<{
    code: number | null;
    signal: number | string | null;
  }>();
  child.on('error', reject);
  child.on('close', (code: unknown, signal: unknown) => {
    resolve({ code: code as number | null, signal: signal as number | string | null });
  });
  return promise;
}

function shellPrintArgs(text: string): [string, string[]] {
  if (process.platform === 'win32') {
    return ['cmd.exe', ['/d', '/s', '/c', `<nul set /p=${text}`]];
  }
  return ['/bin/sh', ['-c', `printf ${text}`]];
}

function envCommand(): [string, string[]] {
  if (process.platform === 'win32') {
    return ['cmd.exe', ['/d', '/s', '/c', 'set']];
  }
  return ['/usr/bin/env', []];
}

function sleepCommand(): [string, string[]] {
  if (process.platform === 'win32') {
    return ['cmd.exe', ['/d', '/s', '/c', 'ping -n 6 127.0.0.1 >NUL']];
  }
  return ['/bin/sleep', ['5']];
}

test('node:child_process spawn captures stdout and exit code', async () => {
  const [file, args] = envCommand();
  const child = spawn(file, args) as ChildLike;
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => {
    stdout += String(chunk);
  });
  child.stderr.on('data', (chunk) => {
    stderr += String(chunk);
  });
  const status = await closeOf(child);

  assertEqual(status.code, 0, stderr || 'environment command should exit successfully');
  assert(
    stdout.includes('PATH=') || stdout.includes('Path=') || stdout.length > 0,
    'environment command should produce stdout',
  );
});

test('node:child_process spawnSync captures stdout and status', () => {
  const [file, args] = shellPrintArgs('sync-out');
  const result = spawnSync(file, args) as {
    status: number | null;
    stdout: string;
    stderr: string;
  };
  assertEqual(result.status, 0, result.stderr || 'spawnSync should exit successfully');
  assertEqual(result.stdout, 'sync-out');
});

test('node:child_process execFileSync returns stdout', () => {
  const [file, args] = shellPrintArgs('exec-out');
  assertEqual(String(execFileSync(file, args)), 'exec-out');
});

test('node:child_process kill terminates a long-running child', async () => {
  const [file, args] = sleepCommand();
  const child = spawn(file, args) as ChildLike;
  const statusPromise = closeOf(child);
  setTimeout(() => {
    child.kill('SIGTERM');
  }, 20);
  const status = await statusPromise;

  assertEqual(child.killed, true, 'kill should report signal delivery');
  assert(
    status.code !== 0 || status.signal !== null,
    'killed child should not report a clean zero exit',
  );
});

test('node:dns lookup all and resolve4 use host DNS results', async () => {
  const lookupResult = await new Promise<Array<{ address: string; family: number }>>(
    (resolve, reject) => {
      lookup(
        'localhost',
        { family: 4, all: true },
        (error: unknown, addresses?: Array<{ address: string; family: number }>) => {
          if (error) {
            reject(error);
          } else {
            resolve(addresses ?? []);
          }
        },
      );
    },
  );
  assert(lookupResult.length > 0, 'lookup all should return at least one IPv4 localhost address');
  assert(
    lookupResult.every((entry) => entry.family === 4),
    'lookup all should honor family: 4',
  );

  const resolved = await new Promise<string[]>((resolve, reject) => {
    resolve4('localhost', (error: unknown, addresses?: string[]) => {
      if (error) {
        reject(error);
      } else {
        resolve(addresses ?? []);
      }
    });
  });
  assert(resolved.includes('127.0.0.1'), 'resolve4 localhost should include 127.0.0.1');
});

await run();
