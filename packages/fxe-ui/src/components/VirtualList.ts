import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityNodeSnapshot, AccessibilityProps } from '../a11y/types.ts';
import { registerVirtualSource } from '../a11y/virtual.ts';
import {
  type BoundaryChild,
  Component,
  type Node,
  useEffect,
  useId,
  useInternalLayout,
  useMemo,
  useRef,
  useState,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { rectFromStyle } from './common.ts';
import { ScrollView } from './ScrollView.ts';
import { View } from './View.ts';

export type VirtualItemHeight = number | ((index: number) => number);

export interface VirtualListProps<T> extends AccessibilityProps {
  key?: string;
  style?: StyleValue;
  contentStyle?: StyleValue;
  data: readonly T[];
  itemHeight: VirtualItemHeight;
  estimatedItemHeight?: number;
  overscan?: number;
  renderItem: (item: T, index: number) => Node;
  keyExtractor?: (item: T, index: number) => string;
  onScroll?: (offset: { x: number; y: number }) => void;
  getItemAccessibilityLabel?: (index: number) => string;
}

interface VirtualListRowProps {
  key?: string;
  top: number;
  width: number;
  fixedHeight?: number;
  onMeasure?: (height: number) => void;
  children?: BoundaryChild;
}

const VirtualListRow = Component((props: VirtualListRowProps): Node => {
  const measuredHeight = useInternalLayout()?.height;

  useEffect(() => {
    if (props.onMeasure === undefined || measuredHeight === undefined || measuredHeight <= 0) {
      return;
    }
    props.onMeasure(measuredHeight);
  }, [props.onMeasure, measuredHeight]);

  return View({
    style: {
      position: 'absolute',
      left: 0,
      top: props.top,
      width: props.width > 0 ? props.width : undefined,
      height: props.fixedHeight,
    },
    children: props.children,
  });
}, 'VirtualListRow');

interface HeightIndex {
  readonly totalHeight: number;
  offsetFor(index: number): number;
  heightFor(index: number): number;
  indexAt(offset: number): number;
}

class FixedHeightIndex implements HeightIndex {
  readonly totalHeight: number;

  constructor(
    private readonly count: number,
    private readonly height: number,
  ) {
    this.totalHeight = count * height;
  }

  offsetFor(index: number): number {
    return clampIndex(index, this.count) * this.height;
  }

  heightFor(_index: number): number {
    return this.height;
  }

  indexAt(offset: number): number {
    if (this.count === 0) return 0;
    if (this.height === 0) return 0;
    return Math.min(this.count - 1, Math.max(0, Math.floor(offset / this.height)));
  }
}

class VariableHeightIndex implements HeightIndex {
  private readonly prefix: number[] = [0];

  constructor(
    private readonly count: number,
    private readonly estimateFor: (index: number) => number,
    private readonly measuredHeights: ReadonlyMap<number, number>,
  ) {}

  get totalHeight(): number {
    this.ensurePrefix(this.count);
    return this.prefix[this.count] ?? 0;
  }

  offsetFor(index: number): number {
    const bounded = clampIndex(index, this.count);
    this.ensurePrefix(bounded);
    return this.prefix[bounded] ?? 0;
  }

  heightFor(index: number): number {
    if (index < 0 || index >= this.count) return 0;
    return this.measuredHeights.get(index) ?? this.estimateFor(index);
  }

  indexAt(offset: number): number {
    if (this.count === 0) return 0;
    const target = Math.max(0, offset);
    let lo = 0;
    let hi = this.count;

    while (lo < hi) {
      const mid = Math.floor((lo + hi) / 2);
      this.ensurePrefix(mid + 1);
      if ((this.prefix[mid + 1] ?? 0) <= target) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }

    return Math.min(this.count - 1, lo);
  }

  private ensurePrefix(count: number): void {
    while (this.prefix.length <= count) {
      const index = this.prefix.length - 1;
      this.prefix.push((this.prefix[index] ?? 0) + this.heightFor(index));
    }
  }
}

function clampIndex(index: number, count: number): number {
  if (index <= 0 || count <= 0) return 0;
  if (index >= count) return count;
  return index;
}

function cleanCount(value: number | undefined, fallback: number): number {
  if (value === undefined) return fallback;
  if (!Number.isFinite(value)) return fallback;
  return Math.max(0, Math.floor(value));
}

function validHeight(value: number, label: string): number {
  if (!Number.isFinite(value) || value < 0) {
    throw new TypeError(`${label} must be a finite non-negative number`);
  }
  return value;
}

function virtualSourceCount(data: unknown): number | null {
  if (Array.isArray(data)) return data.length;
  if (typeof data !== 'object' || data === null || !('length' in data)) return null;
  const length = data.length;
  if (typeof length !== 'number' || !Number.isFinite(length)) return null;
  return Math.max(0, Math.floor(length));
}

function rangeFor(
  index: HeightIndex,
  count: number,
  offsetY: number,
  viewportHeight: number,
  overscan: number,
) {
  if (count === 0) return { start: 0, end: 0 };
  const first = index.indexAt(offsetY);
  const last = index.indexAt(offsetY + Math.max(0, viewportHeight));
  return {
    start: Math.max(0, first - overscan),
    end: Math.min(count, last + overscan + 1),
  };
}

export const VirtualList = Component(<T>(props: VirtualListProps<T>): Node => {
  const accessibilityId = props.accessibilityId ?? useId();
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const [heightVersion, setHeightVersion] = useState(0);
  const measuredRef = useRef<{
    data: readonly T[];
    heights: Map<number, number>;
  }>({ data: props.data, heights: new Map<number, number>() });

  if (measuredRef.current.data !== props.data) {
    measuredRef.current = { data: props.data, heights: new Map<number, number>() };
  }

  const rect = rectFromStyle(splitStyle(props.style).layout, useInternalLayout() ?? undefined);
  const count = props.data.length;
  const overscan = cleanCount(props.overscan, 5);
  const fixedHeight =
    typeof props.itemHeight === 'number' ? validHeight(props.itemHeight, 'itemHeight') : undefined;
  const variableHeight = typeof props.itemHeight === 'function' ? props.itemHeight : undefined;
  const estimatedFallback = validHeight(
    props.estimatedItemHeight ?? fixedHeight ?? 0,
    'estimatedItemHeight',
  );

  const heightIndex = useMemo<HeightIndex>(() => {
    if (fixedHeight !== undefined) {
      return new FixedHeightIndex(count, fixedHeight);
    }

    if (variableHeight === undefined) {
      return new VariableHeightIndex(count, () => estimatedFallback, measuredRef.current.heights);
    }

    return new VariableHeightIndex(
      count,
      (index) => {
        const estimate = variableHeight(index);
        return validHeight(
          estimate === 0 && estimatedFallback > 0 ? estimatedFallback : estimate,
          `itemHeight(${index})`,
        );
      },
      measuredRef.current.heights,
    );
  }, [count, fixedHeight, variableHeight, estimatedFallback, props.data, heightVersion]);

  const visibleRange = rangeFor(heightIndex, count, offset.y, rect.height, overscan);
  const lastVisibleIndex =
    visibleRange.end > visibleRange.start ? visibleRange.end - 1 : visibleRange.start - 1;
  const totalHeight = heightIndex.totalHeight;

  useEffect(() => {
    const totalCount = virtualSourceCount(props.data);
    if (totalCount === null) return;
    return registerVirtualSource({
      parentId: accessibilityId,
      totalCount,
      renderedRange: [visibleRange.start, lastVisibleIndex],
      buildVirtualNode(index: number): AccessibilityNodeSnapshot {
        return {
          id: `${accessibilityId}-row-${index}`,
          parentId: accessibilityId,
          role: 'listitem',
          label: props.getItemAccessibilityLabel?.(index) ?? '',
          state: { offscreen: true },
          rect: { x: 0, y: 0, width: 0, height: 0 },
          focusable: false,
          liveRegion: 'off',
          children: [],
        };
      },
    });
  }, [
    accessibilityId,
    lastVisibleIndex,
    props.data,
    props.getItemAccessibilityLabel,
    visibleRange.start,
  ]);

  const children = useMemo(() => {
    const nodes: Node[] = [];
    for (let index = visibleRange.start; index < visibleRange.end; ++index) {
      const item = props.data[index];
      if (item === undefined) continue;
      const key = props.keyExtractor?.(item, index) ?? String(index);
      const top = heightIndex.offsetFor(index);
      const measuredHeight = fixedHeight === undefined ? undefined : heightIndex.heightFor(index);
      const onMeasure =
        fixedHeight === undefined
          ? (height: number) => {
              const prev = measuredRef.current.heights.get(index);
              if (prev !== undefined && Math.abs(prev - height) < 0.5) return;
              measuredRef.current.heights.set(index, height);
              setHeightVersion((version) => version + 1);
            }
          : undefined;

      nodes.push(
        VirtualListRow({
          key,
          top,
          width: rect.width,
          fixedHeight: measuredHeight,
          onMeasure,
          children: props.renderItem(item, index),
        }),
      );
    }
    return nodes;
  }, [
    props.data,
    props.renderItem,
    props.keyExtractor,
    visibleRange.start,
    visibleRange.end,
    heightIndex,
    fixedHeight,
    rect.width,
  ]);

  return ScrollView({
    key: props.key,
    ...extractA11yProps(props),
    accessibilityId,
    accessibilityRole: props.accessibilityRole ?? 'list',
    style: props.style,
    contentStyle: [
      props.contentStyle,
      { width: rect.width > 0 ? rect.width : undefined, height: totalHeight },
    ],
    onScroll: (next) => {
      setOffset(next);
      props.onScroll?.(next);
    },
    children: View({
      style: {
        position: 'relative',
        width: rect.width > 0 ? rect.width : undefined,
        height: totalHeight,
      },
      children,
    }),
  });
}, 'VirtualList');
