// Type-only smoke test for the Performance.timeline binding surface.
//
// Compiled (and type-checked) as part of `just ts-check`; not executed
// directly because the bindings live in the embedded V8 host. The goal is
// to lock the public TypeScript shape of `performance.timeline` so callers
// (and the `.d.ts` generator) catch drift early.

declare global {
  interface PerformanceMarkSnapshot {
    count: number;
    totalMs: number;
    lastMs: number;
    minMs: number;
    maxMs: number;
  }

  interface PerformanceTimelineSnapshot {
    marks: Record<string, PerformanceMarkSnapshot>;
    render?: Record<string, number>;
  }

  interface PerformanceTimeline {
    beginMark(name: string): void;
    endMark(name: string): number;
    snapshot(): PerformanceTimelineSnapshot;
  }

  interface Performance {
    timeline: PerformanceTimeline;
  }

  // HMR registry surfaced by typescript.cpp's prelude.
  interface FxeHmrRegistry {
    handlers: Record<string, Array<(path: string) => void>>;
    accept(handler: (path: string) => void): void;
    accept(path: string, handler: (path: string) => void): void;
    fire(path: string): number;
  }

  // eslint-disable-next-line no-var
  var __fxe_hmr: FxeHmrRegistry;
}

export function exercise(): PerformanceTimelineSnapshot {
  performance.timeline.beginMark('frame');
  // ... user code ...
  const dt = performance.timeline.endMark('frame');
  if (typeof dt !== 'number') throw new Error('endMark must return a duration');

  const snap = performance.timeline.snapshot();
  const frame = snap.marks['frame'];
  if (!frame || frame.count < 1) throw new Error('frame mark missing after endMark');
  return snap;
}

export function exerciseHmr(modulePath: string): void {
  globalThis.__fxe_hmr.accept(modulePath, () => {
    // user re-init
  });
}
