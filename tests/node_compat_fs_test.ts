// Native node:fs compatibility tests intentionally import FXE host-backed builtins.
// @ts-ignore FXE host-backed builtin
import fsDefault, {
  appendFile,
  appendFileSync,
  exists,
  existsSync,
  close,
  closeSync,
  createReadStream,
  createWriteStream,
  mkdirSync,
  readdirSync,
  readFile,
  readFileSync,
  realpathSync,
  fdatasyncSync,
  fstat,
  fstatSync,
  renameSync,
  rmSync,
  open,
  openSync,
  stat,
  statSync,
  read,
  readSync,
  watch,
  writeFile,
  write,
  writeFileSync,
  writeSync,
} from 'node:fs';
// @ts-ignore FXE host-backed builtin
import * as fsPromises from 'node:fs/promises';

import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

let tempCounter = 0;

function join(...parts: string[]): string {
  let out = parts[0] ?? '';
  for (const raw of parts.slice(1)) {
    const part = String(raw);
    if (out.endsWith('/')) {
      out += part.startsWith('/') ? part.slice(1) : part;
    } else {
      out += part.startsWith('/') ? part : `/${part}`;
    }
  }
  return out;
}

function tempRoot(label: string): string {
  return join(process.cwd(), `.fxe-node-compat-fs-${label}-${process.pid}-${++tempCounter}`);
}

async function withTempDir(name: string, fn: (dir: string) => void | Promise<void>): Promise<void> {
  const dir = tempRoot(name);
  mkdirSync(dir, { recursive: true });
  try {
    await fn(dir);
  } finally {
    try {
      rmSync(dir, { recursive: true, force: true });
    } catch {
      // Best-effort cleanup only.
    }
  }
}

function bytes(value: Uint8Array): number[] {
  return Array.from(value);
}

function sorted(values: string[]): string[] {
  return [...values].sort();
}

function assertStatFile(
  value: { size: number; isFile: boolean; isDirectory: boolean; mtimeMs: number },
  size: number,
): void {
  assertEqual(value.isFile, true, 'stat should report a file');
  assertEqual(value.isDirectory, false, 'file stat should not report a directory');
  assertEqual(value.size, size, 'stat size mismatch');
  assert(
    typeof value.mtimeMs === 'number' && Number.isFinite(value.mtimeMs),
    'mtimeMs must be finite',
  );
}

function waitForWatchEvent(
  path: string,
  mutate: () => void,
  options: { interval?: number; recursive?: boolean } = {},
): Promise<{ eventType: string; filename: string }> {
  const { promise, resolve, reject } = Promise.withResolvers<{
    eventType: string;
    filename: string;
  }>();
  let closed = false;
  const watcher = watch(path, options, (eventType: string, filename: string) => {
    if (!closed) {
      closed = true;
      watcher.close();
      clearTimeout(timeout);
      resolve({ eventType, filename });
    }
  });
  const timeout = setTimeout(() => {
    if (!closed) {
      closed = true;
      watcher.close();
      reject(new Error('timed out waiting for native fs.watch event'));
    }
  }, 1000);
  mutate();
  return promise;
}

test('node:fs sync adapter delegates to host fs', async () => {
  await withTempDir('sync', (dir) => {
    const file = join(dir, 'data.txt');
    const renamed = join(dir, 'renamed.txt');
    const child = join(dir, 'child');

    fsDefault.mkdirSync(child);
    writeFileSync(file, 'hello');
    appendFileSync(file, ' world');
    assertEqual(readFileSync(file, 'utf8'), 'hello world');
    assertDeepEqual(
      bytes(readFileSync(file)),
      [104, 101, 108, 108, 111, 32, 119, 111, 114, 108, 100],
    );
    assertStatFile(statSync(file), 11);
    assertDeepEqual(sorted(readdirSync(dir)), ['child', 'data.txt']);

    renameSync(file, renamed);
    assertEqual(existsSync(file), false);
    assertEqual(existsSync(renamed), true);
    const canonical = realpathSync(renamed);
    assert(
      canonical.endsWith('/renamed.txt') || canonical.endsWith('\\renamed.txt'),
      'realpathSync should return renamed path',
    );

    rmSync(child, { recursive: true, force: true });
    assertEqual(existsSync(child), false);
  });
});

test('node:fs promise and callback adapters delegate to host fs', async () => {
  await withTempDir('async', async (dir) => {
    const file = join(dir, 'async.txt');
    const callbackFile = join(dir, 'callback.txt');

    await fsPromises.writeFile(file, 'alpha');
    await fsDefault.promises.appendFile(file, ' beta');
    assertEqual(await fsPromises.readFile(file, 'utf8'), 'alpha beta');
    assertStatFile(await stat(file), 10);
    assertDeepEqual(sorted(await fsPromises.readdir(dir)), ['async.txt']);
    assertEqual(await exists(file), true);

    const {
      promise: writePromise,
      resolve: resolveWrite,
      reject: rejectWrite,
    } = Promise.withResolvers<void>();
    writeFile(callbackFile, 'callback', (err: Error | null) => {
      if (err) {
        rejectWrite(err);
      } else {
        resolveWrite();
      }
    });
    await writePromise;
    const {
      promise: readPromise,
      resolve: resolveRead,
      reject: rejectRead,
    } = Promise.withResolvers<string>();
    readFile(callbackFile, 'utf8', (err: Error | null, data: string) => {
      if (err) {
        rejectRead(err);
      } else {
        resolveRead(data);
      }
    });
    const callbackText = await readPromise;
    assertEqual(callbackText, 'callback');

    await appendFile(callbackFile, ' ok');
    assertEqual(await readFile(callbackFile, 'utf8'), 'callback ok');
  });
});

