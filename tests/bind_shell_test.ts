import { assert, assertEqual, test } from './ts_harness.ts';

const shellTypes: {
  openExternal(url: string): boolean;
  showItemInFolder(path: string): boolean;
  beep(): void;
  trashItem(path: string): boolean;
} = shell;

test('shell exposes typed functions', () => {
  assertEqual(typeof shellTypes.openExternal, 'function');
  assertEqual(typeof shellTypes.showItemInFolder, 'function');
  assertEqual(typeof shellTypes.beep, 'function');
  assertEqual(typeof shellTypes.trashItem, 'function');
});

test('shell.beep returns undefined', () => {
  const result = shellTypes.beep();
  assertEqual(result, undefined);
});

test('shell rejects invalid external URLs and missing show paths without throwing', () => {
  assertEqual(shellTypes.openExternal('http://[::1'), false);

  const missing = path.join(
    process.cwd(),
    `.fxe-missing-shell-parent-${process.pid}`,
    'missing-item.txt',
  );
  assert(!fs.existsSync(path.dirname(missing)), 'missing fixture parent unexpectedly exists');
  assertEqual(shellTypes.showItemInFolder(missing), false);
});

test('shell.trashItem handles a temporary file', () => {
  const dir = path.join(
    process.cwd(),
    `.fxe-shell-trash-test-${process.pid}-${Math.floor(performance.now() * 1000)}`,
  );
  const file = path.join(dir, 'trash-me.txt');

  fs.mkdirSync(dir);
  fs.writeFileSync(file, 'temporary shell trash test file');

  try {
    const trashed = shellTypes.trashItem(file);
    assertEqual(typeof trashed, 'boolean');

    if (trashed) {
      assertEqual(fs.existsSync(file), false, 'trashed file should not remain at original path');
    } else {
      assertEqual(fs.existsSync(file), true, 'failed trashItem should leave file for cleanup');
    }
  } finally {
    fs.rmSync(file, { force: true });
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
