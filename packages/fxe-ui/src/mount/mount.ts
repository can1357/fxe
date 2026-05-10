import { Renderer, type Window, type WindowDisposer } from 'fxe';
import {
  Component,
  consumeLayoutUnresolvedDuringRender,
  type FrameLoopDisposer,
  Layer,
  type Node,
  render,
  setRenderTarget,
  startFrameLoop,
  useFrame,
} from '../reconciler/fiber.ts';
import { type Theme, ThemeProvider } from '../theme/index.ts';
import { installDevToolsShortcut } from './devtools_shortcut.ts';
import {
  attachClipboardSink,
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

const MAX_LAYOUT_SETTLE_PASSES = 16;

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

  const detachClipboardSink = attachClipboardSink({
    onCopy: (text) => window.setClipboardText?.(text),
    onCut: (text) => window.setClipboardText?.(text),
    onPaste: () => window.clipboardText?.() ?? '',
  });
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
      renderer.beginFrame();
      beganFrame = true;
      for (let pass = 0; pass < MAX_LAYOUT_SETTLE_PASSES; ++pass) {
        if (pass > 0) renderer.clear();
        clearHitTargets();
        render(FrameRoot({ key: rootKey }), renderer);
        if (!consumeLayoutUnresolvedDuringRender()) break;
        if (pass + 1 >= MAX_LAYOUT_SETTLE_PASSES) break;
        // The unresolved-layout pass posted a redraw as a fallback for callers
        // outside mount(). We are settling it before present, so consume that
        // internal dirty bit and immediately rebuild into the same frame.
        window.takeRedrawRequest?.();
      }
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
  disposers.push(window.on('keydown', dispatchKeyDown));
  disposers.push(window.on('keypress', dispatchKeyPress));
  disposers.push(window.on('compose', dispatchCompose));
  disposers.push(
    window.on('resize', () => {
      if (renderTargetOwner === owner) setCurrentRenderTargetSize(window);
      frame();
    }),
  );

  const hasNativeLazyCallback =
    !shouldRunFrameLoop && typeof window.setFrameCallback === 'function';
  if (hasNativeLazyCallback) {
    // Lazy mode: drive frames on demand from real redraw requests. Clear the
    // constructor's initial dirty bit before the synchronous startup frame so
    // a redraw requested during that frame (for example after unresolved
    // first-pass layout) remains pending for App.run / Window.run to consume.
    window.setFrameCallback(frame);
    if (typeof window.takeRedrawRequest === 'function') {
      window.takeRedrawRequest();
    }
  }

  frame();
  if (shouldRunFrameLoop) {
    try {
      frameLoopDispose = startFrameLoop();
    } catch {
      frameLoopDispose = null;
    }
  } else if (!hasNativeLazyCallback) {
    window.requestRedraw();
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
    detachClipboardSink();
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
