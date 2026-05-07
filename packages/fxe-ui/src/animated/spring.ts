import {
  type AnimatedValue,
  type AnimationEndCallback,
  type CompositeAnimation,
  type ActiveAnimation,
  clearActiveAnimation,
  registerAnimatedFrameStep,
  replaceActiveAnimation,
} from './timing.ts';

export interface SpringAnimationConfig {
  to: number;
  stiffness?: number;
  damping?: number;
  mass?: number;
  restThreshold?: number;
}

interface SpringState {
  x: number;
  v: number;
}

interface SpringDerivative {
  dx: number;
  dv: number;
}

const DEFAULT_STIFFNESS = 170;
const DEFAULT_DAMPING = 26;
const DEFAULT_MASS = 1;
const DEFAULT_REST_THRESHOLD = 0.001;
const MAX_STEP_SECONDS = 1 / 60;
const MAX_ACCUMULATED_SECONDS = 0.064;

export function spring(
  value: AnimatedValue<number>,
  config: SpringAnimationConfig,
): CompositeAnimation {
  validateSpringConfig(config);

  const stiffness = config.stiffness ?? DEFAULT_STIFFNESS;
  const damping = config.damping ?? DEFAULT_DAMPING;
  const mass = config.mass ?? DEFAULT_MASS;
  const restThreshold = config.restThreshold ?? DEFAULT_REST_THRESHOLD;

  let disposeFrame: (() => void) | null = null;
  let callback: AnimationEndCallback | undefined;
  let running = false;
  let settled = false;
  let state: SpringState = { x: value.getValue(), v: 0 };

  const animation: ActiveAnimation & CompositeAnimation = {
    start(cb?: AnimationEndCallback): void {
      if (running) animation.stopFromOwner(false);
      callback = cb;
      running = true;
      settled = false;
      state = { x: value.getValue(), v: 0 };
      replaceActiveAnimation(value, animation);
      disposeFrame = registerAnimatedFrameStep(step);
      if (isAtRest(state)) finish(true);
    },
    stop(): void {
      animation.stopFromOwner(false);
    },
    stopFromOwner(finished: boolean): void {
      finish(finished);
    },
  };

  const step = (dtMs: number): void => {
    if (!running) return;
    let remainingSeconds = Math.min(Math.max(0, dtMs) / 1000, MAX_ACCUMULATED_SECONDS);
    if (remainingSeconds === 0) return;

    while (remainingSeconds > 0) {
      const stepSeconds = Math.min(remainingSeconds, MAX_STEP_SECONDS);
      state = rk4(state, stepSeconds, config.to, stiffness, damping, mass);
      remainingSeconds -= stepSeconds;
    }

    value.setValue(state.x);
    if (isAtRest(state)) {
      value.setValue(config.to);
      finish(true);
    }
  };

  const finish = (finished: boolean): void => {
    if (settled) return;
    settled = true;
    running = false;
    disposeFrame?.();
    disposeFrame = null;
    clearActiveAnimation(value, animation);
    callback?.({ finished });
    callback = undefined;
  };

  const isAtRest = (candidate: SpringState): boolean =>
    Math.abs(candidate.v) <= restThreshold && Math.abs(candidate.x - config.to) <= restThreshold;

  return animation;
}

function rk4(
  state: SpringState,
  dt: number,
  target: number,
  stiffness: number,
  damping: number,
  mass: number,
): SpringState {
  const a = derivative(state, target, stiffness, damping, mass);
  const b = derivative(
    { x: state.x + a.dx * dt * 0.5, v: state.v + a.dv * dt * 0.5 },
    target,
    stiffness,
    damping,
    mass,
  );
  const c = derivative(
    { x: state.x + b.dx * dt * 0.5, v: state.v + b.dv * dt * 0.5 },
    target,
    stiffness,
    damping,
    mass,
  );
  const d = derivative(
    { x: state.x + c.dx * dt, v: state.v + c.dv * dt },
    target,
    stiffness,
    damping,
    mass,
  );

  return {
    x: state.x + (dt / 6) * (a.dx + 2 * b.dx + 2 * c.dx + d.dx),
    v: state.v + (dt / 6) * (a.dv + 2 * b.dv + 2 * c.dv + d.dv),
  };
}

function derivative(
  state: SpringState,
  target: number,
  stiffness: number,
  damping: number,
  mass: number,
): SpringDerivative {
  const displacement = state.x - target;
  return {
    dx: state.v,
    dv: (-stiffness * displacement - damping * state.v) / mass,
  };
}

function validateSpringConfig(config: SpringAnimationConfig): void {
  assertFiniteNumber(config.to, 'Animated.spring to');
  assertPositive(config.stiffness ?? DEFAULT_STIFFNESS, 'Animated.spring stiffness');
  const damping = config.damping ?? DEFAULT_DAMPING;
  assertFiniteNumber(damping, 'Animated.spring damping');
  if (damping < 0) throw new RangeError('Animated.spring damping must be >= 0');
  assertPositive(config.mass ?? DEFAULT_MASS, 'Animated.spring mass');
  assertPositive(config.restThreshold ?? DEFAULT_REST_THRESHOLD, 'Animated.spring restThreshold');
}

function assertPositive(value: number, label: string): void {
  assertFiniteNumber(value, label);
  if (value <= 0) throw new RangeError(`${label} must be > 0`);
}

function assertFiniteNumber(value: number, label: string): void {
  if (!Number.isFinite(value)) throw new TypeError(`${label} must be a finite number`);
}
