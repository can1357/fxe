import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { registerHitTarget } from '../mount/hit_test.ts';
import { type BoundaryChild, Component, type Node, useState } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { type InternalLayoutProps, rectFromStyle } from './common.ts';
import { View } from './View.ts';
import { INTERNAL_LAYOUT, INTERNAL_TEXT_STYLE } from '../internal_keys.ts';

export interface ScrollViewProps extends InternalLayoutProps, AccessibilityProps {
  key?: string;
  style?: StyleValue;
  contentStyle?: StyleValue;
  children?: BoundaryChild;
  onScroll?: (offset: { x: number; y: number }) => void;
}

export const ScrollView = Component((props: ScrollViewProps): Node => {
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const rect = rectFromStyle(splitStyle(props.style).layout, props[INTERNAL_LAYOUT]);
  registerHitTarget({
    id: `scroll:${rect.x}:${rect.y}`,
    rect,
    a11y: {
      ...extractA11yProps(props),
      accessibilityRole: props.accessibilityRole ?? 'scrollview',
    },
    componentType: 'ScrollView',
    tabIndex: props.tabIndex,
    onWheel: (ev) => {
      const next = { x: Math.max(0, offset.x + ev.dx), y: Math.max(0, offset.y + ev.dy) };
      setOffset(next);
      props.onScroll?.(next);
    },
  });
  return View({
    ...props,
    style: [{ overflow: 'hidden' as const }, props.style],
    children: View({
      style: [{ position: 'relative', left: -offset.x, top: -offset.y }, props.contentStyle],
      children: props.children,
    }),
  });
}, 'ScrollView');
