// Native Node compatibility smoke tests intentionally import host-backed builtins
// that are provided by FXE at runtime rather than by @types/node.
import osDefault, * as osNamed from 'node:os';
import { platform as barePlatform } from 'node:os';
import ttyDefault, * as ttyNamed from 'node:tty';
import { isatty as bareIsatty } from 'node:tty';

import { assert, assertEqual, run, test } from './ts_harness.ts';

const {
  arch,
  cpus,
  endianness,
  freemem,
  homedir,
  networkInterfaces,
  platform,
  release,
  tmpdir,
  totalmem,
  type,
  userInfo,
} = osNamed;
const { getWindowSize, isatty } = ttyNamed;

test('node:os exposes host-backed primitives', () => {
  assert(
    typeof platform() === 'string' && platform().length > 0,
    'platform must be a non-empty string',
  );
  assert(typeof type() === 'string' && type().length > 0, 'type must be a non-empty string');
  assert(typeof release() === 'string', 'release must be a string');
  assert(typeof arch() === 'string' && arch().length > 0, 'arch must be a non-empty string');
  assert(endianness() === 'LE' || endianness() === 'BE', 'endianness must be LE or BE');
  assert(typeof homedir() === 'string', 'homedir must be a string');
  assert(typeof tmpdir() === 'string' && tmpdir().length > 0, 'tmpdir must be a non-empty string');
  assert(typeof osDefault.hostname() === 'string', 'hostname must be a string');
  assert(
    Number.isFinite(osDefault.uptime()) && osDefault.uptime() >= 0,
    'uptime must be finite and non-negative',
  );

  const total = totalmem();
  const free = freemem();
  assert(Number.isFinite(total) && Number.isFinite(free), 'memory values must be finite');
  assert(total >= free && free >= 0, 'expected totalmem >= freemem >= 0');

  assert(Array.isArray(cpus()), 'cpus must return an array');
  assert(
    networkInterfaces() !== null && typeof networkInterfaces() === 'object',
    'networkInterfaces must return an object',
  );
  assert(userInfo() !== null && typeof userInfo() === 'object', 'userInfo must return an object');
  assertEqual(barePlatform(), platform());
});

test('node:tty exposes host-backed primitives', () => {
  assertEqual(typeof isatty(1), 'boolean');
  assertEqual(typeof ttyDefault.isatty(1), 'boolean');
  assertEqual(typeof bareIsatty(1), 'boolean');

  const size = getWindowSize(1);
  assert(Array.isArray(size) && size.length === 2, 'getWindowSize must return a two-element array');
  assert(Number.isFinite(size[0]), 'columns must be finite');
  assert(Number.isFinite(size[1]), 'rows must be finite');
});

await run();
