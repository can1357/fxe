import { tickAnimatedFrames } from '../animated/timing.ts';
import {
  frameProfileBeginFrame,
  frameProfileCommitFrame,
  frameProfilePhase,
} from './frame_profile.ts';
import { tickSchedulerFrame } from './scheduler.ts';

// Frame phases (JS-visible only; C++ submit/present-wait reported separately):
//   reconcile  → tickSchedulerFrame: drain priority lanes into fiber work
//   animations → tickAnimatedFrames: advance Animated.timing/spring drivers
//   frameCallbacks → useFrame() + g_frame_callbacks
// After tickFrame returns, the C++ frame loop runs layout → paint → submit → present-wait.

export type FrameLoopDisposer = () => void;

export interface FrameLoopOptions {
  requestAnimationFrame?: (fn: (timeMs: number) => void) => unknown;
  cancelAnimationFrame?: (id: unknown) => void;
}

type FxeUiFrameLoopBridgeGlobal = typeof globalThis & {
  __fxeUiEnsureFrameLoop?: () => FrameLoopDisposer;
};

export const g_frame_callbacks: Array<(dtMs: number) => void> = [];

let g_tick_frame_counter = 0;
let g_render_frame_loop_dispose: FrameLoopDisposer | null = null;

export function bumpTickFrameCounter(): number {
  return ++g_tick_frame_counter;
}

export function getTickFrameCounter(): number {
  return g_tick_frame_counter;
}

export function ensureRenderFrameLoop(options?: FrameLoopOptions): FrameLoopDisposer {
  if (!g_render_frame_loop_dispose) g_render_frame_loop_dispose = startFrameLoop(options);
  return g_render_frame_loop_dispose;
}

(globalThis as FxeUiFrameLoopBridgeGlobal).__fxeUiEnsureFrameLoop = () => ensureRenderFrameLoop();

export function startFrameLoop(options: FrameLoopOptions = {}): FrameLoopDisposer {
  const globals = globalThis as {
    requestAnimationFrame?: (fn: (timeMs: number) => void) => unknown;
    cancelAnimationFrame?: (id: unknown) => void;
    performance?: { now?: () => number };
  };
  const requestFrame = options.requestAnimationFrame ?? globals.requestAnimationFrame;
  if (typeof requestFrame !== 'function')
    throw new Error('startFrameLoop() requires requestAnimationFrame');
  const cancelFrame = options.cancelAnimationFrame ?? globals.cancelAnimationFrame;

  let disposed = false;
  let handle: unknown;
  let previousTimeMs: number | null = null;
  const step = (timeMs: number): void => {
    if (disposed) return;
    const dtMs = previousTimeMs === null ? 0 : Math.max(0, timeMs - previousTimeMs);
    previousTimeMs = timeMs;
    tickFrame(dtMs);
    if (!disposed) handle = requestFrame(step);
  };

  handle = requestFrame(step);
  const dispose = (): void => {
    if (disposed) return;
    disposed = true;
    if (typeof cancelFrame === 'function') cancelFrame(handle);
    if (g_render_frame_loop_dispose === dispose) g_render_frame_loop_dispose = null;
  };
  return dispose;
}

export function tickFrame(dtMs: number): void {
  bumpTickFrameCounter();
  const sample = frameProfileBeginFrame(dtMs);
  const perfNow = (globalThis as { performance?: { now?: () => number } }).performance?.now;
  const t0 = sample ? (typeof perfNow === 'function' ? perfNow() : Date.now()) : 0;
  frameProfilePhase(sample, 'reconcile', () => {
    tickSchedulerFrame(dtMs);
  });
  frameProfilePhase(sample, 'animations', () => {
    tickAnimatedFrames(dtMs);
  });
  frameProfilePhase(sample, 'frameCallbacks', () => {
    for (const fn of g_frame_callbacks) {
      try {
        fn(dtMs);
      } catch (e) {
        console.error(`useFrame threw: ${e}`);
      }
    }
  });
  if (sample)
    frameProfileCommitFrame(sample, (typeof perfNow === 'function' ? perfNow() : Date.now()) - t0);
}
