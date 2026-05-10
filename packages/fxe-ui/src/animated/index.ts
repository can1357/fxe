import { useEffect, useRef } from '../reconciler/fiber.ts';
import {
  type SpringAnimationConfig,
  type SpringPresetName,
  spring,
  springPreset,
  springPresets,
} from './spring.ts';
import {
  type AnimatedListener,
  type AnimatedOutput,
  AnimatedValue,
  type AnimationEndCallback,
  type AnimationEndResult,
  type CompositeAnimation,
  cubicBezier,
  type Easing,
  type EasingFunction,
  type EasingName,
  Easings,
  type ExtrapolateMode,
  type InterpolationConfig,
  registerAnimatedFrameStep,
  type TimingAnimationConfig,
  tickAnimatedFrames,
  timing,
} from './timing.ts';

export {
  type AnimatedListener,
  type AnimatedOutput,
  AnimatedValue,
  type AnimationEndCallback,
  type AnimationEndResult,
  type CompositeAnimation,
  cubicBezier,
  type Easing,
  type EasingFunction,
  type EasingName,
  Easings,
  type ExtrapolateMode,
  type InterpolationConfig,
  registerAnimatedFrameStep,
  type SpringAnimationConfig,
  type SpringPresetName,
  spring,
  springPreset,
  springPresets,
  type TimingAnimationConfig,
  tickAnimatedFrames,
  timing,
};

interface AnimatedNamespace {
  readonly Value: new (initial: number) => AnimatedValue<number>;
  readonly timing: typeof timing;
  readonly spring: typeof spring;
  readonly Easings: typeof Easings;
}

export const Animated: AnimatedNamespace = {
  Value: AnimatedValue,
  timing,
  spring,
  Easings,
} as const;

export function useAnimatedValue(initial: number): AnimatedValue<number> {
  const ref = useRef<AnimatedValue<number> | null>(null);
  if (ref.current === null) ref.current = new AnimatedValue(initial);

  useEffect(() => {
    const value = ref.current;
    return () => {
      value?._dispose();
    };
  }, []);

  return ref.current;
}
