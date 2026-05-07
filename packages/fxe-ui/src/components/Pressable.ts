import { registerHitTarget, type SyntheticEvent } from '../mount/hit_test.ts';
import { type BoundaryChild, Component, type Node, useId, useState } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';
import { View } from './View.ts';

export interface PressableState {
  hovered: boolean;
  pressed: boolean;
  focused: boolean;
}
export interface PressableProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue | ((state: PressableState) => StyleValue);
  children?: BoundaryChild | ((state: PressableState) => BoundaryChild);
  disabled?: boolean;
  onPress?: (ev: SyntheticEvent) => void;
  onPressIn?: (ev: SyntheticEvent) => void;
  onPressOut?: (ev: SyntheticEvent) => void;
  onHoverIn?: (ev: SyntheticEvent) => void;
  onHoverOut?: (ev: SyntheticEvent) => void;
  onFocus?: () => void;
  onBlur?: () => void;
  onLongPress?: (ev: SyntheticEvent) => void;
}

let currentPressableState: PressableState = { hovered: false, pressed: false, focused: false };

export function usePressableState(): PressableState {
  return currentPressableState;
}
export function useHover(): boolean {
  return currentPressableState.hovered;
}
export function useFocus(): boolean {
  return currentPressableState.focused;
}

export const Pressable = Component((props: PressableProps): Node => {
  const id = useId();
  const [state, setState] = useState<PressableState>({
    hovered: false,
    pressed: false,
    focused: false,
  });
  currentPressableState = state;
  const style = typeof props.style === 'function' ? props.style(state) : props.style;
  const rect = rectFromStyle(splitStyle(style).layout, props.__layout);
  if (!props.disabled) {
    registerHitTarget({
      id,
      rect,
      cursor: splitStyle(style).paint.cursor ?? 'hand',
      onHoverIn: (ev) => {
        setState((s) => ({ ...s, hovered: true }));
        props.onHoverIn?.(ev);
      },
      onHoverOut: (ev) => {
        setState((s) => ({ ...s, hovered: false, pressed: false }));
        props.onHoverOut?.(ev);
      },
      onPressIn: (ev) => {
        setState((s) => ({ ...s, pressed: true }));
        props.onPressIn?.(ev);
      },
      onPressOut: (ev) => {
        setState((s) => ({ ...s, pressed: false }));
        props.onPressOut?.(ev);
      },
      onPress: props.onPress,
      onFocus: () => {
        setState((s) => ({ ...s, focused: true }));
        props.onFocus?.();
      },
      onBlur: () => {
        setState((s) => ({ ...s, focused: false, pressed: false }));
        props.onBlur?.();
      },
    });
  }
  const children = typeof props.children === 'function' ? props.children(state) : props.children;
  return View({ ...props, style, children });
}, 'Pressable');
