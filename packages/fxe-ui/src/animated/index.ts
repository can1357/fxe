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
  cubicBezier,
  type CompositeAnimation,
  type Easing,
  Easings,
  type EasingFunction,
  type EasingName,
  type ExtrapolateMode,
  type InterpolationConfig,
  type TimingAnimationConfig,
  timing,
} from './timing.ts';

export {
  type AnimatedListener,
  type AnimatedOutput,
  AnimatedValue,
  type AnimationEndCallback,
  type AnimationEndResult,
  cubicBezier,
  type CompositeAnimation,
  type Easing,
  Easings,
  type EasingFunction,
  type EasingName,
  type ExtrapolateMode,
  type InterpolationConfig,
  type SpringAnimationConfig,
  type SpringPresetName,
  spring,
  springPreset,
  springPresets,
  type TimingAnimationConfig,
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
