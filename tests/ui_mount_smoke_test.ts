import { CommandBuffer, type Renderer, type Window, type WindowEventName } from 'fxe';
import { mount, Text, View } from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

test('mount renders one frame and disposes listeners', () => {
  const renderer = new CommandBuffer() as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<string, number>();
  const win = {
    framebufferSize: () => [160, 100] as [number, number],
    requestRedraw: () => undefined,
    on: (event: WindowEventName) => {
      listeners.set(event, (listeners.get(event) ?? 0) + 1);
      return () => listeners.set(event, (listeners.get(event) ?? 1) - 1);
    },
  } as unknown as Window;

  const dispose = mount(
    View({
      style: { width: 160, height: 100, backgroundColor: 0x101010ff },
      children: Text({ children: 'Mounted' }),
    }),
    win,
    { renderer },
  );
  assert(renderer.vertexCount() > 0, 'mount should render non-empty command buffer');
  dispose();
  for (const count of listeners.values())
    assertEqual(count, 0, 'dispose should remove every listener');
});

await run();
