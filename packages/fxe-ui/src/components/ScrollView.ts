import { CommandBuffer, Primitives } from 'fxe';
import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import type { LayoutResult, LayoutStyle } from '../layout/types.ts';
import {
  type HitTarget,
  hitTargets,
  materializeHitTarget,
  registerHitTarget,
} from '../mount/hit_test.ts';
import { coarseClip } from '../paint/clip.ts';
import {
  type BoundaryChild,
  Component,
  Draw,
  Layer,
  type Node,
  Portal,
  useInternalLayout,
  useInternalTextStyle,
  useRef,
  useState,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { attachInternalLayout, rectFromStyle } from './common.ts';
import { View } from './View.ts';

const SCROLL_LINE_PX = 48;
const SCROLLBAR_THICKNESS = 6;
const SCROLLBAR_INSET = 2;
const SCROLLBAR_MIN_THUMB = 24;
const SCROLLBAR_TRACK_COLOR = 0x00000033;
const SCROLLBAR_THUMB_COLOR = 0xffffff99;

type Offset = { x: number; y: number };
type Size = { width: number; height: number };

function numericLength(value: LayoutStyle[keyof LayoutStyle] | undefined): number | undefined {
  return typeof value === 'number' ? value : undefined;
}

function contentSize(rect: LayoutResult, contentStyle: StyleValue): Size {
  const layout = splitStyle(contentStyle).layout;
  let width = numericLength(layout.width) ?? rect.width;
  let height = numericLength(layout.height) ?? rect.height;
  for (const child of rect.children) {
    width = Math.max(width, child.x + child.width);
    height = Math.max(height, child.y + child.height);
  }
  return { width, height };
}

function maxOffset(rect: LayoutResult, content: Size): Offset {
  return {
    x: Math.max(0, content.width - rect.width),
    y: Math.max(0, content.height - rect.height),
  };
}

function clampOffset(offset: Offset, max: Offset): Offset {
  return {
    x: Math.min(Math.max(0, offset.x), max.x),
    y: Math.min(Math.max(0, offset.y), max.y),
  };
}

// Child buffers are rendered at their unscrolled coordinates; applying a
// fractional scroll translation afterward moves already-rasterised glyph quads
// off their pixel grid. Snap only the paint/hit-test projection — keep the
// logical scroll offset fractional for wheel accumulation and scrollbar math.
export function __snapScrollOffsetForPaint(offset: Offset): Offset {
  return {
    x: Math.round(offset.x),
    y: Math.round(offset.y),
  };
}

function sameOffset(a: Offset, b: Offset): boolean {
  return a.x === b.x && a.y === b.y;
}

function combineSize(a: Size, b: Size | null): Size {
  if (b === null) return a;
  return {
    width: Math.max(a.width, b.width),
    height: Math.max(a.height, b.height),
  };
}

function measuredContentSize(
  rect: LayoutResult,
  contentStyle: StyleValue,
  bounds: { x: number; y: number; width: number; height: number } | null,
): Size {
  const base = contentSize(rect, contentStyle);
  if (bounds === null) return base;
  return {
    width: Math.max(base.width, bounds.x + bounds.width - rect.x),
    height: Math.max(base.height, bounds.y + bounds.height - rect.y),
  };
}

function clipChildHitTargets(start: number, clip: LayoutResult, offset: Offset): void {
  const targets = hitTargets() as HitTarget[];
  for (let i = targets.length - 1; i >= start; --i) {
    const target = targets[i];
    const shifted = {
      ...target.rect,
      x: target.rect.x - offset.x,
      y: target.rect.y - offset.y,
    };
    const left = Math.max(shifted.x, clip.x);
    const top = Math.max(shifted.y, clip.y);
    const right = Math.min(shifted.x + shifted.width, clip.x + clip.width);
    const bottom = Math.min(shifted.y + shifted.height, clip.y + clip.height);
    if (right <= left || bottom <= top) {
      targets.splice(i, 1);
    } else {
      materializeHitTarget(targets, i).rect = {
        ...shifted,
        x: left,
        y: top,
        width: right - left,
        height: bottom - top,
      };
    }
  }
}

function paintScrollbars(
  cb: CommandBuffer,
  rect: LayoutResult,
  content: Size,
  offset: Offset,
): void {
  const max = maxOffset(rect, content);
  const vertical = max.y > 0;
  const horizontal = max.x > 0;
  if (!vertical && !horizontal) return;

  const thickness = SCROLLBAR_THICKNESS;
  const inset = SCROLLBAR_INSET;
  const corner = vertical && horizontal ? thickness + inset : 0;

  if (vertical) {
    const trackX = rect.x + rect.width - thickness - inset;
    const trackY = rect.y + inset;
    const trackH = Math.max(0, rect.height - inset * 2 - corner);
    if (trackH > 0) {
      const thumbH = Math.max(SCROLLBAR_MIN_THUMB, (rect.height / content.height) * trackH);
      const travel = Math.max(0, trackH - thumbH);
      const thumbY = trackY + (max.y === 0 ? 0 : (offset.y / max.y) * travel);
      Primitives.fillRect(cb, trackX, trackY, thickness, trackH, 0, SCROLLBAR_TRACK_COLOR);
      Primitives.fillRect(
        cb,
        trackX,
        thumbY,
        thickness,
        Math.min(trackH, thumbH),
        0,
        SCROLLBAR_THUMB_COLOR,
      );
    }
  }

  if (horizontal) {
    const trackX = rect.x + inset;
    const trackY = rect.y + rect.height - thickness - inset;
    const trackW = Math.max(0, rect.width - inset * 2 - corner);
    if (trackW > 0) {
      const thumbW = Math.max(SCROLLBAR_MIN_THUMB, (rect.width / content.width) * trackW);
      const travel = Math.max(0, trackW - thumbW);
      const thumbX = trackX + (max.x === 0 ? 0 : (offset.x / max.x) * travel);
      Primitives.fillRect(cb, trackX, trackY, trackW, thickness, 0, SCROLLBAR_TRACK_COLOR);
      Primitives.fillRect(
        cb,
        thumbX,
        trackY,
        Math.min(trackW, thumbW),
        thickness,
        0,
        SCROLLBAR_THUMB_COLOR,
      );
    }
  }
}
export interface ScrollViewProps extends AccessibilityProps {
  key?: string;
  style?: StyleValue;
  contentStyle?: StyleValue;
  children?: BoundaryChild;
  onScroll?: (offset: { x: number; y: number }) => void;
}

export const ScrollView = Component((props: ScrollViewProps): Node => {
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const measuredContentRef = useRef<Size | null>(null);
  const offsetRef = useRef(offset);
  const rect = rectFromStyle(splitStyle(props.style).layout, useInternalLayout() ?? undefined);
  const inheritedTextStyle = useInternalTextStyle();
  const layoutContent = contentSize(rect, props.contentStyle);
  const content = combineSize(layoutContent, measuredContentRef.current);
  const max = maxOffset(rect, content);
  const clampedOffset = clampOffset(offset, max);
  const paintOffset = __snapScrollOffsetForPaint(clampedOffset);
  offsetRef.current = clampedOffset;
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
      const currentContent = combineSize(layoutContent, measuredContentRef.current);
      const currentMax = maxOffset(rect, currentContent);
      const currentOffset = clampOffset(offsetRef.current, currentMax);
      const next = clampOffset(
        {
          x: currentOffset.x + ev.dx * SCROLL_LINE_PX,
          y: currentOffset.y - ev.dy * SCROLL_LINE_PX,
        },
        currentMax,
      );
      offsetRef.current = next;
      setOffset((prev) => (sameOffset(prev, next) ? prev : next));
      props.onScroll?.(next);
    },
  });
  const childBuffer = new CommandBuffer();
  let hitTargetStart = 0;
  const contentNode = attachInternalLayout(
    View({
      style: props.contentStyle,
      children: props.children,
    }),
    rect,
    inheritedTextStyle ?? undefined,
  );
  const background = attachInternalLayout(
    View({ ...props, children: undefined }),
    rect,
    inheritedTextStyle ?? undefined,
  );
  const markHitTargetStart = Draw(() => {
    hitTargetStart = hitTargets().length;
  });
  const clippedContentAndScrollbar = Draw((cb) => {
    const measured = measuredContentSize(rect, props.contentStyle, childBuffer.bounds());
    measuredContentRef.current = measured;
    clipChildHitTargets(hitTargetStart, rect, paintOffset);
    const clipped = coarseClip(childBuffer, rect, {
      x: -paintOffset.x,
      y: -paintOffset.y,
    });
    if (clipped.__fxe_v_len !== 0) cb.queue(clipped);
    paintScrollbars(cb, rect, measured, clampedOffset);
  });
  return Layer({
    children: [
      background,
      markHitTargetStart,
      Portal({ to: childBuffer, children: contentNode }),
      clippedContentAndScrollbar,
    ],
  });
}, 'ScrollView');
