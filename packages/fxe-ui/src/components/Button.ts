import type { AccessibilityProps } from '../a11y/types.ts';
import { extractA11yProps } from '../a11y/extract.ts';
import { Component, type Node } from '../reconciler/fiber.ts';
import type { StyleValue } from '../style/types.ts';
import { useTheme } from '../theme/provider.ts';
import { Pressable, type PressableProps } from './Pressable.ts';
import { Text } from './Text.ts';

export interface ButtonProps extends Omit<PressableProps, 'children'> {
  title?: string;
  children?: string;
  textStyle?: StyleValue;
}

function buttonA11yProps(props: ButtonProps, label: string): AccessibilityProps {
  const a11y = extractA11yProps(props);
  a11y.accessibilityLabel ??= label;
  a11y.accessibilityRole ??= props.onPress ? 'button' : undefined;
  return a11y;
}
export const Button = Component((props: ButtonProps): Node => {
  const theme = useTheme();
  const label = props.title ?? props.children ?? 'Button';
  const a11yProps = buttonA11yProps(props, label);
  return Pressable({
    ...props,
    ...a11yProps,
    style: [
      {
        minHeight: 36,
        paddingX: theme.spacing.lg,
        paddingY: theme.spacing.sm,
        borderRadius: theme.radii.md,
        backgroundColor: props.disabled ? theme.colors.border : theme.colors.primary,
        alignItems: 'center',
        justifyContent: 'center',
      },
      typeof props.style === 'function'
        ? props.style({ hovered: false, pressed: false, focused: false })
        : props.style,
    ],
    __componentType: 'Button',
    children: Text({
      style: [{ color: theme.colors.primaryText, fontSize: theme.fontSizes.md }, props.textStyle],
      accessibilityLabel: a11yProps.accessibilityLabel,
      children: label,
    }),
  } as PressableProps & { __componentType: 'Button' });
}, 'Button');
