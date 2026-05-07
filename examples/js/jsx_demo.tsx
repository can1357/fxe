/** @jsxImportSource fxe-ui */

import { Renderer, Window } from 'fxe';
import { type Node, render, setRenderTarget, useFrame, useState } from 'fxe-ui';

const rgba = (r: number, g: number, b: number, a = 255): number =>
  ((r & 255) << 24) | ((g & 255) << 16) | ((b & 255) << 8) | (a & 255);

function CounterPanel(): Node {
  const [count, setCount] = useState(0);
  useFrame(() => setCount((previous) => (previous + 1) % 240));
  const width = 80 + (count % 120);

  return (
    <view style={{ width: 480, height: 260, padding: 40, backgroundColor: rgba(16, 24, 40) }}>
      <text style={{ height: 30, color: rgba(240, 248, 255), fontSize: 20 }}>
        JSX frame {count}
      </text>
      <view style={{ width, height: 28, marginTop: 24, backgroundColor: rgba(80, 160, 255) }} />
    </view>
  );
}

const win = new Window({ width: 480, height: 260, title: 'fxe-ui JSX demo' });
const renderer = new Renderer(win);
setRenderTarget(win);

const root = <CounterPanel />;

win.run(
  () => {
    RenderStats.beginFrame();
    renderer.beginFrame();
    renderer.setClearColor(0.03, 0.04, 0.06, 1);
    render(root, renderer, { animate: true });
    renderer.endFrame();
  },
  { animate: true, fps: 60 },
);
