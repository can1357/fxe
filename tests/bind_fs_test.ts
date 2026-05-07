import {
  assert,
  assertDeepEqual,
  assertEqual,
  assertRejects,
  assertThrows,
  test,
} from './ts_harness.ts';

let tempCounter = 0;

function tempRoot(label: string): string {
  return path.join(process.cwd(), `.fxe-bind-fs-${label}-${process.pid}-${++tempCounter}`);
}

function withTempDir(name: string, fn: (dir: string) => void): void {
  const dir = tempRoot(name);
  fs.mkdirSync(dir, { recursive: true });
  try {
    fn(dir);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

async function withTempDirAsync(name: string, fn: (dir: string) => Promise<void>): Promise<void> {
  const dir = tempRoot(name);
  await fs.mkdir(dir, { recursive: true });
  try {
    await fn(dir);
  } finally {
    await fs.rm(dir, { recursive: true, force: true });
  }
}

function bytes(value: Uint8Array): number[] {
  return Array.from(value);
}

function sorted(values: string[]): string[] {
  return [...values].sort();
}

function sortedDirents(values: FxeDirent[]): FxeDirent[] {
  return [...values]
    .map((entry) => ({ name: entry.name, isFile: entry.isFile, isDirectory: entry.isDirectory }))
    .sort((a, b) => a.name.localeCompare(b.name));
}

function assertStatFile(stat: FxeStats, size: number): void {
  assertEqual(stat.isFile, true, 'stat should report a file');
  assertEqual(stat.isDirectory, false, 'file stat should not report a directory');
  assertEqual(stat.size, size, 'file stat size mismatch');
  assert(typeof stat.mtimeMs === 'number', 'mtimeMs should be numeric');
  assert(typeof stat.atimeMs === 'number', 'atimeMs should be numeric');
  assert(typeof stat.ctimeMs === 'number', 'ctimeMs should be numeric');
}

function assertStatDirectory(stat: FxeStats): void {
  assertEqual(stat.isFile, false, 'directory stat should not report a file');
  assertEqual(stat.isDirectory, true, 'stat should report a directory');
}

test('fs sync read/write string bytes append stat readdir rename realpath rm', () => {
  withTempDir('sync', (dir) => {
    const nested = path.join(dir, 'one', 'two');
    const file = path.join(nested, 'data.txt');
    const renamed = path.join(nested, 'renamed.txt');
    const bytesFile = path.join(nested, 'bytes.bin');
    const childDir = path.join(nested, 'child');

    fs.mkdirSync(nested, { recursive: true });
    assertEqual(fs.existsSync(nested), true, 'recursive mkdir should create nested directory');
    assertStatDirectory(fs.statSync(nested));
    fs.mkdirSync(childDir);
    assertStatDirectory(fs.statSync(childDir));

    fs.writeFileSync(file, 'hello');
    fs.appendFileSync(file, ' world');
    assertEqual(fs.readFileSync(file, 'utf8'), 'hello world');
    assertEqual(fs.readFileSync(file, { encoding: 'utf-8' }), 'hello world');
    assertDeepEqual(
      bytes(fs.readFileSync(file)),
      bytes(new Uint8Array([104, 101, 108, 108, 111, 32, 119, 111, 114, 108, 100])),
    );
    assertStatFile(fs.statSync(file), 11);

    fs.writeFileSync(bytesFile, new Uint8Array([0, 1, 2, 253, 254, 255]));
    fs.appendFileSync(bytesFile, new Uint8Array([7, 8]).buffer);
    assertDeepEqual(bytes(fs.readFileSync(bytesFile)), [0, 1, 2, 253, 254, 255, 7, 8]);
    assertStatFile(fs.statSync(bytesFile), 8);

    assertDeepEqual(sorted(fs.readdirSync(nested)), ['bytes.bin', 'child', 'data.txt']);
    assertDeepEqual(sortedDirents(fs.readdirSync(nested, { withFileTypes: true })), [
      { name: 'bytes.bin', isFile: true, isDirectory: false },
      { name: 'child', isFile: false, isDirectory: true },
      { name: 'data.txt', isFile: true, isDirectory: false },
    ]);

    fs.renameSync(file, renamed);
    assertEqual(fs.existsSync(file), false, 'old path should not exist after rename');
    assertEqual(fs.existsSync(renamed), true, 'new path should exist after rename');
    assertEqual(fs.readFileSync(renamed, 'utf8'), 'hello world');

    const canonicalDir = fs.realpathSync(nested);
    const canonicalRenamed = fs.realpathSync(renamed);
    assert(path.isAbsolute(canonicalDir), 'realpathSync should return an absolute directory path');
    assert(path.isAbsolute(canonicalRenamed), 'realpathSync should return an absolute file path');
    assert(
      canonicalRenamed.endsWith('/renamed.txt') || canonicalRenamed.endsWith('\\renamed.txt'),
      'realpathSync should end with renamed file',
    );

    fs.rmSync(path.join(dir, 'one'), { recursive: true });
    assertEqual(fs.existsSync(path.join(dir, 'one')), false, 'recursive rm should remove tree');
    fs.rmSync(path.join(dir, 'missing-tree'), { recursive: true, force: true });
  });
});

test('fs sync reports expected missing path errors', () => {
  withTempDir('sync-errors', (dir) => {
    const missing = path.join(dir, 'missing.txt');
    const renamed = path.join(dir, 'renamed.txt');

    assertEqual(fs.existsSync(missing), false);
    assertThrows(() => fs.readFileSync(missing), /open: .*missing\.txt/);
    assertThrows(() => fs.statSync(missing), /stat: .*missing\.txt/);
    assertThrows(() => fs.readdirSync(missing), /scandir: .*missing\.txt/);
    assertThrows(() => fs.renameSync(missing, renamed), /rename: .*missing\.txt/);
  });
});

test('fs async Promise variants mirror sync behavior', async () => {
  await withTempDirAsync('async', async (dir) => {
    const nested = path.join(dir, 'async', 'tree');
    const file = path.join(nested, 'note.txt');
    const renamed = path.join(nested, 'moved.txt');
    const bytesFile = path.join(nested, 'payload.bin');
    const childDir = path.join(nested, 'child');

    await fs.mkdir(nested, { recursive: true });
    assertEqual(
      await fs.exists(nested),
      true,
      'async recursive mkdir should create nested directory',
    );
    assertStatDirectory(await fs.stat(nested));
    await fs.mkdir(childDir);
    assertStatDirectory(await fs.stat(childDir));

    await fs.writeFile(file, 'alpha');
    await fs.appendFile(file, ' beta');
    assertEqual(await fs.readFile(file, 'utf8'), 'alpha beta');
    assertEqual(await fs.readFile(file, { encoding: 'utf8' }), 'alpha beta');
    assertDeepEqual(
      bytes(await fs.readFile(file)),
      bytes(new Uint8Array([97, 108, 112, 104, 97, 32, 98, 101, 116, 97])),
    );
    assertStatFile(await fs.stat(file), 10);

    await fs.writeFile(bytesFile, new Uint8Array([9, 10, 11]));
    const view = new Uint8Array([12, 13, 14, 15]).subarray(1, 3);
    await fs.appendFile(bytesFile, view);
    assertDeepEqual(bytes(await fs.readFile(bytesFile)), [9, 10, 11, 13, 14]);
    assertStatFile(await fs.stat(bytesFile), 5);

    assertDeepEqual(sorted(await fs.readdir(nested)), ['child', 'note.txt', 'payload.bin']);
    assertDeepEqual(sortedDirents(await fs.readdir(nested, { withFileTypes: true })), [
      { name: 'child', isFile: false, isDirectory: true },
      { name: 'note.txt', isFile: true, isDirectory: false },
      { name: 'payload.bin', isFile: true, isDirectory: false },
    ]);

    await fs.rename(file, renamed);
    assertEqual(await fs.exists(file), false, 'old async path should not exist after rename');
    assertEqual(await fs.exists(renamed), true, 'new async path should exist after rename');
    assertEqual(await fs.readFile(renamed, 'utf-8'), 'alpha beta');

    const canonicalNested = await fs.realpath(nested);
    const canonicalRenamed = await fs.realpath(renamed);
    assert(
      path.isAbsolute(canonicalNested),
      'async realpath should return absolute directory path',
    );
    assert(path.isAbsolute(canonicalRenamed), 'async realpath should return absolute file path');
    assert(
      canonicalRenamed.endsWith('/moved.txt') || canonicalRenamed.endsWith('\\moved.txt'),
      'async realpath should end with renamed file',
    );

    await fs.rm(path.join(dir, 'async'), { recursive: true });
    assertEqual(
      await fs.exists(path.join(dir, 'async')),
      false,
      'async recursive rm should remove tree',
    );
    await fs.rm(path.join(dir, 'missing-tree'), { recursive: true, force: true });
  });
});

test('fs async reports expected missing path errors', async () => {
  await withTempDirAsync('async-errors', async (dir) => {
    const missing = path.join(dir, 'missing.txt');
    const renamed = path.join(dir, 'renamed.txt');

    assertEqual(await fs.exists(missing), false);
    await assertRejects(() => fs.readFile(missing), /open: .*missing\.txt/);
    await assertRejects(() => fs.stat(missing), /stat: .*missing\.txt/);
    await assertRejects(() => fs.readdir(missing), /scandir: .*missing\.txt/);
    await assertRejects(() => fs.rename(missing, renamed), /rename: .*missing\.txt/);
  });
});

test('fs extras copy symlink glob atomic access and locks', async () => {
  await withTempDirAsync('extras', async (dir) => {
    const src = path.join(dir, 'src.txt');
    const copied = path.join(dir, 'copied.txt');
    const link = path.join(dir, 'src-link.txt');
    const atomic = path.join(dir, 'atomic.txt');
    const globRoot = path.join(dir, 'glob-root');

    await fs.writeFile(src, 'copy me');
    await fs.copyFile(src, copied);
    assertEqual(await fs.readFile(copied, 'utf8'), 'copy me');

    await fs.symlink(src, link);
    assertEqual(fs.readlinkSync(link), src);
    assertEqual((await fs.lstat(link)).isSymbolicLink, true);

    try {
      await fs.access(path.join(dir, 'missing.txt'));
      throw new Error('expected access to reject');
    } catch (error) {
      const err = error as { code?: string; syscall?: string; path?: string };
      assertEqual(err.code, 'ENOENT');
      assertEqual(err.syscall, 'access');
      assertEqual(err.path, path.join(dir, 'missing.txt'));
    }

    await fs.mkdir(path.join(globRoot, 'a'), { recursive: true });
    await fs.writeFile(path.join(globRoot, 'a', 'one.ts'), 'one');
    await fs.writeFile(path.join(globRoot, 'two.ts'), 'two');
    await fs.writeFile(path.join(globRoot, 'skip.txt'), 'skip');
    const matches: string[] = [];
    for await (const p of fs.glob('**/*.ts', { cwd: globRoot })) {
      matches.push(p);
    }
    assertDeepEqual(sorted(matches), ['a/one.ts', 'two.ts']);

    fs.writeFileAtomicSync(atomic, 'safe');
    assertEqual(fs.readFileSync(atomic, 'utf8'), 'safe');
    assertThrows(
      () => fs.writeFileAtomicSync(atomic, { toString() { throw new Error('encoder failed'); } } as unknown as string),
      /data must be string or TypedArray/,
    );
    assertDeepEqual(
      fs.readdirSync(dir).filter((name) => name.includes('atomic.txt.tmp.')),
      [],
    );

    const fd = fs.openSync(src, 'r+');
    try {
      fs.lockSync(fd, { exclusive: true, nonBlocking: true });
      fs.unlockSync(fd);
      await fs.lock(fd, { exclusive: true, nonBlocking: true });
      await fs.unlock(fd);
    } finally {
      fs.closeSync(fd);
    }
  });
});

test('fs async readFile is deferred and parallelized through libuv', async () => {
  await withTempDirAsync('async-parallel', async (dir) => {
    const file = path.join(dir, 'large.txt');
    const payload = 'x'.repeat(2 * 1024 * 1024);
    await fs.writeFile(file, payload);

    let settled = false;
    const pending = fs.readFile(file, 'utf8').then(() => {
      settled = true;
    });
    assertEqual(settled, false, 'readFile promise must not settle on the calling thread');
    await pending;

    const singleStart = performance.now();
    await fs.readFile(file);
    const singleMs = Math.max(performance.now() - singleStart, 0.001);

    const parallelStart = performance.now();
    await Promise.all(Array.from({ length: 8 }, () => fs.readFile(file)));
    const parallelMs = performance.now() - parallelStart;

    assert(
      parallelMs < Math.max(singleMs * 7, 250),
      `parallel reads should complete well under 8x a single read (single=${singleMs}, parallel=${parallelMs})`,
    );
  });
});

test('fs async honors AbortSignal and reports Node-shaped errors', async () => {
  await withTempDirAsync('async-abort', async (dir) => {
    const missing = path.join(dir, 'missing.txt');
    const controller = new AbortController();
    controller.abort('pre-cancel');

    await assertRejects(
      () => fs.readFile(missing, { signal: controller.signal }),
      /The operation was aborted: pre-cancel/,
    );

    try {
      await fs.stat(missing);
      throw new Error('expected stat to reject');
    } catch (error) {
      const err = error as { code?: string; errno?: number; syscall?: string; path?: string };
      assertEqual(err.code, 'ENOENT');
      assertEqual(typeof err.errno, 'number');
      assertEqual(err.syscall, 'stat');
      assertEqual(err.path, missing);
    }
  });
});
