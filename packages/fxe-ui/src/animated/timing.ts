export type AnimatedOutput = number | string;

export type AnimatedListener<T extends AnimatedOutput> = (value: T) => void;

export type ExtrapolateMode = 'extend' | 'clamp' | 'identity';

export interface InterpolationConfig<T extends AnimatedOutput = AnimatedOutput> {
  inputRange: readonly number[];
  outputRange: readonly T[];
  extrapolate?: ExtrapolateMode;
}

export type EasingName = 'linear' | 'ease' | 'ease-in' | 'ease-out' | 'ease-in-out';
export type EasingFunction = (t: number) => number;
export type Easing = EasingName | EasingFunction;

export const Easings = {
  linear: (t: number): number => t,
  ease: cubicBezier(0.25, 0.1, 0.25, 1),
  easeIn: cubicBezier(0.42, 0, 1, 1),
  easeOut: cubicBezier(0, 0, 0.58, 1),
  easeInOut: cubicBezier(0.42, 0, 0.58, 1),
  caEaseIn: cubicBezier(0.42, 0, 1, 1),
  caEaseOut: cubicBezier(0, 0, 0.58, 1),
  caEaseInEaseOut: cubicBezier(0.42, 0, 0.58, 1),
  caDefault: cubicBezier(0.25, 0.1, 0.25, 1),
  materialStandard: cubicBezier(0.4, 0, 0.2, 1),
  materialDecelerate: cubicBezier(0, 0, 0.2, 1),
  materialAccelerate: cubicBezier(0.4, 0, 1, 1),
} as const satisfies Record<string, EasingFunction>;
export interface TimingAnimationConfig {
  to: number;
  duration: number;
  easing?: Easing;
  delay?: number;
}

export interface AnimationEndResult {
  finished: boolean;
}

export type AnimationEndCallback = (result: AnimationEndResult) => void;

export interface CompositeAnimation {
  start(cb?: AnimationEndCallback): void;
  stop(): void;
}

type FrameStep = (dtMs: number) => void;

const g_animated_frame_steps = new Set<FrameStep>();
const kActiveAnimation = Symbol('fxe-ui.activeTimingAnimation');

export interface ActiveAnimation {
  stopFromOwner(finished: boolean): void;
  currentVelocity?(): number;
}

export class AnimatedValue<T extends AnimatedOutput = number> {
  current: T;

  #listeners = new Set<AnimatedListener<T>>();
  #disposeParent: (() => void) | null = null;

  constructor(initial: AnimatedOutput) {
    this.current = initial as T;
  }

