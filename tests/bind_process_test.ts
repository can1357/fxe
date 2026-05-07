import { assert, assertDeepEqual, assertEqual, test } from './ts_harness.ts';

function assertStringArray(value: unknown, label: string): asserts value is string[] {
  assert(Array.isArray(value), `${label} should be an array`);
  for (const [index, item] of value.entries()) {
    assertEqual(typeof item, 'string', `${label}[${index}] should be a string`);
  }
}

test('process argv is a string array snapshot', () => {
  assertStringArray(process.argv, 'process.argv');
  assertEqual(
    process.argv.length,
    process.argv.slice().length,
    'argv slice should preserve length',
  );
});

test('process cwd and chdir round-trip', () => {
  const original = process.cwd();
  assertEqual(typeof original, 'string');
  assert(original.length > 0, 'cwd should not be empty');

  try {
    process.chdir(original);
    assertEqual(process.cwd(), original, 'chdir to current cwd should round-trip');

    const parent = path.dirname(original);
    if (parent !== original) {
      process.chdir(parent);
      assertEqual(process.cwd(), parent, 'chdir to parent should update cwd');
      process.chdir(original);
      assertEqual(process.cwd(), original, 'chdir back to original should restore cwd');
    }
  } finally {
    process.chdir(original);
  }
});

test('process env get set delete and Object.keys', () => {
  const key = 'FXE_BIND_PROCESS_TEST_ENV';
  const original = process.env[key];

  try {
    delete process.env[key];
    assertEqual(process.env[key], undefined, 'deleted env key should read as undefined');
    assert(!Object.keys(process.env).includes(key), 'deleted env key should not enumerate');

    process.env[key] = 'alpha';
    assertEqual(process.env[key], 'alpha', 'set env key should be readable');
    assert(Object.keys(process.env).includes(key), 'set env key should enumerate');

    process.env[key] = 'beta';
    assertEqual(process.env[key], 'beta', 'updated env key should be readable');

    delete process.env[key];
    assertEqual(process.env[key], undefined, 'deleted env key should be absent');
  } finally {
    if (original === undefined) {
      delete process.env[key];
    } else {
      process.env[key] = original;
    }
  }
});

test('process platform arch pid and versions are populated', () => {
  assertEqual(typeof process.platform, 'string');
  assert(process.platform.length > 0, 'platform should be non-empty');
  assert(
    ['darwin', 'linux', 'win32', 'unknown'].includes(process.platform) ||
      process.platform.length > 0,
  );

  assertEqual(typeof process.arch, 'string');
  assert(process.arch.length > 0, 'arch should be non-empty');
  assert(['arm64', 'x64', 'ia32', 'unknown'].includes(process.arch) || process.arch.length > 0);

  assertEqual(typeof process.pid, 'number');
  assert(Number.isInteger(process.pid), 'pid should be an integer');
  assert(process.pid > 0, 'pid should be positive');

  assertEqual(typeof process.versions.fxe, 'string');
  assertEqual(typeof process.versions.v8, 'string');
  assertEqual(typeof process.versions.dawn, 'string');
  assert(process.versions.fxe.length > 0, 'fxe version should be non-empty');
  assert(process.versions.v8.length > 0, 'v8 version should be non-empty');
  assert(process.versions.dawn.length > 0, 'dawn version should be non-empty');
});

test('process stdout and stderr write return true', () => {
  assertEqual(process.stdout.write(''), true, 'stdout string write should return true');
  assertEqual(process.stderr.write(''), true, 'stderr string write should return true');
  assertEqual(
    process.stdout.write(new Uint8Array()),
    true,
    'stdout Uint8Array write should return true',
  );
  assertEqual(
    process.stderr.write(new Uint8Array()),
    true,
    'stderr Uint8Array write should return true',
  );
});

test('process nextTick preserves microtask ordering and arguments', async () => {
  const order: string[] = [];

  process.nextTick(
    (first: string, second: string) => {
      order.push(`tick:${first}:${second}`);
    },
    'a',
    'b',
  );
  Promise.resolve().then(() => {
    order.push('promise');
  });
  order.push('sync');

  await Promise.resolve();

  assertDeepEqual(order, ['sync', 'tick:a:b', 'promise']);
});

test('process on and off accept supported events and are chainable', () => {
  const handler = (..._args: unknown[]): void => {};

  assertEqual(process.on('exit', handler), process, 'on(exit) should be chainable');
  assertEqual(process.off('exit', handler), process, 'off(exit) should be chainable');

  assertEqual(
    process.on('unhandledRejection', handler),
    process,
    'on(unhandledRejection) should be chainable',
  );
  assertEqual(
    process.off('unhandledRejection', handler),
    process,
    'off(unhandledRejection) should be chainable',
  );
});
