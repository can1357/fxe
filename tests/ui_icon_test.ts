import { CommandBuffer, type Renderer, type Window, type WindowEventName } from 'fxe';
import { Icon } from '../packages/fxe-ui/src/components/Icon.ts';
import { ICON_PATHS, type IconName } from '../packages/fxe-ui/src/components/icons.ts';
import { View } from '../packages/fxe-ui/src/components/View.ts';
import { mount } from '../packages/fxe-ui/src/mount/index.ts';
import { assert, assertEqual, run, test } from './ts_harness.ts';

test('fxe_ui_icon_test', () => {
  assert(ICON_PATHS.check.length > 0, 'check icon should contain commands');

  const names = Object.keys(ICON_PATHS) as IconName[];
  assertEqual(new Set(names).size, names.length, 'icon names should be unique');

  for (const name of names) {
    const commands = ICON_PATHS[name];
    assert(commands.length > 0, `${name} should contain at least one command`);
    for (const command of commands) {
      for (const [field, value] of Object.entries(command)) {
        if (field === 'op' || value === undefined || typeof value === 'boolean') continue;
        assert(Number.isFinite(value), `${name}.${field} should be finite`);
      }
    }
  }

  const renderer = new CommandBuffer() as unknown as Renderer;
  renderer.beginFrame = () => renderer.clear();
  renderer.endFrame = () => undefined;
  const listeners = new Map<string, number>();
  const frameCallbacks: Array<((window: Window) => void) | null> = [];
  const win = {
    framebufferSize: () => [96, 48] as [number, number],
    requestRedraw: () => undefined,
    setFrameCallback: (cb: ((window: Window) => void) | null) => {
      frameCallbacks.push(cb);
    },
    takeRedrawRequest: () => true,
    on: (event: WindowEventName) => {
      listeners.set(event, (listeners.get(event) ?? 0) + 1);
      return () => listeners.set(event, (listeners.get(event) ?? 1) - 1);
    },
  } as unknown as Window;

  const dispose = mount(
    View({
      style: { width: 96, height: 48, flexDirection: 'row', gap: 8 },
      children: [Icon({ name: 'check' }), Icon({ name: 'settings' })],
    }),
    win,
    { renderer },
  );
  assert(renderer.vertexCount() > 0, 'mounted icons should emit geometry');
  dispose();
  assertEqual(frameCallbacks[frameCallbacks.length - 1], null);
  for (const count of listeners.values()) assertEqual(count, 0);
});

const isMain = (import.meta as ImportMeta & { main?: boolean }).main;
if (isMain) {
  await run();
}
