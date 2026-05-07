import { paintImage } from '../paint/image_painter.ts';
import { Component, type Node } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';

export interface ImageProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue;
  source?: unknown;
  width?: number;
  height?: number;
  tint?: number;
}

export const Image = Component((props: ImageProps): Node => {
  const resolved = splitStyle([{ width: props.width, height: props.height }, props.style]);
  if (props.tint !== undefined) resolved.paint.tint = props.tint;
  const rect = rectFromStyle(resolved.layout, props.__layout);
  return {
    type: 'draw',
    props: {
      deps: [props.source, props.style, rect.x, rect.y, rect.width, rect.height],
      fn: (cb) => paintImage(cb, rect, resolved.paint),
    },
  } as Node;
}, 'Image');
