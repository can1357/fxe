import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { registerHitTarget } from '../mount/hit_test.ts';
import { paintImage } from '../paint/image_painter.ts';
import { Component, type Node, useId } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';

export interface ImageProps extends InternalLayoutProps, AccessibilityProps {
  key?: string;
  style?: StyleValue;
  source?: unknown;
  width?: number;
  height?: number;
  tint?: number;
}

export const Image = Component((props: ImageProps): Node => {
  const id = useId();
  const resolved = splitStyle([{ width: props.width, height: props.height }, props.style]);
  if (props.tint !== undefined) resolved.paint.tint = props.tint;
  const rect = rectFromStyle(resolved.layout, props.__layout);
  registerHitTarget({
    id,
    rect,
    a11y: { ...extractA11yProps(props), accessibilityRole: props.accessibilityRole ?? 'image' },
    componentType: 'Image',
    tabIndex: props.tabIndex,
  });
  return {
    type: 'draw',
    props: {
      deps: [props.source, props.style, rect.x, rect.y, rect.width, rect.height],
      fn: (cb) => paintImage(cb, rect, resolved.paint),
    },
  } as Node;
}, 'Image');
