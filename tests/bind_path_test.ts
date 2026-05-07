import { assert, assertEqual, test } from './ts_harness.ts';

const isWindows = process.platform === 'win32';
const nativeSep = isWindows ? '\\' : '/';
const nativeDelimiter = isWindows ? ';' : ':';

function withoutTrailingSlash(value: string): string {
  if (value.length > 1 && value.endsWith('/')) {
    return value.slice(0, -1);
  }
  return value;
}

test('path exposes platform separators', () => {
  assertEqual(path.sep, nativeSep);
  assertEqual(path.delimiter, nativeDelimiter);
});

test('path.join combines and normalizes relative segments', () => {
  assertEqual(path.join('alpha', 'beta', 'gamma.txt'), 'alpha/beta/gamma.txt');
  assertEqual(path.join('alpha', '.', 'beta', '..', 'gamma'), 'alpha/gamma');
  assertEqual(path.join('', 'alpha', '', 'beta'), 'alpha/beta');
  assertEqual(path.join('', ''), '.');
  assertEqual(path.join(), '.');
  assertEqual(path.join('alpha/', 'beta/'), 'alpha/beta');
});

test('path.join preserves absolute roots portably', () => {
  if (!isWindows) {
    assertEqual(path.join('/tmp', 'fxe', '..', 'asset.png'), '/tmp/asset.png');
    assertEqual(path.join('/', 'tmp', ''), '/tmp');
  } else {
    const joined = path.join('C:\\tmp', 'fxe', '..', 'asset.png');
    assert(joined.endsWith('tmp/asset.png'), `unexpected windows join result: ${joined}`);
  }
});

test('path.resolve uses cwd for relative inputs and resets on absolute inputs', () => {
  const cwd = withoutTrailingSlash(path.normalize(process.cwd()));
  assertEqual(path.resolve(), cwd);
  assertEqual(path.resolve('alpha', '..', 'beta'), `${cwd}/beta`);

  if (!isWindows) {
    assertEqual(path.resolve('ignored', '/tmp', 'fxe', '..', 'asset.png'), '/tmp/asset.png');
  } else {
    const resolved = path.resolve('ignored', 'C:\\tmp', 'fxe', '..', 'asset.png');
    assert(resolved.endsWith('tmp/asset.png'), `unexpected windows resolve result: ${resolved}`);
  }
});

test('path.dirname returns parent paths and dot for bare filenames', () => {
  assertEqual(path.dirname('file.txt'), '.');
  assertEqual(path.dirname('alpha/beta/file.txt'), 'alpha/beta');
  assertEqual(path.dirname('alpha/beta/../file.txt'), 'alpha/beta/..');

  if (!isWindows) {
    assertEqual(path.dirname('/tmp/file.txt'), '/tmp');
    assertEqual(path.dirname('/tmp'), '/');
  }
});

test('path.basename returns filename and strips matching extension', () => {
  assertEqual(path.basename('alpha/beta/file.txt'), 'file.txt');
  assertEqual(path.basename('file.txt', '.txt'), 'file');
  assertEqual(path.basename('archive.tar.gz', '.gz'), 'archive.tar');
  assertEqual(path.basename('archive.tar.gz', '.zip'), 'archive.tar.gz');
  assertEqual(path.basename('archive.tar.gz', ''), 'archive.tar.gz');
  assertEqual(path.basename('alpha/beta/.env'), '.env');
});

test('path.extname returns the final extension', () => {
  assertEqual(path.extname('file.txt'), '.txt');
  assertEqual(path.extname('archive.tar.gz'), '.gz');
  assertEqual(path.extname('no-extension'), '');
  assertEqual(path.extname('alpha/.env'), '');
  assertEqual(path.extname('alpha/file.'), '.');
});

test('path.relative computes lexical route between paths', () => {
  assertEqual(path.relative('alpha/beta', 'alpha/beta/gamma/file.txt'), 'gamma/file.txt');
  assertEqual(path.relative('alpha/beta/gamma', 'alpha/beta/file.txt'), '../file.txt');
  assertEqual(path.relative('alpha/beta', 'alpha/beta'), '.');
  assertEqual(path.relative('alpha/beta/one', 'alpha/beta/two'), '../two');

  if (!isWindows) {
    assertEqual(path.relative('/tmp/fxe/assets', '/tmp/fxe/out/app.bin'), '../out/app.bin');
  }
});

test('path.normalize collapses dot segments while preserving meaningful parents', () => {
  assertEqual(path.normalize('alpha/./beta//gamma'), 'alpha/beta/gamma');
  assertEqual(path.normalize('alpha/beta/../gamma'), 'alpha/gamma');
  assertEqual(path.normalize(''), '.');
  assertEqual(path.normalize('.'), '.');
  assertEqual(path.normalize('alpha/../../beta'), '../beta');

  if (!isWindows) {
    assertEqual(path.normalize('/tmp/fxe/../asset'), '/tmp/asset');
    assertEqual(path.normalize('/'), '/');
  }
});

test('path.isAbsolute distinguishes absolute and relative inputs', () => {
  assertEqual(path.isAbsolute('alpha/beta'), false);
  assertEqual(path.isAbsolute('./alpha'), false);
  assertEqual(path.isAbsolute('../alpha'), false);
  assertEqual(path.isAbsolute(''), false);

  if (!isWindows) {
    assertEqual(path.isAbsolute('/alpha/beta'), true);
  } else {
    assertEqual(path.isAbsolute('C:\\alpha\\beta'), true);
    assertEqual(path.isAbsolute('C:alpha\\beta'), false);
  }
});
