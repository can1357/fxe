import { Renderer, type Window, type WindowDisposer } from 'fxe';
import {
  Component,
  Layer,
  type FrameLoopDisposer,
  type Node,
  render,
  setRenderTarget,
  startFrameLoop,
  useFrame,
} from '../reconciler/fiber.ts';
import { type Theme, ThemeProvider } from '../theme/index.ts';
import {
  dispatchCompose,
  dispatchKeyDown,
  dispatchKeyPress,
  dispatchMouseDown,
  dispatchMouseMove,
  dispatchMouseUp,
  dispatchWheel,
  resetEventPipeline,
} from './event_pipeline.ts';
import { clearFocus } from './focus.ts';
import { clearHitTargets } from './hit_test.ts';

export interface MountOptions {
  renderer?: Renderer;
  theme?: Theme;
  /**
   * When `false`, mount runs a continuous requestAnimationFrame loop that
   * re-renders every tick — necessary for animated/realtime apps that
   * read clocks or external state outside the reconciler.
   *
   * When `undefined` (default) or `true`, mount renders **on demand**:
   * once at startup, then again only when state changes (setState,
   * external store updates, focus / hover / press dispatch, or an
   * explicit `window.requestRedraw()`). Idle CPU cost is zero.
   *
   * Static UIs and form-style apps want the default. Only flip to
   * `lazy: false` if you genuinely need every frame.
   */
  lazy?: boolean;
}

let nextMountId = 0;
let renderTargetOwner: symbol | null = null;

let currentScreenSize = { width: 0, height: 0 };

export function currentRenderTargetSize(): { width: number; height: number } {
  return { ...currentScreenSize };
}

function setCurrentRenderTargetSize(window: Window | null): void {
  if (!window) {
    currentScreenSize = { width: 0, height: 0 };
    return;
  }
  const [width, height] = window.framebufferSize();
  currentScreenSize = { width, height };
}

export function mount(root: Node, window: Window, opts: MountOptions = {}): () => void {
  const renderer = opts.renderer ?? new Renderer(window);
  const disposers: WindowDisposer[] = [];
  const owner = Symbol('fxe-ui mount');
  const rootKey = `fxe-ui-root-${++nextMountId}`;
  const shouldRunFrameLoop = opts.lazy === false;
  let disposed = false;
  let rendering = false;
  let frameLoopDispose: FrameLoopDisposer | null = null;

  renderTargetOwner = owner;
  setCurrentRenderTargetSize(window);
  setRenderTarget(window);

  // Inject the framebuffer rect into the user root. The reconciler now
  // propagates internal `__layout` / `__textStyle` props from wrapper
  // components to the first produced component node, so top-level user
  // components that return a `View`/`Text` subtree receive truthful root
  // constraints instead of collapsing to zero-sized placeholder layout.
  const withRootLayout = (node: Node, width: number, height: number): Node =>
    node.type === 'component'
      ? {
          ...node,
          props: {
            ...(node.props as Record<string, unknown>),
            __layout: {
              x: 0,
              y: 0,
              width,
              height,
              paddingLeft: 0,
              paddingTop: 0,
              paddingRight: 0,
              paddingBottom: 0,
              children: [],
            },
          },
        }
      : node;

  const frame = (): void => {
    if (disposed || rendering) return;
    rendering = true;
    let beganFrame = false;
    try {
      clearHitTargets();
      renderer.beginFrame();
      beganFrame = true;
      render(FrameRoot({ key: rootKey }), renderer);
    } finally {
      if (beganFrame) renderer.endFrame();
      rendering = false;
    }
  };

  const FrameRoot = Component((): Node => {
    if (shouldRunFrameLoop) useFrame(frame);
    const [width, height] = window.framebufferSize();
    return ThemeProvider({ theme: opts.theme, children: withRootLayout(root, width, height) });
  }, 'FXEUIFrameRoot');

  disposers.push(window.on('mousemove', (ev) => dispatchMouseMove(ev, window)));
  disposers.push(window.on('mousedown', dispatchMouseDown));
  disposers.push(window.on('mouseup', dispatchMouseUp));
  disposers.push(window.on('wheel', dispatchWheel));
  disposers.push(window.on('keydown', (ev) => dispatchKeyDown(ev, window)));
  disposers.push(window.on('keypress', dispatchKeyPress));
  disposers.push(window.on('compose', dispatchCompose));
  disposers.push(
    window.on('resize', () => {
      if (renderTargetOwner === owner) setCurrentRenderTargetSize(window);
      frame();
    }),
  );

  frame();
  if (shouldRunFrameLoop) {
    try {
      frameLoopDispose = startFrameLoop();
    } catch {
      frameLoopDispose = null;
    }
  } else {
    // Lazy mode: drive frames on demand from window redraw acks. setState
    // / signals call window.requestRedraw() via requestRenderTargetRedraw;
    // the OS event loop (App.run / Window.run) consumes that flag and
    // invokes the per-window onFrame we register here. Test stubs may not
    // expose this method, so we feature-detect rather than hard-require.
    if (typeof window.setFrameCallback === 'function') {
      window.setFrameCallback(frame);
    }
    window.requestRedraw();
  }

  return () => {
    if (disposed) return;
    disposed = true;
    frameLoopDispose?.();
    frameLoopDispose = null;
    if (!shouldRunFrameLoop && typeof window.setFrameCallback === 'function') {
      window.setFrameCallback(null);
    }
    for (const dispose of disposers.splice(0)) dispose();
    resetEventPipeline();
    clearHitTargets();
    clearFocus();
    if (renderTargetOwner === owner) {
      render(Layer({ key: `${rootKey}-disposed`, children: [] }), renderer);
      renderTargetOwner = null;
      setCurrentRenderTargetSize(null);
      setRenderTarget(null);
    }
  };
}
