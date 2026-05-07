import {
  App,
  CommandBuffer,
  type MonitorInfo,
  Monitors,
  Primitives,
  VertexTopology,
  Window,
  type WindowEventHandler,
  type WindowEventMap,
} from 'fxe';

class Counter {
  constructor(private readonly offset: number) {}

  value(input: number): number {
    return input + this.offset;
  }
}

const cb: CommandBuffer = new CommandBuffer();
Primitives.fillRect(cb, 0, 0, 8, 8, 0, 0xffffffff);

// Type-only references. Guarded so the runtime test (which has no GPU) never
// constructs a real Window. The compiler still validates everything inside.
if (false as boolean) {
  const win = new Window({
    width: 64,
    height: 32,
    visible: false,
    resizable: true,
    alwaysOnTop: false,
    minWidth: 16,
    minHeight: 16,
    decorated: true,
  });
  const onKey: WindowEventHandler<'keydown'> = (ev) => {
    const _scancode: number = ev.scancode;
    const _mods: number = ev.modifiers;
    void _scancode;
    void _mods;
  };
  const dispose = win.on('keydown', onKey);
  win.on('mousemove', (ev) => {
    void ev.x;
    void ev.dx;
  });
  win.on('drop', (ev) => {
    void ev.paths.length;
  });
  win.on('close', (ev) => {
    void ev.type;
  });
  win.off('keydown', onKey);
  win.removeAllListeners('keydown');
  win.removeAllListeners();
  dispose();
  win.setTitle('hello');
  win.setSize(100, 100);
  win.setPosition(0, 0);
  const _pos: [number, number] = win.position();
  win.setOpacity(0.5);
  const _op: number = win.opacity();
  win.setIcon(new Uint8Array(4), 1, 1);
  win.setCursor('ibeam');
  const _cp: [number, number] = win.cursorPos();
  const _cb: string = win.clipboardText();
  const _html: string | null = win.clipboardHtml();
  const _setHtml: boolean = win.setClipboardHtml('<b>x</b>');
  const _rtf: string | null = win.clipboardRtf();
  const _setRtf: boolean = win.setClipboardRtf('{\\rtf1 x}');
  const _mime: Uint8Array | null = win.clipboardMime('application/x-fxe-test');
  const _setMime: boolean = win.setClipboardMime('application/x-fxe-test', new Uint8Array([1]));
  const _drag: boolean = win.startDrag({
    html: '<b>x</b>',
    image: { width: 1, height: 1, data: new Uint8Array([255, 0, 0, 255]) },
  });
  void _pos;
  void _op;
  void _cp;
  void _cb;
  void _html;
  void _setHtml;
  void _rtf;
  void _setRtf;
  void _mime;
  void _setMime;
  void _drag;

  const mons: MonitorInfo[] = Monitors.list();
  const primary: MonitorInfo = Monitors.primary();
  void mons;
  void primary;

  App.run({ animate: true, fps: 60 });
  App.run();
  App.quit();
  const _ws: Window[] = App.windows();
  void _ws;
  Window.exit();

  // Type-level coverage of every event name.
  type _AllEvents = keyof WindowEventMap;
  const _name: _AllEvents = 'wheel';
  void _name;
}

console.log(`ts-smoke=${new Counter(5).value(2)}:${cb.indexCount(VertexTopology.Triangle)}`);
