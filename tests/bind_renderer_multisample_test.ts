import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

function invisibleWindow(options: FXE.WindowOptions = {}): FXE.Window {
  return new Window({
    width: 64,
    height: 48,
    visible: false,
    decorated: false,
    resizable: false,
    title: 'bind-renderer-multisample-test',
    ...options,
  });
}

function isPowerOfTwo(value: number): boolean {
  return Number.isInteger(value) && value > 0 && (value & (value - 1)) === 0;
}

test('Renderer exposes supported multisample counts and rejects unsupported values', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, { multisampleCount: 1, enableBloom: false, vsync: false });
    const counts = renderer.supportedMultisampleCounts();

    assertEqual(Array.isArray(counts), true, 'supportedMultisampleCounts returns an array');
    assert(counts.length > 0, 'supportedMultisampleCounts must not be empty');
    assert(counts.includes(1), 'supportedMultisampleCounts must include 1');

    for (let i = 0; i < counts.length; ++i) {
      const count = counts[i];
      assertEqual(typeof count, 'number', `count ${i} type`);
      assert(isPowerOfTwo(count), `count ${count} must be a positive power of two`);
      if (i > 0)
        assert(counts[i - 1] < count, 'counts must be sorted ascending without duplicates');
      renderer.setMultisample(count);
    }

    assertThrows(() => renderer.setMultisample(3), /unsupported multisample count/);
  } finally {
    win.close();
  }
});
