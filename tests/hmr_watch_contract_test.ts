import { assert, assertEqual, run, test } from './ts_harness.ts';

const hmr = globalThis.__fxe_hmr;
let tempCounter = 0;

function resetHmr(): void {
  for (const key of Object.keys(hmr.handlers)) {
    delete hmr.handlers[key];
  }
}

function tempRoot(label: string): string {
  return path.join(process.cwd(), `.fxe-hmr-watch-${label}-${process.pid}-${++tempCounter}`);
}

async function withTempDir(name: string, fn: (dir: string) => Promise<void>): Promise<void> {
  const dir = tempRoot(name);
  fs.mkdirSync(dir, { recursive: true });
  try {
    await fn(dir);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

test('__fxe_hmr.watch fires registered handlers for file changes', async () => {
  resetHmr();
  assert(hmr !== undefined, '__fxe_hmr must be installed');
  assertEqual(typeof hmr.accept, 'function');
  assertEqual(typeof hmr.fire, 'function');
  assertEqual(typeof hmr.watch, 'function');

  await withTempDir('change', async (dir) => {
    const file = path.join(dir, 'watched.ts');
    fs.writeFileSync(file, 'before');

    let calls = 0;
    const seen: string[] = [];
    hmr.accept(file, (path) => {
      ++calls;
      seen.push(path);
    });

    const watcher = hmr.watch(file);
    try {
      const observed = new Promise<number>((resolve, reject) => {
        const timeout = setTimeout(() => {
          reject(new Error(`timed out waiting for HMR watch event; calls=${calls}`));
        }, 500);

        hmr.accept(file, () => {
          clearTimeout(timeout);
          resolve(calls);
        });

        fs.appendFileSync(file, ' after');
      });

      assertEqual(await observed, 1, 'first registered handler should have fired once');
      assertEqual(seen.length, 1, 'path-specific handler should fire once');
      assertEqual(seen[0], file, 'handler receives watched path');
    } finally {
      watcher.close();
    }
  });
});

await run();
