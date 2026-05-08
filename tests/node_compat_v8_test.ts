// Native node:v8 compatibility tests intentionally import FXE host-backed builtins.

import { readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import v8Default, {
  cachedDataVersionTag,
  deserialize,
  getHeapCodeStatistics,
  getHeapSpaceStatistics,
  getHeapStatistics,
  serialize,
  writeHeapSnapshot,
} from 'node:v8';

import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

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

test('node:v8 exposes heap statistics and cached data tag', () => {
  const heap = getHeapStatistics();
  assert(
    typeof heap.heap_size_limit === 'number' && heap.heap_size_limit >= 1024,
    'heap stats broken',
  );
  assert(
    typeof heap.used_heap_size === 'number' && heap.used_heap_size >= 0,
    'heap stats missing fields',
  );

  const spaces = getHeapSpaceStatistics();
  assert(Array.isArray(spaces) && spaces.length > 0, 'heap space stats broken');
  assert(typeof spaces[0]?.space_name === 'string', 'heap space stats missing space_name');

  const code = getHeapCodeStatistics();
  assert(typeof code.code_and_metadata_size === 'number', 'heap code stats broken');

  const tag = cachedDataVersionTag();
  assertEqual(typeof tag, 'number');
  assertEqual(typeof v8Default.cachedDataVersionTag(), 'number');
});

test('node:v8 serialize and deserialize round-trip objects', () => {
  const value = { a: 1, b: [2, 3], nested: { ok: true } };
  const bytes = serialize(value);
  assert(bytes instanceof Uint8Array, 'serialize must return Uint8Array');
  const back = deserialize(bytes);
  assertDeepEqual(back, value);
  assertDeepEqual(v8Default.deserialize(v8Default.serialize(value)), value);
});

test('node:v8 writes heap snapshots', () => {
  const path = join(
    tmpdir(),
    `fxe-v8-${Date.now()}-${Math.random().toString(16).slice(2)}.heapsnapshot`,
  );
  try {
    assert(writeHeapSnapshot(path), 'writeHeapSnapshot should succeed');
    const bytes = readFileSync(path);
    assert(
      bytes instanceof Uint8Array && bytes.byteLength > 0,
      'heap snapshot file should be non-empty',
    );
  } finally {
    rmSync(path, { force: true });
  }
});

await run();
