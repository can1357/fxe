import { useEffect, useRef } from '../reconciler/fiber.ts';
import { type SpringAnimationConfig, spring } from './spring.ts';
import {
  type AnimatedListener,
  type AnimatedOutput,
  AnimatedValue,
  type AnimationEndCallback,
  type AnimationEndResult,
  type CompositeAnimation,
  type Easing,
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
  type CompositeAnimation,
  type Easing,
  type EasingFunction,
  type EasingName,
  type ExtrapolateMode,
  type InterpolationConfig,
  type SpringAnimationConfig,
  spring,
  type TimingAnimationConfig,
  timing,
};

interface AnimatedNamespace {
  readonly Value: new (initial: number) => AnimatedValue<number>;
  readonly timing: typeof timing;
  readonly spring: typeof spring;
}

export const Animated: AnimatedNamespace = {
  Value: AnimatedValue,
  timing,
  spring,
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