  setValue(value: T): void {
    if (Object.is(this.current, value)) return;
    this.current = value;
    for (const listener of [...this.#listeners]) listener(value);
  }

  getValue(): T {
    return this.current;
  }

  addListener(fn: AnimatedListener<T>): () => void {
    this.#listeners.add(fn);
    return () => {
      this.#listeners.delete(fn);
    };
  }

  interpolate<U extends AnimatedOutput>(config: InterpolationConfig<U>): AnimatedValue<U> {
    validateInterpolationConfig(config);
    const derived = new AnimatedValue<U>(interpolateValue(asFiniteNumber(this.current), config));
    const unsubscribe = this.addListener((value) => {
      derived.setValue(interpolateValue(asFiniteNumber(value), config));
    });
    derived.#disposeParent = unsubscribe;
    return derived;
  }

  _dispose(): void {
    if (typeof this.current === 'number') {
      stopActiveAnimation(this as unknown as AnimatedValue<number>, false);
    }
    this.#listeners.clear();
    this.#disposeParent?.();
    this.#disposeParent = null;
  }
}

export function timing(
  value: AnimatedValue<number>,
  config: TimingAnimationConfig,
): CompositeAnimation {
  validateTimingConfig(config);
  let disposeFrame: (() => void) | null = null;
  let callback: AnimationEndCallback | undefined;
  let running = false;
  let settled = false;
  let elapsedMs = 0;
  let from = value.getValue();
  let sampledVelocity = 0;
  const durationMs = config.duration;
  const delayMs = config.delay ?? 0;
  const easing = resolveEasing(config.easing ?? 'ease');

  const animation: ActiveAnimation & CompositeAnimation = {
    start(cb?: AnimationEndCallback): void {
      if (running) animation.stopFromOwner(false);
      stopActiveAnimation(value, false);
      callback = cb;
      running = true;
      settled = false;
      elapsedMs = 0;
      from = value.getValue();
      sampledVelocity = 0;
      Reflect.set(value, kActiveAnimation, animation);

      if (durationMs === 0 && delayMs === 0) {
        value.setValue(config.to);
        finish(true);
        return;
      }

      disposeFrame = registerAnimatedFrameStep(step);
    },
    stop(): void {
      animation.stopFromOwner(false);
    },
    stopFromOwner(finished: boolean): void {
      finish(finished);
    },
    currentVelocity(): number {
      return sampledVelocity;
    },
  };

  const step = (dtMs: number): void => {
    if (!running) return;
    elapsedMs += Math.max(0, dtMs);
    if (elapsedMs < delayMs) return;

    const activeMs = elapsedMs - delayMs;
    const t = durationMs <= 0 ? 1 : clamp01(activeMs / durationMs);
    const eased = easing(t);
    if (!Number.isFinite(eased))
      throw new TypeError('Animated.timing easing returned a non-finite value');
    const previous = value.getValue();
    const next = from + (config.to - from) * eased;
    sampledVelocity = dtMs > 0 ? (next - previous) / (dtMs / 1000) : sampledVelocity;
    value.setValue(next);

    if (t >= 1) {
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
    if (Reflect.get(value, kActiveAnimation) === animation)
      Reflect.deleteProperty(value, kActiveAnimation);
    callback?.({ finished });
    callback = undefined;
  };

  return animation;
}

export function registerAnimatedFrameStep(step: FrameStep): () => void {
  g_animated_frame_steps.add(step);
  return () => {
    g_animated_frame_steps.delete(step);
  };
}

export function tickAnimatedFrames(dtMs: number): void {
  if (g_animated_frame_steps.size === 0) return;
  for (const step of [...g_animated_frame_steps]) step(dtMs);
}

export function replaceActiveAnimation(
  value: AnimatedValue<number>,
  animation: ActiveAnimation,
): void {
  stopActiveAnimation(value, false);
  Reflect.set(value, kActiveAnimation, animation);
}

export function clearActiveAnimation(
  value: AnimatedValue<number>,
  animation: ActiveAnimation,
): void {
  if (Reflect.get(value, kActiveAnimation) === animation)
    Reflect.deleteProperty(value, kActiveAnimation);
}

export function getActiveAnimationVelocity(value: AnimatedValue<number>): number | undefined {
  return (Reflect.get(value, kActiveAnimation) as ActiveAnimation | undefined)?.currentVelocity?.();
}

function stopActiveAnimation(value: AnimatedValue<number>, finished: boolean): void {
  const active = Reflect.get(value, kActiveAnimation) as ActiveAnimation | undefined;
  if (!active) return;
  active.stopFromOwner(finished);
}

function validateTimingConfig(config: TimingAnimationConfig): void {
  assertFiniteNumber(config.to, 'Animated.timing to');
  assertFiniteNumber(config.duration, 'Animated.timing duration');
  if (config.duration < 0) throw new RangeError('Animated.timing duration must be >= 0');
  if (config.delay !== undefined) {
    assertFiniteNumber(config.delay, 'Animated.timing delay');
    if (config.delay < 0) throw new RangeError('Animated.timing delay must be >= 0');
  }
  if (config.easing !== undefined && typeof config.easing !== 'function') {
    resolveEasing(config.easing);
  }
}

function resolveEasing(easing: Easing): EasingFunction {
  if (typeof easing === 'function') return easing;
  switch (easing) {
    case 'linear':
      return Easings.linear;
    case 'ease':
      return Easings.ease;
    case 'ease-in':
      return Easings.easeIn;
    case 'ease-out':
      return Easings.easeOut;
    case 'ease-in-out':
      return Easings.easeInOut;
    default:
      throw new RangeError(`unknown easing: ${String(easing)}`);
  }
}

export function cubicBezier(x1: number, y1: number, x2: number, y2: number): EasingFunction {
  assertFiniteNumber(x1, 'Animated.cubicBezier x1');
  assertFiniteNumber(y1, 'Animated.cubicBezier y1');
  assertFiniteNumber(x2, 'Animated.cubicBezier x2');
  assertFiniteNumber(y2, 'Animated.cubicBezier y2');
  assertUnitIntervalNumber(x1, 'Animated.cubicBezier x1');
  assertUnitIntervalNumber(x2, 'Animated.cubicBezier x2');
  return (t: number): number => {
    const targetX = clamp01(t);
    let lo = 0;
    let hi = 1;
    let u = targetX;
    for (let i = 0; i < 16; ++i) {
      u = (lo + hi) / 2;
      const x = bezier(u, x1, x2);
      if (x < targetX) lo = u;
      else hi = u;
    }
    return bezier(u, y1, y2);
  };
}

function bezier(t: number, p1: number, p2: number): number {
  const inv = 1 - t;
  return 3 * inv * inv * t * p1 + 3 * inv * t * t * p2 + t * t * t;
}

function validateInterpolationConfig(config: InterpolationConfig): void {
  if (config.inputRange.length !== config.outputRange.length) {
    throw new RangeError('interpolate inputRange and outputRange must have the same length');
  }
  if (config.inputRange.length < 2) {
    throw new RangeError('interpolate ranges must contain at least two entries');
  }
  for (let i = 0; i < config.inputRange.length; ++i) {
    assertFiniteNumber(config.inputRange[i], `interpolate inputRange[${i}]`);
    if (i > 0 && config.inputRange[i] <= config.inputRange[i - 1]) {
      throw new RangeError('interpolate inputRange values must be strictly increasing');
    }
  }
  if (
    config.extrapolate !== undefined &&
    config.extrapolate !== 'extend' &&
    config.extrapolate !== 'clamp' &&
    config.extrapolate !== 'identity'
  ) {
    throw new RangeError(`unknown extrapolate mode: ${String(config.extrapolate)}`);
  }
  for (let i = 1; i < config.outputRange.length; ++i) {
    interpolateOutput(config.outputRange[i - 1], config.outputRange[i], 0.5);
  }
}

function interpolateValue<T extends AnimatedOutput>(
  input: number,
  config: InterpolationConfig<T>,
): T {
  const { inputRange, outputRange } = config;
  const extrapolate = config.extrapolate ?? 'extend';

  if (input < inputRange[0]) {
    if (extrapolate === 'identity') return input as T;
    if (extrapolate === 'clamp') return outputRange[0];
  }
  const last = inputRange.length - 1;
  if (input > inputRange[last]) {
    if (extrapolate === 'identity') return input as T;
    if (extrapolate === 'clamp') return outputRange[last];
  }

  let segment = 0;
  while (segment < last - 1 && input > inputRange[segment + 1]) ++segment;
  const inMin = inputRange[segment];
  const inMax = inputRange[segment + 1];
  const t = (input - inMin) / (inMax - inMin);
  return interpolateOutput(outputRange[segment], outputRange[segment + 1], t) as T;
}

function interpolateOutput(from: AnimatedOutput, to: AnimatedOutput, t: number): AnimatedOutput {
  if (typeof from === 'number' && typeof to === 'number') {
    return from + (to - from) * t;
  }
  if (typeof from !== 'string' || typeof to !== 'string') {
    throw new TypeError('interpolate outputRange entries must use one output type per segment');
  }

  const fromColor = parseHexColor(from);
  const toColor = parseHexColor(to);
  if (fromColor && toColor) return formatHexColor(mixColor(fromColor, toColor, t));

  const fromDimension = parseDimension(from);
  const toDimension = parseDimension(to);
  if (fromDimension && toDimension && fromDimension.unit === toDimension.unit) {
    return `${fromDimension.value + (toDimension.value - fromDimension.value) * t}${fromDimension.unit}`;
  }

  if (t <= 0) return from;
  if (t >= 1) return to;
  throw new TypeError(
    'interpolate string outputRange entries must be hex colors or matching numeric units',
  );
}

interface RgbaColor {
  r: number;
  g: number;
  b: number;
  a: number;
  hasAlpha: boolean;
}

function parseHexColor(value: string): RgbaColor | null {
  const raw = value.startsWith('#') ? value.slice(1) : '';
  if (![3, 4, 6, 8].includes(raw.length) || !/^[0-9a-fA-F]+$/.test(raw)) return null;
  const expanded = raw.length <= 4 ? [...raw].map((ch) => `${ch}${ch}`).join('') : raw;
  const r = Number.parseInt(expanded.slice(0, 2), 16);
  const g = Number.parseInt(expanded.slice(2, 4), 16);
  const b = Number.parseInt(expanded.slice(4, 6), 16);
  const hasAlpha = expanded.length === 8;
  const a = hasAlpha ? Number.parseInt(expanded.slice(6, 8), 16) : 255;
  return { r, g, b, a, hasAlpha };
}

function mixColor(from: RgbaColor, to: RgbaColor, t: number): RgbaColor {
  return {
    r: Math.round(from.r + (to.r - from.r) * t),
    g: Math.round(from.g + (to.g - from.g) * t),
    b: Math.round(from.b + (to.b - from.b) * t),
    a: Math.round(from.a + (to.a - from.a) * t),
    hasAlpha: from.hasAlpha || to.hasAlpha,
  };
}

function formatHexColor(color: RgbaColor): string {
  const channels = color.hasAlpha
    ? [color.r, color.g, color.b, color.a]
    : [color.r, color.g, color.b];
  return `#${channels.map((channel) => channel.toString(16).padStart(2, '0')).join('')}`;
}

function parseDimension(value: string): { value: number; unit: string } | null {
  const match = /^(-?(?:\d+|\d*\.\d+))(.*)$/.exec(value);
  if (!match) return null;
  const amount = Number(match[1]);
  if (!Number.isFinite(amount)) return null;
  return { value: amount, unit: match[2] };
}

function asFiniteNumber(value: AnimatedOutput): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError('Animated.Value interpolation source must be a finite number');
  }
  return value;
}

function assertFiniteNumber(value: number, label: string): void {
  if (!Number.isFinite(value)) throw new TypeError(`${label} must be a finite number`);
}

function assertUnitIntervalNumber(value: number, label: string): void {
  if (value < 0 || value > 1) throw new TypeError(`${label} must be between 0 and 1`);
}

function clamp01(value: number): number {
  if (value <= 0) return 0;
  if (value >= 1) return 1;
  return value;
}
