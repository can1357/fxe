import { App, Primitives, Renderer } from 'fxe';

type MessageEventLike<T = unknown> = {
  readonly data: T;
};

type BroadcastChannelLike = {
  onmessage: ((event: MessageEventLike) => void) | null;
  postMessage(value: unknown): void;
  close(): void;
};

const globals = globalThis as unknown as {
  BroadcastChannel?: new (name: string) => BroadcastChannelLike;
};

if (typeof globals.BroadcastChannel !== 'function') {
  console.error('window_chat requires BroadcastChannel; this runtime does not provide it.');
} else {
  const left = App.openWindow({ width: 360, height: 220, title: 'chat left', x: 80, y: 120 });
  const right = App.openWindow({ width: 360, height: 220, title: 'chat right', x: 480, y: 120 });
  const leftRenderer = new Renderer(left);
  const rightRenderer = new Renderer(right);

  const leftChannel = new globals.BroadcastChannel('fxe-window-chat');
  const rightChannel = new globals.BroadcastChannel('fxe-window-chat');

  let leftMessages = 0;
  let rightMessages = 0;
  let frames = 0;
  let closed = false;

  leftChannel.onmessage = (event) => {
    if (event.data === 'right:ping') {
      ++leftMessages;
    }
  };
  rightChannel.onmessage = (event) => {
    if (event.data === 'left:ping') {
      ++rightMessages;
    }
  };

  const closeBoth = (): void => {
    if (closed) {
      return;
    }
    closed = true;
    leftChannel.close();
    rightChannel.close();
    left.close();
    right.close();
  };

  left.on('close', closeBoth);
  right.on('close', closeBoth);

  const draw = (renderer: Renderer, color: number, pulses: number): void => {
    renderer.beginFrame();
    Primitives.fillRect(renderer, 0, 0, 360, 220, 0, color);
    for (let i = 0; i < Math.min(pulses, 8); ++i) {
      Primitives.fillRect(renderer, 24 + i * 34, 92, 22, 36, 0, 0xffffffff);
    }
    renderer.endFrame();
  };

  left.run(
    () => {
      if (frames % 30 === 0) {
        leftChannel.postMessage('left:ping');
        rightChannel.postMessage('right:ping');
      }

      draw(leftRenderer, 0xff2848c8, leftMessages);
      draw(rightRenderer, 0xff28a858, rightMessages);

      if (++frames >= 240) {
        closeBoth();
      }
    },
    { animate: true, fps: 30 },
  );
}
