import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { registerHitTarget, type SyntheticEvent } from '../mount/hit_test.ts';
import { type BoundaryChild, Component, type Node, useId, useState } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';
import { View, type ViewProps } from './View.ts';

export interface PressableState {
  hovered: boolean;
  pressed: boolean;
  focused: boolean;
}
export interface PressableProps extends InternalLayoutProps, AccessibilityProps {
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
type PressableInternalProps = PressableProps & { __componentType?: string };

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

function pressableA11yProps(props: PressableProps, componentType: string): AccessibilityProps {
  const a11y = extractA11yProps(props);
  if (!a11y.accessibilityRole && props.onPress) {
    a11y.accessibilityRole = 'button';
  }
  if (componentType === 'Button' && !a11y.accessibilityRole && props.onPress) {
    a11y.accessibilityRole = 'button';
  }
  return a11y;
}
export const Pressable = Component((props: PressableProps): Node => {
  const internalProps = props as PressableInternalProps;
  const id = useId();
  const [state, setState] = useState<PressableState>({
    hovered: false,
    pressed: false,
    focused: false,
  });
  currentPressableState = state;
  const style = typeof props.style === 'function' ? props.style(state) : props.style;
  const resolved = splitStyle(style);
  const rect = rectFromStyle(resolved.layout, props.__layout);
  const cursor =
    resolved.paint.cursor ?? (props.disabled ? 'notAllowed' : props.onPress ? 'hand' : 'arrow');
  const componentType = internalProps.__componentType ?? 'Pressable';
  const a11y = pressableA11yProps(props, componentType);
  if (props.disabled) {
    registerHitTarget({
      id,
      rect,
      cursor,
      a11y,
      componentType,
      tabIndex: a11y.tabIndex,
    });
  } else {
    registerHitTarget({
      id,
      rect,
      cursor,
      a11y,
      componentType,
      tabIndex: a11y.tabIndex,
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
  return View({ ...props, style, children, __skipA11yHitTarget: true } as ViewProps & {
    __skipA11yHitTarget: true;
  });
}, 'Pressable');
