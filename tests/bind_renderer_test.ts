import { assert, assertDeepEqual, assertEqual, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;

function assertFiniteNumber(value: number, label: string): void {
  assert(Number.isFinite(value), `${label} must be finite, got ${value}`);
}

function assertFinitePair(value: readonly [number, number], label: string): void {
  assertEqual(value.length, 2, `${label} length`);
  assertFiniteNumber(value[0], `${label}[0]`);
  assertFiniteNumber(value[1], `${label}[1]`);
}

function assertFiniteVec4(value: readonly [number, number, number, number], label: string): void {
  assertEqual(value.length, 4, `${label} length`);
  assertFiniteNumber(value[0], `${label}[0]`);
  assertFiniteNumber(value[1], `${label}[1]`);
  assertFiniteNumber(value[2], `${label}[2]`);
  assertFiniteNumber(value[3], `${label}[3]`);
}

function assertClose(actual: number, expected: number, label: string): void {
  const delta = Math.abs(actual - expected);
  assert(delta <= 0.001, `${label}: expected ${expected}, got ${actual}`);
}

function invisibleWindow(options: FXE.WindowOptions = {}): FXE.Window {
  return new Window({
    width: 64,
    height: 48,
    visible: false,
    decorated: false,
    resizable: false,
    title: 'bind-renderer-test',
    ...options,
  });
}

test('Renderer constructs for an invisible Window and inherits CommandBuffer', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, {
      multisampleCount: 1,
      enableBloom: false,
      vsync: false,
    });

    assertEqual(win.isVisible(), false, 'test window should remain invisible');
    assert(renderer instanceof Renderer, 'renderer instanceof Renderer');
    assert(renderer instanceof CommandBuffer, 'renderer inherits CommandBuffer prototype');
    assertEqual(typeof renderer.beginFrame, 'function', 'beginFrame is installed');
    assertEqual(typeof renderer.endFrame, 'function', 'endFrame is installed');
    assertEqual(typeof renderer.setMultisample, 'function', 'setMultisample is installed');
    assertEqual(typeof renderer.setBloom, 'function', 'setBloom is installed');
    assertEqual(typeof renderer.setClearColor, 'function', 'setClearColor is installed');
    assertEqual(typeof renderer.screen, 'function', 'screen is installed');
    assertEqual(typeof renderer.viewport, 'function', 'viewport is installed');
    assertEqual(typeof renderer.worldToScreen, 'function', 'worldToScreen is installed');
    assertEqual(renderer.isEmpty(), true, 'new renderer command buffer starts empty');
  } finally {
    win.close();
  }
});

test('Renderer settings APIs accept documented inputs', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, { multisampleCount: 4, enableBloom: true, vsync: false });

    renderer.setMultisample(1);
    renderer.setMultisample(4);
    renderer.setBloom(false);
    renderer.setBloom(true);
    renderer.setClearColor(0.1, 0.2, 0.3);
    renderer.setClearColor(0.4, 0.5, 0.6, 0.7);
    renderer.setClearColor([0.8, 0.7, 0.6, 0.5]);

    assertEqual(renderer.isEmpty(), true, 'settings should not enqueue geometry');
  } finally {
    win.close();
  }
});

test('Renderer screen, viewport, and worldToScreen are deterministic after beginFrame', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, { multisampleCount: 1, enableBloom: false, vsync: false });
    const framebuffer = win.framebufferSize();

    renderer.beginFrame(
      new Float32Array([0, 0, 1]),
      new Float32Array([0, 0, -1]),
      new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
    );

    const screen = renderer.screen();
    assertFinitePair(screen, 'screen');
    assert(screen[0] > 0, 'screen width must be positive');
    assert(screen[1] > 0, 'screen height must be positive');
    assertDeepEqual(screen, framebuffer, 'screen matches Window.framebufferSize()');

    const viewport = renderer.viewport();
    assertDeepEqual(viewport.at, [0, 0], 'viewport origin');
    assertDeepEqual(viewport.size, screen, 'viewport size matches screen');

    const center = renderer.worldToScreen(new Float32Array([0, 0, 0]));
    assertFiniteVec4(center, 'center projection');
    assertClose(center[0], screen[0] / 2, 'center x');
    assertClose(center[1], screen[1] / 2, 'center y');
    assertClose(center[2], 0, 'center z');
    assertClose(center[3], 1, 'center w');

    const lowerRight = renderer.worldToScreen(new Float32Array([1, -1, 0]));
    assertFiniteVec4(lowerRight, 'lower-right projection');
    assertClose(lowerRight[0], screen[0], 'lower-right x');
    assertClose(lowerRight[1], screen[1], 'lower-right y');
    assertClose(lowerRight[2], 0, 'lower-right z');
    assertClose(lowerRight[3], 1, 'lower-right w');

    renderer.endFrame();
  } finally {
    win.close();
  }
});

test('Renderer exposes CommandBuffer mutation APIs', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, { multisampleCount: 1, enableBloom: false, vsync: false });
    const startEpoch = renderer.epoch();

    const allocation = renderer.allocate(3, 3, TRIANGLE);
    assertEqual(allocation.base, 0, 'first renderer allocation base');
    assertEqual(allocation.indexBase, 0, 'first renderer allocation index base');
    assertEqual(allocation.verts.length > 0, true, 'allocation exposes vertices');
    assertEqual(allocation.idxs.length, 3, 'allocation exposes triangle indices');
    assertEqual(renderer.vertexCount(), 3, 'renderer vertex count after allocation');
    assertEqual(renderer.indexCount(TRIANGLE), 3, 'renderer triangle index count after allocation');
    assert(renderer.epoch() > startEpoch, 'allocation advances renderer epoch');

    const clone = renderer.clone();
    assert(clone instanceof CommandBuffer, 'renderer clone is a CommandBuffer');
    assert(!(clone instanceof Renderer), 'renderer clone is not a Renderer');
    assertEqual(clone.vertexCount(), renderer.vertexCount(), 'clone vertex count');
    assertEqual(
      clone.indexCount(TRIANGLE),
      renderer.indexCount(TRIANGLE),
      'clone triangle index count',
    );

    renderer.clear();
    assertEqual(renderer.isEmpty(), true, 'clear empties renderer command buffer');
    renderer.queue(clone);
    assertEqual(
      renderer.vertexCount(),
      clone.vertexCount(),
      'queue accepts CommandBuffer on Renderer',
    );
  } finally {
    win.close();
  }
});