test('node:fs fd primitives round-trip data and stat by descriptor', async () => {
  await withTempDir('fd', async (dir) => {
    const file = join(dir, 'fd.txt');
    const fd = openSync(file, 'w+');
    try {
      assertEqual(writeSync(fd, Buffer.from('hi')), 2);
      fdatasyncSync(fd);
      assertStatFile(fstatSync(fd), 2);
      const buf = new Uint8Array(2);
      assertEqual(readSync(fd, buf, 0, 2, 0), 2);
      assertDeepEqual(bytes(buf), [104, 105]);
    } finally {
      closeSync(fd);
    }

    const afd = await open(file, 'r+');
    try {
      const out = new Uint8Array(2);
      const readResult = await read(afd, out, 0, 2, 0);
      assertEqual(readResult.bytesRead, 2);
      assertDeepEqual(bytes(out), [104, 105]);
      const writeResult = await write(afd, new Uint8Array([33]), 0, 1, 2);
      assertEqual(writeResult.bytesWritten, 1);
      assertStatFile(await fstat(afd), 3);
    } finally {
      await close(afd);
    }
    assertEqual(readFileSync(file, 'utf8'), 'hi!');
  });
});

test('node:fs createReadStream emits data and end', async () => {
  await withTempDir('stream', async (dir) => {
    const file = join(dir, 'stream.txt');
    writeFileSync(file, 'stream-data');
    const { promise, resolve, reject } = Promise.withResolvers<number>();
    let total = 0;
    createReadStream(file, { highWaterMark: 3 })
      .on('data', (chunk: Uint8Array | string) => {
        total += typeof chunk === 'string' ? chunk.length : chunk.byteLength;
      })
      .on('end', () => resolve(total))
      .on('error', reject);
    assertEqual(await promise, 11);

    const written = join(dir, 'written.txt');
    const { promise: finishPromise, resolve: resolveFinish, reject: rejectFinish } = Promise.withResolvers<void>();
    createWriteStream(written)
      .on('finish', resolveFinish)
      .on('error', rejectFinish)
      .end(Buffer.from('ok'));
    await finishPromise;
    assertEqual(readFileSync(written, 'utf8'), 'ok');
  });
});

test('node:fs watch uses native file notifications for file changes', async () => {
  await withTempDir('watch-change', async (dir) => {
    const file = join(dir, 'watched.txt');
    writeFileSync(file, 'before');
    const event = await waitForWatchEvent(file, () => appendFileSync(file, ' after'), {
      interval: 60_000,
    });
    assertEqual(event.eventType, 'change');
    assertEqual(event.filename, 'watched.txt');
  });
});

test('node:fs watch uses native notifications for existence toggles', async () => {
  await withTempDir('watch-rename', async (dir) => {
    const file = join(dir, 'toggle.txt');
    writeFileSync(file, 'present');
    const event = await waitForWatchEvent(file, () => rmSync(file, { force: true }), {
      interval: 60_000,
    });
    assertEqual(event.eventType, 'rename');
    assertEqual(event.filename, 'toggle.txt');
  });
});

test('node:fs watch supports recursive native directory notifications', async () => {
  await withTempDir('watch-recursive', async (dir) => {
    const child = join(dir, 'child');
    const file = join(child, 'nested.txt');
    mkdirSync(child, { recursive: true });
    const event = await waitForWatchEvent(dir, () => writeFileSync(file, 'nested'), {
      recursive: true,
    });
    assertEqual(event.eventType, 'rename');
    assertEqual(event.filename, 'nested.txt');
  });
});

test('node:fs watch returns an EventEmitter-like watcher with idempotent close', async () => {
  await withTempDir('watch-emitter', async (dir) => {
    const file = join(dir, 'emitter.txt');
    writeFileSync(file, 'before');
    const { promise, resolve, reject } = Promise.withResolvers<{
      eventType: string;
      filename: string;
    }>();
    const watcher = watch(file, { interval: 60_000 });
    let closeCount = 0;
    watcher.on('close', () => {
      closeCount += 1;
    });
    watcher.on('change', (eventType: string, filename: string) => {
      watcher.close();
      watcher.close();
      resolve({ eventType, filename });
    });
    const timeout = setTimeout(() => {
      watcher.close();
      reject(new Error('timed out waiting for EventEmitter-like fs.watch event'));
    }, 1000);
    appendFileSync(file, ' after');
    const event = await promise;
    clearTimeout(timeout);
    assertEqual(event.eventType, 'change');
    assertEqual(event.filename, 'emitter.txt');
    assertEqual(closeCount, 1, 'close should be idempotent');
  });
});

await run();
