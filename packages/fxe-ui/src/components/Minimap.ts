import { type CommandBuffer, Primitives } from 'fxe';
import { registerHitTarget, type SyntheticEvent } from '../mount/hit_test.ts';
import {
  Component,
  Draw,
  type Node,
  useFrame,
  useId,
  useInternalLayout,
  useMemo,
  useState,
} from '../reconciler/fiber.ts';
import { parseColor, StyleSheet } from '../style/index.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { useTheme } from '../theme/provider.ts';
import { rectFromStyle } from './common.ts';
import type { LineDecorationFn, LineSpan } from './LineViewport.ts';
import { Pressable } from './Pressable.ts';

type NativeTextDocument = InstanceType<typeof TextDocument>;
type PackedColor = number;

export interface MinimapProps {
  key?: string;
  doc: NativeTextDocument;
  width?: number;
  scale?: number;
  lineHeight: number;
  scrollOffset: number;
  viewportHeight: number;
  onScrollRequest?: (offset: number) => void;
  getLineDecorations?: LineDecorationFn;
  style?: StyleValue;
  testID?: string;
}

export interface MinimapViewportRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface MinimapRectBucket {
  color: PackedColor;
  rects: Float32Array;
}

const DEFAULT_SCALE = 0.15;
const DEFAULT_WIDTH = 80;
const MIN_BLOCK_THICKNESS = 1;
const VIEWPORT_ALPHA = 0.22;

const styles = StyleSheet.create({
  root: {
    overflow: 'hidden',
  },
});

const EMPTY_BUCKETS: readonly MinimapRectBucket[] = Object.freeze([]);

export const Minimap = Component((props: MinimapProps): Node => {
  const id = useId();
  const theme = useTheme();
  const scale = props.scale ?? DEFAULT_SCALE;
  const width = props.width ?? DEFAULT_WIDTH;
  const lineHeight = Math.max(1, props.lineHeight);
  const contentHeight = Math.max(lineHeight, props.doc.lineCount() * lineHeight);
  const style: StyleValue = [
    styles.root,
    { width, minWidth: width, maxWidth: width, height: contentHeight * scale },
    props.style,
  ];
  const resolved = splitStyle(style);
  const rect = rectFromStyle(resolved.layout, useInternalLayout() ?? undefined);
  const [revision, setRevision] = useState(props.doc.revision());
  const defaultColor = toPackedColor(parseColor(theme.colors.mutedText, 0xa7afc2ff), 0xa7afc2ff);
  const viewportColor = withAlpha(
    toPackedColor(parseColor(theme.colors.primary, 0x5b8cffff), 0x5b8cffff),
    VIEWPORT_ALPHA,
  );

  useFrame(() => {
    const next = props.doc.revision();
    if (next !== revision) setRevision(next);
  });

  const buckets = useMemo(
    () =>
      __buildMinimapBuckets({
        doc: props.doc,
        scale,
        lineHeight,
        width: rect.width,
        height: rect.height,
        getLineDecorations: props.getLineDecorations,
        defaultColor,
      }),
    [
      props.doc,
      revision,
      scale,
      lineHeight,
      rect.width,
      rect.height,
      props.getLineDecorations,
      defaultColor,
    ],
  );

  if (props.onScrollRequest) {
    const scrollToEvent = (ev: SyntheticEvent): void => {
      props.onScrollRequest?.(
        __minimapHitToOffset(ev.y - rect.y, contentHeight, props.viewportHeight, scale),
      );
    };
    registerHitTarget({
      id,
      rect,
      z: 1_000_000,
      cursor: 'hand',
      componentType: 'Minimap',
      a11y: { accessibilityRole: 'slider' },
      onPressIn: scrollToEvent,
      onDrag: scrollToEvent,
    });
  }

  return Pressable({
    key: props.key,
    style,
    children: Draw(
      (cb: CommandBuffer) => {
        paintMinimapBuckets(cb, rect.x, rect.y, buckets);
        const viewportRect = __computeViewportRect({
          scrollOffset: props.scrollOffset,
          viewportHeight: props.viewportHeight,
          contentHeight: props.doc.lineCount() * lineHeight,
          width: rect.width,
          scale,
        });
        if (viewportRect.width <= 0 || viewportRect.height <= 0) return;
        Primitives.fillRect(
          cb,
          rect.x + viewportRect.x,
          rect.y + viewportRect.y,
          viewportRect.width,
          viewportRect.height,
          0,
          viewportColor,
        );
      },
      [
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        buckets,
        props.scrollOffset,
        props.viewportHeight,
        scale,
        revision,
      ],
    ),
  });
}, 'Minimap');

export function __computeViewportRect(args: {
  scrollOffset: number;
  viewportHeight: number;
  contentHeight: number;
  width: number;
  scale: number;
}): MinimapViewportRect {
  const scale = Math.max(0, args.scale);
  const contentHeight = Math.max(0, args.contentHeight);
  const width = Math.max(0, args.width);
  if (scale <= 0 || contentHeight <= 0 || width <= 0) {
    return { x: 0, y: 0, width: 0, height: 0 };
  }
  const maxScroll = Math.max(0, contentHeight - Math.max(0, args.viewportHeight));
  const scrollOffset = clamp(args.scrollOffset, 0, maxScroll);
  const y = scrollOffset * scale;
  const height = Math.min(
    Math.max(MIN_BLOCK_THICKNESS, Math.max(0, args.viewportHeight) * scale),
    contentHeight * scale - y,
  );
  return { x: 0, y, width, height: Math.max(0, height) };
}

