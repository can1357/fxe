import type { ClipboardImage, WindowOptions } from 'fxe';
import { Window } from 'fxe';
import { assert, assertEqual, run, test } from './ts_harness.ts';

const hiddenWindowOptions: WindowOptions = {
  width: 16,
  height: 16,
  visible: false,
  title: 'clipboard-image-test',
};

function withHiddenWindow(fn: (win: Window) => void): void {
  const win = new Window(hiddenWindowOptions);
  try {
    fn(win);
  } finally {
    win.close();
  }
}

function assertClipboardImageShape(value: ClipboardImage): void {
  assertEqual(typeof value.width, 'number');
  assertEqual(typeof value.height, 'number');
  assert(value.width > 0, 'clipboard image width should be positive');
  assert(value.height > 0, 'clipboard image height should be positive');
  assert(value.data instanceof Uint8Array, 'clipboard image data should be Uint8Array');
  assert(
    value.data.length >= value.width * value.height * 4,
    'clipboard image data should contain at least width*height*4 RGBA bytes',
  );
}

test('Window clipboard image API exposes stable read/write shapes on hidden windows', () => {
  withHiddenWindow((win) => {
    const current = win.readClipboardImage();
    if (current !== null) {
      assertClipboardImageShape(current);
    }

    const result = win.writeClipboardImage({
      width: 1,
      height: 1,
      data: new Uint8Array([255, 0, 255, 255]),
    });
    assert(
      typeof result === 'boolean' || result === undefined,
      'writeClipboardImage should return boolean support status or legacy undefined',
    );
  });
});

test('Window clipboard HTML and MIME APIs round-trip on macOS', () => {
  if (process.platform !== 'darwin') return;
  withHiddenWindow((win) => {
    const html = '<strong data-fxe="clipboard">hello</strong>';
    assertEqual(win.setClipboardHtml(html), true);
    assertEqual(win.clipboardHtml(), html);

    const bytes = new Uint8Array([1, 2, 3, 4]);
    assertEqual(win.setClipboardMime('application/x-fxe-clipboard-test', bytes), true);
    const read = win.clipboardMime('application/x-fxe-clipboard-test');
    assert(read instanceof Uint8Array, 'clipboardMime should return bytes after setClipboardMime');
    assertEqual(Array.from(read ?? []).join(','), '1,2,3,4');
  });
});

// NOTE: stdin is exposed on process/stdin, not Window; coverage lives in
// tests/stdin_contract_test.ts unless a window-scoped stdin API is added.

if (false as boolean) {
  const win = new Window({ visible: false });
  const image: ClipboardImage | null = win.readClipboardImage();
  const wrote: boolean = win.writeClipboardImage({
    width: 1,
    height: 1,
    data: new Uint8ClampedArray([0, 0, 0, 0]),
  });
  void image;
  void wrote;
  win.close();
}

await run();
