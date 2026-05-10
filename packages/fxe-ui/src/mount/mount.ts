import { Renderer, type Window, type WindowDisposer } from 'fxe';
import {
  Component,
  type FrameLoopDisposer,
  Layer,
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
import { installDevToolsShortcut } from './devtools_shortcut.ts';

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
  /**
   * Backdrop colour shown by the platform compositor for any region of the
   * window not yet covered by a freshly presented frame. Set this to your
   * app's body / root background to suppress the brief flash visible during
   * live resize where the layer bounds grow on one compositor transaction
   * but the new frame lands on the next.
   *
   * Accepts a packed `0xRRGGBBAA` integer. Omit to leave the platform
   * default (transparent on macOS Metal, which presents as black).
   */
  backgroundColor?: number;
  /**
   * When `undefined` (default) or `true`, mount auto-registers the standard
   * DevTools shortcut if `FXE_DEBUG_PORT` is set in the process environment.
   * Pass `false` to opt out, or provide a custom accelerator string.
   */
  devTools?: boolean | { accelerator?: string };
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
  if (opts.backgroundColor !== undefined && typeof window.setBackgroundColor === 'function') {
    window.setBackgroundColor(opts.backgroundColor);
  }

  // Inject the framebuffer rect onto the user root component node so the
  // reconciler can copy it into the root fiber's `internalLayout` slot
  // before the body runs. Lives outside `props` so memo / shallow-equal
  // comparisons in user code never see it.
  const withRootLayout = (node: Node, width: number, height: number): Node =>
    node.type === 'component'
      ? {
          ...node,
          internalLayout: {
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

  disposers.push(window.on('mousemove', (ev) => dispatchMouseMove(ev, window, window)));
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
    // Lazy mode: drive frames on demand from real redraw requests. setState
    // / signals call window.requestRedraw() via requestRenderTargetRedraw;
    // the OS event loop (App.run / Window.run) consumes that flag and
    // invokes the per-window onFrame we register here. The initial synchronous
    // frame above already painted the tree, so clear the window's constructor
    // dirty bit instead of scheduling a duplicate startup frame.
    if (typeof window.setFrameCallback === 'function') {
      window.setFrameCallback(frame);
      if (typeof window.takeRedrawRequest === 'function') {
        window.takeRedrawRequest();
      }
    } else {
      window.requestRedraw();
    }
  }
  const devToolsHandle =
    opts.devTools === false
      ? null
      : installDevToolsShortcut({
          window,
          accelerator: typeof opts.devTools === 'object' ? opts.devTools.accelerator : undefined,
        });

  return () => {
    if (disposed) return;
    disposed = true;
    frameLoopDispose?.();
    frameLoopDispose = null;
    devToolsHandle?.dispose();
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
