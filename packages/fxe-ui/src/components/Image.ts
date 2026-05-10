import {
  type CommandBuffer,
  type ImageHandle,
  Image as NativeImage,
  Primitives,
  Spritesheet,
} from 'fxe';
import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { AnimatedValue, type CompositeAnimation, Easings, timing } from '../animated/index.ts';
import { registerHitTarget } from '../mount/hit_test.ts';
import {
  Component,
  Draw,
  type Node,
  requestRenderTargetRedraw,
  useEffect,
  useId,
  useInternalLayout,
  useMemo,
  useRef,
  useState,
} from '../reconciler/fiber.ts';
import { StyleSheet } from '../style/index.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue } from '../style/types.ts';
import { rectFromStyle } from './common.ts';
import { View, type ViewProps } from './View.ts';

export type ImageSource = string | ImageHandle;
export type ImagePlaceholder = 'color' | { color: number } | ImageHandle;
export type ImageResizeMode = 'cover' | 'contain' | 'stretch' | 'center';

export interface ImageProps extends AccessibilityProps {
  key?: string;
  style?: StyleValue;
  source?: ImageSource;
  placeholder?: ImagePlaceholder;
  fadeInMs?: number;
  resizeMode?: ImageResizeMode;
  width?: number;
  height?: number;
  tint?: number;
  onLoad?: (width: number, height: number) => void;
  onError?: (err: Error) => void;
}

export interface ImageContentRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

type ImagePhase = 'idle' | 'loading' | 'loaded' | 'error';

type ImageNamespaceWithMipHint = typeof NativeImage & {
  generateMipmaps?: (image: ImageHandle) => void;
  generateMips?: (image: ImageHandle) => void;
  hintGenerateMipmaps?: (image: ImageHandle) => void;
};

type PrimitivesWithDrawSprite = typeof Primitives & {
  drawSprite?: (
    cb: CommandBuffer,
    spriteId: number,
    x: number,
    y: number,
    width: number,
    height: number,
    depth?: number,
    tint?: number,
  ) => void;
};

const DEFAULT_FADE_IN_MS = 180;
export const DEFAULT_IMAGE_PLACEHOLDER_COLOR = 0xe5e7ebff;
const DEFAULT_TINT = 0xffffffff;
const styles = StyleSheet.create({
  root: {
    overflow: 'hidden',
  },
});
const g_imageSpritesheet = new Spritesheet();
const g_imageSpriteIds = new WeakMap<ImageHandle, number>();

export function resolveImagePlaceholderColor(
  placeholder: ImagePlaceholder | undefined,
): number | undefined {
  if (placeholder === undefined || placeholder === 'color') return DEFAULT_IMAGE_PLACEHOLDER_COLOR;
  if (isImageHandle(placeholder)) return undefined;
  return placeholder.color;
}

export function normalizeImageFadeInMs(fadeInMs: number | undefined): number {
  return Number.isFinite(fadeInMs) ? Math.max(0, Number(fadeInMs)) : DEFAULT_FADE_IN_MS;
}

export function startImageFadeAnimation(
  value: AnimatedValue<number>,
  fadeInMs: number | undefined,
): CompositeAnimation {
  const animation = timing(value, {
    to: 1,
    duration: normalizeImageFadeInMs(fadeInMs),
    easing: Easings.easeOut,
  });
  animation.start();
  return animation;
}

export function resolveImageContentRect(
  box: ImageContentRect,
  intrinsicWidth: number,
  intrinsicHeight: number,
  resizeMode: ImageResizeMode = 'cover',
): ImageContentRect {
  if (box.width <= 0 || box.height <= 0 || intrinsicWidth <= 0 || intrinsicHeight <= 0) {
    return { x: box.x, y: box.y, width: Math.max(0, box.width), height: Math.max(0, box.height) };
  }
  if (resizeMode === 'stretch') return { ...box };
  if (resizeMode === 'center') {
    return {
      x: box.x + (box.width - intrinsicWidth) * 0.5,
      y: box.y + (box.height - intrinsicHeight) * 0.5,
      width: intrinsicWidth,
      height: intrinsicHeight,
    };
  }
  const scale =
    resizeMode === 'contain'
      ? Math.min(box.width / intrinsicWidth, box.height / intrinsicHeight)
      : Math.max(box.width / intrinsicWidth, box.height / intrinsicHeight);
  const width = intrinsicWidth * scale;
  const height = intrinsicHeight * scale;
  return {
    x: box.x + (box.width - width) * 0.5,
    y: box.y + (box.height - height) * 0.5,
    width,
    height,
  };
}

