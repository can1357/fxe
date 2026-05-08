// Native node:fs fd compatibility tests intentionally import FXE host-backed builtins.
import { open } from 'node:fs';

import { assert, assertEqual, run, test } from './ts_harness.ts';

test('node:fs fd open rejects missing paths with libuv error details', async () => {
  const missing = `${process.cwd()}/.fxe-missing-fd-${process.pid}-${Date.now()}`;
  try {
    await open(missing, 'r');
  } catch (error) {
    const err = error as Error & { code?: string; syscall?: string; path?: string };
    assertEqual(err.code, 'ENOENT');
    assertEqual(err.syscall, 'open');
    assertEqual(err.path, missing);
    assert(err.message.includes('ENOENT'), 'message should include libuv error name');
    assert(
      err.message.toLowerCase().includes('no such file'),
      'message should include libuv strerror',
    );
    return;
  }
  throw new Error('expected open of missing path to reject');
});

await run();
