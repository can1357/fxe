export interface FramePhases {
  js: number;
  animations: number;
  reconcile: number;
  frameCallbacks: number;
}

export interface FrameSample {
  frameId: number;
  startMs: number;
  totalMs: number;
  dtMs: number;
  phases: FramePhases;
}

export interface FrameProfileApi {
  enable(opts?: { ringSize?: number }): void;
  disable(): void;
  isEnabled(): boolean;
  drain(): FrameSample[];
  snapshot(): FrameSample[];
}

interface FrameProfileState {
  enabled: boolean;
  ringSize: number;
  head: number;
  count: number;
  nextFrameId: number;
  samples: Array<FrameSample | undefined>;
}

const DEFAULT_RING_SIZE = 240;
const kFrameProfileState = Symbol.for('fxe-ui.frameProfileState');

type FrameProfileGlobal = typeof globalThis & {
  __fxeFrameProfile?: FrameProfileApi;
};

function now(): number {
  const perf = globalThis as { performance?: { now?: () => number } };
  return typeof perf.performance?.now === 'function' ? perf.performance.now() : Date.now();
}

function createState(ringSize = DEFAULT_RING_SIZE): FrameProfileState {
  return {
    enabled: false,
    ringSize,
    head: 0,
    count: 0,
    nextFrameId: 0,
    samples: new Array<FrameSample | undefined>(ringSize),
  };
}

function frameProfileState(): FrameProfileState {
  const globalObject = globalThis as FrameProfileGlobal;
  let state = Reflect.get(globalObject, kFrameProfileState) as FrameProfileState | undefined;
  if (!state) {
    state = createState();
    Reflect.set(globalObject, kFrameProfileState, state);
  }
  return state;
}

function resetRing(state: FrameProfileState, ringSize: number): void {
  state.ringSize = ringSize;
  state.head = 0;
  state.count = 0;
  state.samples = new Array<FrameSample | undefined>(ringSize);
}

function snapshotFromState(state: FrameProfileState): FrameSample[] {
  const out: FrameSample[] = [];
  for (let i = 0; i < state.count; ++i) {
    const sample = state.samples[(state.head + i) % state.ringSize];
    if (sample) out.push(sample);
  }
  return out;
}

function pushSample(state: FrameProfileState, sample: FrameSample): void {
  if (state.ringSize <= 0) return;
  const writeIndex = (state.head + state.count) % state.ringSize;
  state.samples[writeIndex] = sample;
  if (state.count < state.ringSize) {
    state.count++;
  } else {
    state.head = (state.head + 1) % state.ringSize;
  }
}

const frameProfileApi: FrameProfileApi = {
  enable(opts) {
    const state = frameProfileState();
    const ringSize = Math.max(1, Math.floor(opts?.ringSize ?? DEFAULT_RING_SIZE));
    state.enabled = true;
    resetRing(state, ringSize);
  },
  disable() {
    const state = frameProfileState();
    state.enabled = false;
    state.head = 0;
    state.count = 0;
  },
  isEnabled() {
    return frameProfileState().enabled;
  },
  drain() {
    const state = frameProfileState();
    const out = snapshotFromState(state);
    state.head = 0;
    state.count = 0;
    return out;
  },
  snapshot() {
    return snapshotFromState(frameProfileState());
  },
};

export function frameProfileBeginFrame(dtMs: number): FrameSample | null {
  const state = frameProfileState();
  if (!state.enabled) return null;
  return {
    frameId: ++state.nextFrameId,
    startMs: now(),
    totalMs: 0,
    dtMs,
    phases: {
      js: 0,
      animations: 0,
      reconcile: 0,
      frameCallbacks: 0,
    },
  };
}

export function frameProfilePhase(
  sample: FrameSample | null,
  phase: keyof FramePhases,
  body: () => void,
): void {
  if (!sample) {
    body();
    return;
  }
  const startMs = now();
  try {
    body();
  } finally {
    sample.phases[phase] += now() - startMs;
  }
}

export function frameProfileCommitFrame(sample: FrameSample | null, totalMs: number): void {
  if (!sample) return;
  sample.totalMs = totalMs;
  sample.phases.js = totalMs;
  pushSample(frameProfileState(), sample);
}

const globalObject = globalThis as FrameProfileGlobal;
if (!globalObject.__fxeFrameProfile) {
  Reflect.set(globalObject, '__fxeFrameProfile', frameProfileApi);
}