export function hintImageMipGeneration(image: ImageHandle): boolean {
  const api = NativeImage as ImageNamespaceWithMipHint;
  const hint = api.generateMipmaps ?? api.generateMips ?? api.hintGenerateMipmaps;
  if (typeof hint !== 'function') {
    // TODO(fxe-ui): call the image binding's stable mip-generation hint once it is exported.
    return false;
  }
  hint.call(api, image);
  return true;
}

function isImageHandle(value: unknown): value is ImageHandle {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as ImageHandle).width === 'function' &&
    typeof (value as ImageHandle).height === 'function' &&
    typeof (value as ImageHandle).dispose === 'function' &&
    typeof (value as ImageHandle).bytes === 'function'
  );
}

function asError(error: unknown): Error {
  return error instanceof Error ? error : new Error(String(error));
}

function clamp01(value: number): number {
  if (value <= 0) return 0;
  if (value >= 1) return 1;
  return value;
}

function applyOpacity(color: number, opacity: number): number {
  const alpha = Math.round((color & 0xff) * clamp01(opacity));
  return ((color & 0xffffff00) | alpha) >>> 0;
}

function spriteIdForImage(image: ImageHandle): number {
  let spriteId = g_imageSpriteIds.get(image);
  if (spriteId !== undefined) return spriteId;
  spriteId = g_imageSpritesheet.add(image);
  g_imageSpriteIds.set(image, spriteId);
  return spriteId;
}

function paintHandle(
  cb: CommandBuffer,
  image: ImageHandle,
  rect: ImageContentRect,
  tint: number,
  opacity: number,
): void {
  if (rect.width <= 0 || rect.height <= 0 || opacity <= 0) return;
  const primitives = Primitives as PrimitivesWithDrawSprite;
  const color = applyOpacity(tint, opacity);
  if (typeof primitives.drawSprite === 'function') {
    primitives.drawSprite(
      cb,
      spriteIdForImage(image),
      rect.x,
      rect.y,
      rect.width,
      rect.height,
      0,
      color,
    );
    return;
  }
  // TODO(multi-texture): replace this rect fallback with the textured sprite path in every render context.
  Primitives.fillRect(cb, rect.x, rect.y, rect.width, rect.height, 0, color);
}