export function __minimapHitToOffset(
  localY: number,
  contentHeight: number,
  viewportHeight: number,
  scale: number,
): number {
  const maxScroll = Math.max(0, contentHeight - Math.max(0, viewportHeight));
  if (maxScroll === 0) return 0;
  const safeScale = Math.max(scale, 0.0001);
  return clamp(localY / safeScale - viewportHeight / 2, 0, maxScroll);
}

export function __buildMinimapBuckets(args: {
  doc: NativeTextDocument;
  scale: number;
  lineHeight: number;
  width: number;
  height: number;
  getLineDecorations?: LineDecorationFn;
  defaultColor: PackedColor;
}): readonly MinimapRectBucket[] {
  const scale = Math.max(0, args.scale);
  const lineHeight = Math.max(1, args.lineHeight);
  const width = Math.max(0, args.width);
  const height = Math.max(0, args.height);
  if (scale <= 0 || width <= 0 || height <= 0) return EMPTY_BUCKETS;

  const bucketData = new Map<PackedColor, number[]>();
  const rectHeight = Math.max(MIN_BLOCK_THICKNESS, lineHeight * scale);
  const charWidth = Math.max(MIN_BLOCK_THICKNESS, lineHeight * 0.6 * scale);

  for (let line = 0; line < args.doc.lineCount(); ++line) {
    const y = line * lineHeight * scale;
    if (y >= height) break;
    const text = args.doc.lineText(line);
    if (text.length === 0) continue;
    const spans = resolveLineSpans(text, args.getLineDecorations?.(line)?.spans, args.defaultColor);
    for (const span of spans) {
      const x = span.start * charWidth;
      if (x >= width) break;
      const w = Math.min(
        width - x,
        Math.max(MIN_BLOCK_THICKNESS, (span.end - span.start) * charWidth),
      );
      if (w <= 0) continue;
      const bucket = bucketData.get(span.color);
      if (bucket) {
        bucket.push(x, y, w, rectHeight);
      } else {
        bucketData.set(span.color, [x, y, w, rectHeight]);
      }
    }
  }

  if (bucketData.size === 0) return EMPTY_BUCKETS;
  return Array.from(bucketData, ([color, values]) => ({ color, rects: new Float32Array(values) }));
}

function paintMinimapBuckets(
  cb: CommandBuffer,
  originX: number,
  originY: number,
  buckets: readonly MinimapRectBucket[],
): void {
  for (const bucket of buckets) {
    const rects = new Float32Array(bucket.rects.length);
    for (let i = 0; i < bucket.rects.length; i += 4) {
      rects[i] = originX + bucket.rects[i];
      rects[i + 1] = originY + bucket.rects[i + 1];
      rects[i + 2] = bucket.rects[i + 2];
      rects[i + 3] = bucket.rects[i + 3];
    }
    Primitives.drawSelectionRects(cb, rects, bucket.color);
  }
}

function resolveLineSpans(
  text: string,
  spans: ReadonlyArray<LineSpan> | undefined,
  defaultColor: PackedColor,
): Array<{ start: number; end: number; color: PackedColor }> {
  const out: Array<{ start: number; end: number; color: PackedColor }> = [];
  if (!spans || spans.length === 0) {
    appendNonWhitespaceRuns(out, text, 0, text.length, defaultColor);
    return out;
  }

  let cursor = 0;
  for (const span of spans) {
    const start = clamp(Math.floor(span.start), 0, text.length);
    const end = clamp(Math.floor(span.end), 0, text.length);
    if (start > cursor) appendNonWhitespaceRuns(out, text, cursor, start, defaultColor);
    if (end > start) {
      appendNonWhitespaceRuns(out, text, start, end, toPackedColor(span.color, defaultColor));
    }
    cursor = Math.max(cursor, end);
  }
  if (cursor < text.length) appendNonWhitespaceRuns(out, text, cursor, text.length, defaultColor);
  return out;
}

function appendNonWhitespaceRuns(
  out: Array<{ start: number; end: number; color: PackedColor }>,
  text: string,
  start: number,
  end: number,
  color: PackedColor,
): void {
  let runStart = -1;
  for (let index = start; index < end; ++index) {
    if (!isWhitespace(text.charCodeAt(index))) {
      if (runStart === -1) runStart = index;
      continue;
    }
    if (runStart !== -1) {
      out.push({ start: runStart, end: index, color });
      runStart = -1;
    }
  }
  if (runStart !== -1) out.push({ start: runStart, end, color });
}

function isWhitespace(code: number): boolean {
  return code === 32 || code === 9 || code === 10 || code === 13;
}

function toPackedColor(
  color: number | readonly [number, number, number, number] | undefined,
  fallback: number,
): number {
  if (typeof color === 'number') return color >>> 0;
  if (Array.isArray(color)) {
    return (
      (((Math.round(color[0]) & 0xff) << 24) |
        ((Math.round(color[1]) & 0xff) << 16) |
        ((Math.round(color[2]) & 0xff) << 8) |
        (Math.round(color[3]) & 0xff)) >>>
      0
    );
  }
  return fallback >>> 0;
}

function withAlpha(color: PackedColor, alpha: number): PackedColor {
  return (Math.floor(color / 256) * 256 + Math.round(clamp(alpha, 0, 1) * 255)) >>> 0;
}

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value));
}
