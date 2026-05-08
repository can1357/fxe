import { WASI } from 'node:wasi';

import { assert, assertEqual, assertThrows, run, test } from './ts_harness.ts';

const decoder = new TextDecoder();

type WasmMemory = {
  buffer: ArrayBufferLike;
};

type WasmInstance = {
  exports: {
    memory: WasmMemory;
    _start?: () => void;
    _initialize?: () => void;
  };
};

const WasmMemory = (() => {
  const api = (
    globalThis as typeof globalThis & {
      WebAssembly?: {
        Memory: new (descriptor: { initial: number }) => WasmMemory;
      };
    }
  ).WebAssembly;
  assert(
    api && typeof api.Memory === 'function',
    'expected WebAssembly.Memory support in test runtime',
  );
  return api.Memory;
})();
function makeInstance(
  memory: WasmMemory,
  hooks: { _start?: () => void; _initialize?: () => void } = {},
): WasmInstance {
  return {
    exports: {
      memory,
      ...hooks,
    },
  };
}

test('node:wasi constructs and exposes syscall closures', () => {
  const wasi = new WASI({ args: ['demo'], env: { FOO: 'bar' } });
  const imports = wasi.getImportObject();

  assert(
    typeof imports.wasi_snapshot_preview1.fd_write === 'function',
    'expected fd_write syscall',
  );
  assert(
    typeof imports.wasi_snapshot_preview1.random_get === 'function',
    'expected random_get syscall',
  );
  assert(
    typeof imports.wasi_snapshot_preview1.proc_exit === 'function',
    'expected proc_exit syscall',
  );
});

test('node:wasi args/env/clock/random syscalls operate on wasm memory', () => {
  const wasi = new WASI({ args: ['demo'], env: { FOO: 'bar' } });
  const imports = wasi.getImportObject().wasi_snapshot_preview1;
  const memory = new WasmMemory({ initial: 1 });
  wasi.initialize(makeInstance(memory));

  const view = new DataView(memory.buffer);
  const bytes = new Uint8Array(memory.buffer);

  assertEqual(imports.args_sizes_get(0, 4), 0);
  assertEqual(view.getUint32(0, true), 1);
  assertEqual(view.getUint32(4, true), 5);

  assertEqual(imports.args_get(8, 16), 0);
  const argv0 = view.getUint32(8, true);
  assertEqual(decoder.decode(bytes.subarray(argv0, argv0 + 5)), 'demo\0');

  assertEqual(imports.environ_sizes_get(32, 36), 0);
  assertEqual(view.getUint32(32, true), 1);
  assertEqual(view.getUint32(36, true), 8);

  assertEqual(imports.environ_get(40, 48), 0);
  const env0 = view.getUint32(40, true);
  assertEqual(decoder.decode(bytes.subarray(env0, env0 + 8)), 'FOO=bar\0');

  assertEqual(imports.fd_read(0, 0, 0, 56), 0);
  assertEqual(view.getUint32(56, true), 0);
  assertEqual(imports.fd_close(1), 0);
  assertEqual(imports.fd_close(7), 52);

  bytes.fill(0, 64, 80);
  assertEqual(imports.random_get(64, 16), 0);
  assert(
    bytes.subarray(64, 80).some((value) => value !== 0),
    'expected random_get to mutate memory',
  );

  assertEqual(imports.clock_time_get(0, 0, 80), 0);
  assert(view.getBigUint64(80, true) > 0n, 'expected realtime clock value');
  assertEqual(imports.clock_time_get(1, 0, 88), 0);
  assert(view.getBigUint64(88, true) > 0n, 'expected monotonic clock value');
  assertEqual(imports.clock_res_get(1, 96), 0);
  assertEqual(view.getBigUint64(96, true), 1n);
  assertEqual(imports.clock_time_get(9, 0, 104), 28);
});

test('node:wasi start records proc_exit code and fd_write writes lengths', () => {
  const wasi = new WASI();
  const imports = wasi.getImportObject().wasi_snapshot_preview1;
  const memory = new WasmMemory({ initial: 1 });
  wasi.initialize(makeInstance(memory));
  const view = new DataView(memory.buffer);
  const bytes = new Uint8Array(memory.buffer);

  const text = new TextEncoder().encode('hi');
  bytes.set(text, 32);
  view.setUint32(0, 32, true);
  view.setUint32(4, text.length, true);

  assertEqual(imports.fd_write(1, 0, 1, 8), 0);
  assertEqual(view.getUint32(8, true), 2);

  const instance = makeInstance(memory, {
    _start() {
      imports.proc_exit(17);
    },
  });
  wasi.start(instance);
  assertEqual(wasi.exitCode, 17);

  assertThrows(() => new WASI().start(makeInstance(memory)), /no _start/i);
});

await run();