export const Image = Component((props: ImageProps): Node => {
  const id = useId();
  const inheritedLayout = useInternalLayout() ?? undefined;
  const [asyncHandle, setAsyncHandle] = useState<ImageHandle | null>(null);
  const [asyncPhase, setAsyncPhase] = useState<ImagePhase>(
    typeof props.source === 'string' ? 'loading' : 'idle',
  );
  const [error, setError] = useState<Error | null>(null);
  const ownedHandleRef = useRef<ImageHandle | null>(null);
  const fadeValueRef = useRef<AnimatedValue<number> | null>(null);
  const lastLoadedRef = useRef<ImageHandle | null>(null);
  const lastErrorRef = useRef<Error | null>(null);
  if (fadeValueRef.current === null) {
    fadeValueRef.current = new AnimatedValue(isImageHandle(props.source) ? 1 : 0);
  }
  const fadeValue = fadeValueRef.current;
  const directHandle = isImageHandle(props.source) ? props.source : null;
  const placeholderHandle = isImageHandle(props.placeholder) ? props.placeholder : null;
  const displayHandle = directHandle ?? asyncHandle;
  const phase: ImagePhase = directHandle !== null ? 'loaded' : asyncPhase;
  const resizeMode = props.resizeMode ?? 'cover';
  const resolved = splitStyle([{ width: props.width, height: props.height }, props.style]);
  if (props.tint !== undefined) resolved.paint.tint = props.tint;
  const rect = rectFromStyle(resolved.layout, inheritedLayout);
  const intrinsicHandle = displayHandle ?? placeholderHandle;
  if (inheritedLayout === undefined && intrinsicHandle !== null) {
    if (typeof resolved.layout.width !== 'number') rect.width = intrinsicHandle.width();
    if (typeof resolved.layout.height !== 'number') rect.height = intrinsicHandle.height();
  }
  const baseOpacity = clamp01(
    typeof resolved.paint.opacity === 'number' ? resolved.paint.opacity : 1,
  );
  const placeholderColor = resolveImagePlaceholderColor(props.placeholder);
  const placeholderStyle = useMemo(
    () =>
      placeholderColor === undefined
        ? undefined
        : {
            backgroundColor: applyOpacity(placeholderColor, baseOpacity),
          },
    [placeholderColor, baseOpacity],
  );
  const a11y = extractA11yProps(props);
  registerHitTarget({
    id,
    rect,
    cursor: resolved.paint.cursor,
    a11y: { ...a11y, accessibilityRole: props.accessibilityRole ?? 'image' },
    componentType: 'Image',
    tabIndex: a11y.tabIndex,
  });

  useEffect(() => {
    const unsubscribe = fadeValue.addListener(() => requestRenderTargetRedraw());
    return () => {
      unsubscribe();
    };
  }, [fadeValue]);

  useEffect(() => {
    return () => {
      ownedHandleRef.current?.dispose();
      ownedHandleRef.current = null;
      fadeValue._dispose();
    };
  }, [fadeValue]);

  useEffect(() => {
    if (directHandle !== null || typeof props.source !== 'string') {
      ownedHandleRef.current?.dispose();
      ownedHandleRef.current = null;
      setAsyncHandle(null);
      setAsyncPhase('idle');
      setError(null);
      fadeValue.setValue(directHandle !== null ? 1 : 0);
      return;
    }
    let cancelled = false;
    setAsyncPhase('loading');
    setAsyncHandle(null);
    setError(null);
    fadeValue.setValue(0);
    void NativeImage.loadAsync(props.source)
      .then((handle) => {
        if (cancelled) {
          handle.dispose();
          return;
        }
        hintImageMipGeneration(handle);
        ownedHandleRef.current?.dispose();
        ownedHandleRef.current = handle;
        setAsyncHandle(handle);
        setAsyncPhase('loaded');
      })
      .catch((caught) => {
        if (cancelled) return;
        setAsyncHandle(null);
        setAsyncPhase('error');
        setError(asError(caught));
      });
    return () => {
      cancelled = true;
    };
  }, [directHandle, props.source, fadeValue]);

  useEffect(() => {
    if (typeof props.source !== 'string' || asyncPhase !== 'loaded' || asyncHandle === null) return;
    const animation = startImageFadeAnimation(fadeValue, props.fadeInMs);
    return () => {
      animation.stop();
    };
  }, [asyncHandle, asyncPhase, fadeValue, props.fadeInMs, props.source]);

  useEffect(() => {
    if (phase !== 'loaded' || displayHandle === null) return;
    if (lastLoadedRef.current === displayHandle) return;
    lastLoadedRef.current = displayHandle;
    props.onLoad?.(displayHandle.width(), displayHandle.height());
  }, [displayHandle, phase, props.onLoad]);

  useEffect(() => {
    if (error === null || lastErrorRef.current === error) return;
    lastErrorRef.current = error;
    props.onError?.(error);
  }, [error, props.onError]);

  const drawNode = Draw(
    (cb: CommandBuffer) => {
      const tint = typeof resolved.paint.tint === 'number' ? resolved.paint.tint : DEFAULT_TINT;
      const shouldPaintPlaceholderHandle =
        placeholderHandle !== null &&
        (phase !== 'loaded' || (directHandle === null && fadeValue.getValue() < 1));
      if (shouldPaintPlaceholderHandle && placeholderHandle !== null) {
        const placeholderRect = resolveImageContentRect(
          rect,
          placeholderHandle.width(),
          placeholderHandle.height(),
          resizeMode,
        );
        paintHandle(cb, placeholderHandle, placeholderRect, tint, baseOpacity);
      }
      if (displayHandle !== null) {
        const imageRect = resolveImageContentRect(
          rect,
          displayHandle.width(),
          displayHandle.height(),
          resizeMode,
        );
        const imageOpacity =
          directHandle !== null ? baseOpacity : baseOpacity * fadeValue.getValue();
        paintHandle(cb, displayHandle, imageRect, tint, imageOpacity);
      }
    },
    [
      props.source,
      props.placeholder,
      props.style,
      props.width,
      props.height,
      props.resizeMode,
      props.tint,
      rect.x,
      rect.y,
      rect.width,
      rect.height,
      phase,
      displayHandle,
      placeholderHandle,
    ],
  );

  const view = View({
    key: props.key,
    style: [props.style, placeholderStyle, styles.root],
    children: drawNode,
    __skipA11yHitTarget: true,
  } as ViewProps & { __skipA11yHitTarget: true });
  return view.type === 'component' ? { ...view, internalLayout: rect } : view;
}, 'Image');
