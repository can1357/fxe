import { CommandBuffer, type Mat4, type Paint, Primitives } from 'fxe';
import { Component, Draw, type Node, useInternalLayout, useMemo } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { rectFromStyle } from './common.ts';
import { ICON_PATHS, type IconName, type IconPathCommand } from './icons.ts';
import { View, type ViewProps } from './View.ts';

const DEFAULT_COLOR = 0x111827ff;
const DEFAULT_SIZE = 24;
const DEFAULT_STROKE_WIDTH = 2;
const VIEWBOX_SIZE = 24;

export interface IconProps {
  key?: string;
  name: IconName;
  size?: number;
  color?: Paint;
  strokeWidth?: number;
  strokeLineJoin?: 'miter' | 'bevel' | 'round';
  strokeLineCap?: 'butt' | 'square' | 'round';
  style?: StyleValue;
}

function translationMatrix(x: number, y: number): Mat4 {
  return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, 0, 0]);
}

function buildPath(commands: readonly IconPathCommand[], size: number) {
  const scale = size / VIEWBOX_SIZE;
  const path = new Path();
  for (const command of commands) {
    switch (command.op) {
      case 'M':
        path.moveTo(command.x * scale, command.y * scale);
        break;
      case 'L':
        path.lineTo(command.x * scale, command.y * scale);
        break;
      case 'C':
        path.cubicTo(
          command.c1x * scale,
          command.c1y * scale,
          command.c2x * scale,
          command.c2y * scale,
          command.x * scale,
          command.y * scale,
        );
        break;
      case 'Q':
        path.quadTo(command.cx * scale, command.cy * scale, command.x * scale, command.y * scale);
        break;
      case 'A':
        path.arc(
          command.cx * scale,
          command.cy * scale,
          command.r * scale,
          command.start,
          command.end,
          command.ccw,
        );
        break;
      case 'Z':
        path.close();
        break;
    }
  }
  return path;
}

export const Icon = Component((props: IconProps): Node => {
  const size = props.size ?? DEFAULT_SIZE;
  const color = props.color ?? DEFAULT_COLOR;
  const strokeLineJoin = props.strokeLineJoin ?? 'round';
  const strokeLineCap = props.strokeLineCap ?? 'round';
  const style = [props.style, { width: size, height: size }];
  const resolved = splitStyle(style);
  const rect = rectFromStyle(resolved.layout, useInternalLayout() ?? undefined);
  const path = useMemo(() => buildPath(ICON_PATHS[props.name], size), [props.name, size]);
  const lineWidth = (props.strokeWidth ?? DEFAULT_STROKE_WIDTH) * (size / VIEWBOX_SIZE);
  const strokeBuffer = useMemo(() => {
    const buffer = new CommandBuffer();
    Primitives.strokePath(buffer, path, color, lineWidth, strokeLineJoin, strokeLineCap);
    return buffer;
  }, [path, color, lineWidth, strokeLineJoin, strokeLineCap]);
  const matrix = useMemo(() => translationMatrix(rect.x, rect.y), [rect.x, rect.y]);
  const drawNode = Draw(
    (cb: CommandBuffer) => {
      if (rect.width <= 0 || rect.height <= 0) return;
      cb.queue(strokeBuffer, matrix);
    },
    [strokeBuffer, matrix, rect.width, rect.height],
  );
  const view = View({
    key: props.key,
    style,
    children: drawNode,
    __skipA11yHitTarget: true,
  } as ViewProps & { __skipA11yHitTarget: true });
  return view.type === 'component' ? { ...view, internalLayout: rect } : view;
}, 'Icon');
