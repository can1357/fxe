import {
  arch,
  cpus,
  endianness,
  freemem,
  homedir,
  hostname,
  installSystemChangeObserver,
  networkInterfaces,
  platform,
  release,
  tmpdir,
  totalmem,
  type,
  uptime,
  userInfo,
} from 'fxe:os';
import { assert, test } from './ts_harness.ts';

test('fxe:os exports native functions', () => {
  assert(typeof platform === 'function');
  assert(typeof arch === 'function');
  assert(typeof release === 'function');
  assert(typeof type === 'function');
  assert(typeof endianness === 'function');
  assert(typeof homedir === 'function');
  assert(typeof tmpdir === 'function');
  assert(typeof hostname === 'function');
  assert(typeof uptime === 'function');
  assert(typeof totalmem === 'function');
  assert(typeof freemem === 'function');
  assert(typeof cpus === 'function');
  assert(typeof networkInterfaces === 'function');
  assert(typeof userInfo === 'function');
  assert(typeof installSystemChangeObserver === 'function');
});

test('fxe:os platform returns a value', () => {
  assert(typeof platform() === 'string');
});
