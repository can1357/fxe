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

export const Button = Component((props: ButtonProps): Node => {
  const theme = useTheme();
  const label = props.title ?? props.children ?? 'Button';
  return Pressable({
    ...props,
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
    children: Text({
      style: [{ color: theme.colors.primaryText, fontSize: theme.fontSizes.md }, props.textStyle],
      children: label,
    }),
  });
}, 'Button');
