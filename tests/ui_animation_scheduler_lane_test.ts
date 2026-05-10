// Regression for TODO T1: "Animation scheduler interleaves with reconciler scheduler".
//
// Verifies that `tickAnimatedFrames` (driven by `tickFrame`) always runs with
// `currentLane === 'sync'`, even while a `useTransition` update is pending.
// If that ever regresses, every active spring/timing animation would be
// silently demoted to the transition lane and could be deferred behind a
// long-running transition flush — visible immediately as 'animations stutter
// while user types' UX bugs.

import { CommandBuffer } from 'fxe';
import {
  Animated,
  Component,
  Draw,
  getCurrentSchedulerLane,
  Layer,
  registerAnimatedFrameStep,
  render,
  runWithSchedulerLane,
  tickFrame,
  useAnimatedValue,
  useEffect,
} from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

test('tickAnimatedFrames runs on the sync lane', () => {
  const observed: string[] = [];
  const dispose = registerAnimatedFrameStep(() => {
    observed.push(getCurrentSchedulerLane());
  });
  try {
    tickFrame(16);
    tickFrame(16);
  } finally {
    dispose();
  }
  assert(observed.length >= 2, `expected at least 2 frame ticks, got ${observed.length}`);
  for (const lane of observed) {
    assertEqual(lane, 'sync');
  }
});

test('animation listeners observe sync lane even while transition lane has pending work', () => {
  // Stage a transition-lane fiber update so `transitionFibers`/`transitionCallbacks`
  // is non-empty when `tickAnimatedFrames` runs.
  let setterLane: string | null = null;
  runWithSchedulerLane('transition', () => {
    // Confirms the scope flips lane.
    setterLane = getCurrentSchedulerLane();
  });
  assertEqual(setterLane, 'transition');

  const value = new Animated.Value(0);
  const lanesDuringStep: string[] = [];

  value.addListener(() => {
    lanesDuringStep.push(getCurrentSchedulerLane());
  });

  Animated.timing(value, { to: 100, duration: 100, easing: 'linear' }).start();

  // Drive several frames.
  tickFrame(16);
  tickFrame(16);
  tickFrame(16);

  assert(
    lanesDuringStep.length > 0,
    `expected animation step to fire listeners, got ${lanesDuringStep.length}`,
  );
  for (const lane of lanesDuringStep) {
    assertEqual(lane, 'sync', `animation listener observed lane=${lane}, expected 'sync'`);
  }
});

test('Animated value drives a setState while a useTransition update is pending', () => {
  const observed: { lane: string; value: number }[] = [];
  const triggerRef: { current: (() => void) | null } = { current: null };

  const Probe = Component(() => {
    const value = useAnimatedValue(0);
    useEffect(() => {
      // Wire a listener that records the current lane each time the value moves.
      const dispose = value.addListener((next) => {
        observed.push({ lane: getCurrentSchedulerLane(), value: next });
      });
      const animation = Animated.timing(value, {
        to: 100,
        duration: 100,
        easing: 'linear',
      });
      animation.start();
      // Expose a way for the outer test to enqueue a transition while the
      // animation is mid-flight.
      triggerRef.current = () => {
        runWithSchedulerLane('transition', () => {
          // Lane is set; nothing else needed for this regression — the danger
          // is the animation observing this lane during its frame tick.
        });
      };
      return () => {
        animation.stop();
        dispose();
      };
    }, [value]);
    return Draw(() => undefined);
  }, 'AnimatedLaneProbe');

  const target = new CommandBuffer();
  render(Layer({ children: [Probe({})] }), target);

  tickFrame(16);
  // Simulate a transition update happening between frames.
  if (triggerRef.current) triggerRef.current();
  tickFrame(16);
  if (triggerRef.current) triggerRef.current();
  tickFrame(16);

  // Cleanup
  render(Layer({ children: [] }), target);

  assert(observed.length >= 2, `expected animation listener to fire, got ${observed.length}`);
  for (const { lane, value } of observed) {
    assertEqual(
      lane,
      'sync',
      `animation observed lane=${lane} at value=${value}; expected 'sync' (animation was demoted to transition lane)`,
    );
  }
});

await run();
