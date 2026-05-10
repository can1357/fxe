import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { Component, type Node, useInternalLayout, useMemo, useState } from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { rectFromStyle } from './common.ts';
import { ScrollView } from './ScrollView.ts';
import { View } from './View.ts';

export interface SectionListSection<T> {
  key: string;
  data: readonly T[];
  header?: Node;
}

export interface SectionListProps<T> extends AccessibilityProps {
  key?: string;
  style?: StyleValue;
  contentStyle?: StyleValue;
  sections: readonly SectionListSection<T>[];
  renderItem: (item: T, index: number, sectionIndex: number) => Node;
  renderSectionHeader?: (section: SectionListSection<T>, sectionIndex: number) => Node;
  itemHeight: number;
  sectionHeaderHeight: number;
  keyExtractor?: (item: T, index: number, sectionIndex: number) => string;
  overscan?: number;
  onScroll?: (offset: { x: number; y: number }) => void;
  stickyHeaders?: boolean;
}

export interface SectionListRowLayout {
  kind: 'header' | 'item';
  sectionIndex: number;
  itemIndex: number;
  top: number;
  height: number;
}

export interface SectionListLayout {
  rows: SectionListRowLayout[];
  totalHeight: number;
  sectionStartY: number[];
}

export interface SectionListStickyState {
  activeSectionIndex: number;
  pinnedY: number;
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

function upperBound(rows: readonly SectionListRowLayout[], target: number): number {
  let lo = 0;
  let hi = rows.length;
  while (lo < hi) {
    const mid = Math.floor((lo + hi) / 2);
    if ((rows[mid]?.top ?? 0) <= target) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

export function __sectionListLayout<T>(props: {
  sections: readonly SectionListSection<T>[];
  itemHeight: number;
  sectionHeaderHeight: number;
}): SectionListLayout {
  const itemHeight = validHeight(props.itemHeight, 'itemHeight');
  const sectionHeaderHeight = validHeight(props.sectionHeaderHeight, 'sectionHeaderHeight');
  const rows: SectionListRowLayout[] = [];
  const sectionStartY: number[] = [];
  let y = 0;

  for (let sectionIndex = 0; sectionIndex < props.sections.length; ++sectionIndex) {
    const section = props.sections[sectionIndex];
    if (section === undefined) continue;
    sectionStartY.push(y);
    rows.push({
      kind: 'header',
      sectionIndex,
      itemIndex: -1,
      top: y,
      height: sectionHeaderHeight,
    });
    y += sectionHeaderHeight;
    for (let itemIndex = 0; itemIndex < section.data.length; ++itemIndex) {
      rows.push({
        kind: 'item',
        sectionIndex,
        itemIndex,
        top: y,
        height: itemHeight,
      });
      y += itemHeight;
    }
  }

  return { rows, totalHeight: y, sectionStartY };
}

export function __sectionListStickyState(props: {
  sectionStartY: readonly number[];
  sectionHeaderHeight: number;
  offsetY: number;
}): SectionListStickyState | null {
  const sectionHeaderHeight = validHeight(props.sectionHeaderHeight, 'sectionHeaderHeight');
  const offsetY = Math.max(0, props.offsetY);
  if (props.sectionStartY.length === 0) return null;

  let activeSectionIndex = 0;
  for (let index = 1; index < props.sectionStartY.length; ++index) {
    const startY = props.sectionStartY[index];
    if (startY === undefined || startY > offsetY) break;
    activeSectionIndex = index;
  }

  const nextStartY = props.sectionStartY[activeSectionIndex + 1];
  const pinnedY =
    nextStartY === undefined ? 0 : Math.min(0, nextStartY - sectionHeaderHeight - offsetY);
  return {
    activeSectionIndex,
    pinnedY,
  };
}

function visibleRange(
  rows: readonly SectionListRowLayout[],
  offsetY: number,
  viewportHeight: number,
  overscanPx: number,
): { start: number; end: number } {
  if (rows.length === 0) return { start: 0, end: 0 };
  const minY = Math.max(0, offsetY - overscanPx);
  const maxY = Math.max(minY, offsetY + Math.max(0, viewportHeight) + overscanPx);
  const start = Math.max(0, upperBound(rows, minY) - 1);
  const end = Math.min(rows.length, upperBound(rows, maxY) + 1);
  return { start, end };
}

function sectionHeaderNode<T>(
  section: SectionListSection<T>,
  sectionIndex: number,
  renderSectionHeader: SectionListProps<T>['renderSectionHeader'],
): Node {
  return renderSectionHeader?.(section, sectionIndex) ?? section.header ?? View({});
}

export const SectionList = Component(<T>(props: SectionListProps<T>): Node => {
  const [offset, setOffset] = useState({ x: 0, y: 0 });

  const rect = rectFromStyle(splitStyle(props.style).layout, useInternalLayout() ?? undefined);
  const overscan = cleanCount(props.overscan, 5);
  const layout = useMemo(
    () =>
      __sectionListLayout({
        sections: props.sections,
        itemHeight: props.itemHeight,
        sectionHeaderHeight: props.sectionHeaderHeight,
      }),
    [props.sections, props.itemHeight, props.sectionHeaderHeight],
  );
  const overscanPx = overscan * Math.max(props.itemHeight, props.sectionHeaderHeight);
  const range = visibleRange(layout.rows, offset.y, rect.height, overscanPx);
  const stickyState =
    props.stickyHeaders === false
      ? null
      : __sectionListStickyState({
          sectionStartY: layout.sectionStartY,
          sectionHeaderHeight: props.sectionHeaderHeight,
          offsetY: offset.y,
        });

  const children = useMemo(() => {
    const nodes: Node[] = [];
    const width = rect.width > 0 ? rect.width : undefined;
    for (let rowIndex = range.start; rowIndex < range.end; ++rowIndex) {
      const row = layout.rows[rowIndex];
      if (row === undefined) continue;
      const section = props.sections[row.sectionIndex];
      if (section === undefined) continue;
      if (row.kind === 'header') {
        nodes.push(
          View({
            key: `${section.key}:header`,
            style: {
              position: 'absolute',
              left: 0,
              top: row.top,
              width,
              height: row.height,
            },
            children: sectionHeaderNode(section, row.sectionIndex, props.renderSectionHeader),
          }),
        );
        continue;
      }

      const item = section.data[row.itemIndex];
      if (item === undefined) continue;
      const key =
        props.keyExtractor?.(item, row.itemIndex, row.sectionIndex) ??
        `${row.sectionIndex}-${row.itemIndex}`;
      nodes.push(
        View({
          key,
          style: {
            position: 'absolute',
            left: 0,
            top: row.top,
            width,
            height: row.height,
          },
          children: props.renderItem(item, row.itemIndex, row.sectionIndex),
        }),
      );
    }
    return nodes;
  }, [
    layout.rows,
    props.sections,
    props.renderSectionHeader,
    props.renderItem,
    props.keyExtractor,
    range.start,
    range.end,
    rect.width,
  ]);

  const activeHeader =
    stickyState === null ? undefined : props.sections[stickyState.activeSectionIndex];

  if (props.sections.length === 0) {
    return View({
      key: props.key,
      ...extractA11yProps(props),
      accessibilityRole: props.accessibilityRole ?? 'list',
      style: props.style,
    });
  }

  const rootChildren: Node[] = [
    ScrollView({
      ...extractA11yProps(props),
      accessibilityRole: props.accessibilityRole ?? 'list',
      style: { width: '100%', height: '100%' },
      contentStyle: [
        props.contentStyle,
        { width: rect.width > 0 ? rect.width : undefined, height: layout.totalHeight },
      ],
      onScroll: (next) => {
        setOffset(next);
        props.onScroll?.(next);
      },
      children: View({
        style: {
          position: 'relative',
          width: rect.width > 0 ? rect.width : undefined,
          height: layout.totalHeight,
        },
        children,
      }),
    }),
  ];
  if (stickyState !== null && activeHeader !== undefined) {
    rootChildren.push(
      View({
        style: {
          position: 'absolute',
          top: stickyState.pinnedY,
          left: 0,
          width: rect.width > 0 ? rect.width : undefined,
          height: props.sectionHeaderHeight,
          overflow: 'hidden',
          pointerEvents: 'none',
        },
        children: sectionHeaderNode(
          activeHeader,
          stickyState.activeSectionIndex,
          props.renderSectionHeader,
        ),
      }),
    );
  }
  return View({
    key: props.key,
    style: props.style,
    children: rootChildren,
  });
}, 'SectionList');
