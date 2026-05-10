import type { Database, SQLBindings } from 'fxe:sqlite';
import type {
  CommandBuffer,
  CursorKind,
  Color as FxeRuntimeColor,
  ImageHandle,
  Mat4,
  Renderer,
  Vec4,
  Window,
  WindowEventMap,
  WindowEventName,
} from 'fxe';

type FxeUiLength = number | `${number}%` | 'auto';
type FxeUiColor = number | readonly [number, number, number, number] | `#${string}`;

declare module 'fxe-ui' {
  export type Length = FxeUiLength;
  export type Color = FxeUiColor;
  export type RuntimeColor = FxeRuntimeColor;

  export type AccessibilityRole =
    | 'none'
    | 'group'
    | 'text'
    | 'button'
    | 'link'
    | 'image'
    | 'textbox'
    | 'searchbox'
    | 'checkbox'
    | 'switch'
    | 'radio'
    | 'slider'
    | 'progressbar'
    | 'heading'
    | 'list'
    | 'listitem'
    | 'scrollview'
    | 'dialog'
    | 'alert'
    | 'status'
    | 'tab'
    | 'tablist'
    | 'menu'
    | 'menuitem';
  export type AccessibilityLiveRegion = 'off' | 'polite' | 'assertive';
  export type AccessibilityCheckedState = boolean | 'mixed';
  export interface AccessibilityState {
    disabled?: boolean;
    selected?: boolean;
    checked?: AccessibilityCheckedState;
    expanded?: boolean;
    busy?: boolean;
    required?: boolean;
    invalid?: boolean | 'grammar' | 'spelling';
    readOnly?: boolean;
    pressed?: boolean;
    // Virtual-list specific flag for synthesised offscreen descendants.
    offscreen?: boolean;
  }
  export interface AccessibilityValue {
    text?: string;
    now?: number;
    min?: number;
    max?: number;
  }
  export interface AccessibilityProps {
    accessible?: boolean;
    accessibilityRole?: AccessibilityRole;
    accessibilityLabel?: string;
    accessibilityHint?: string;
    accessibilityState?: AccessibilityState;
    accessibilityValue?: string | number | AccessibilityValue;
    accessibilityLiveRegion?: AccessibilityLiveRegion;
    accessibilityLanguage?: string;
    accessibilityHeadingLevel?: 1 | 2 | 3 | 4 | 5 | 6;
    accessibilityId?: string;
    tabIndex?: number;
    focusable?: boolean;
    dir?: 'ltr' | 'rtl' | 'auto';
  }
  export interface AccessibilityRect {
    x: number;
    y: number;
    width: number;
    height: number;
  }
  export interface AccessibilityNodeSnapshot {
    id: string;
    parentId: string | null;
    role: AccessibilityRole;
    label: string;
    hint?: string;
    value?: AccessibilityValue;
    state: AccessibilityState;
    rect: AccessibilityRect;
    focusable: boolean;
    tabIndex?: number;
    liveRegion: AccessibilityLiveRegion;
    language?: string;
    headingLevel?: number;
    children: AccessibilityNodeSnapshot[];
  }
  export interface AccessibilityTreeSnapshot {
    rootId: string;
    generation: number;
    focusedId: string | null;
    nodesById: Record<string, AccessibilityNodeSnapshot>;
    childrenById: Record<string, string[]>;
  }
  export interface AccessibilityFiberLike {
    id: string;
    componentType: string;
    a11y: AccessibilityProps;
    rect: { x: number; y: number; width: number; height: number };
    children: AccessibilityFiberLike[];
    fallbackLabel?: string;
  }
  export interface BuildTreeOptions {
    generation: number;
    focusedId?: string | null;
  }
  export function buildAccessibilityTree(
    root: AccessibilityFiberLike,
    options: BuildTreeOptions,
  ): AccessibilityTreeSnapshot;

  export interface A11yBridge {
    publish(snapshot: AccessibilityTreeSnapshot): void;
    latest(): AccessibilityTreeSnapshot | null;
    clear(): void;
    subscribe(cb: (s: AccessibilityTreeSnapshot) => void): () => void;
  }
  export function getA11yBridge(): A11yBridge;
  export function publishAccessibilityTree(snapshot: AccessibilityTreeSnapshot): void;

  export interface VirtualDescendantSource {
    parentId: string;
    totalCount: number;
    renderedRange: [number, number];
    buildVirtualNode(index: number): AccessibilityNodeSnapshot | null;
  }
  export function registerVirtualSource(source: VirtualDescendantSource): () => void;
  export function getVirtualSources(): ReadonlyMap<string, VirtualDescendantSource>;
  export function expandVirtualDescendants(
    node: AccessibilityNodeSnapshot,
    source: VirtualDescendantSource,
  ): AccessibilityNodeSnapshot;

  export interface LayoutTraceEntry {
    component: string;
    rect: LayoutResult;
    hasParentLayout: boolean;
    styleWidth: Style['width'];
    styleHeight: Style['height'];
    tag?: string;
  }
  export function setLayoutTraceEnabled(on: boolean, opts?: { limit?: number }): void;
  export function drainLayoutTrace(clear?: boolean): LayoutTraceEntry[];

  export interface Constraint {
    width?: number;
    height?: number;
  }

  export interface LayoutStyle {
    display?: 'flex' | 'none';
    width?: Length;
    height?: Length;
    minWidth?: Length;
    minHeight?: Length;
    maxWidth?: Length;
    maxHeight?: Length;
    padding?: Length;
    paddingX?: Length;
    paddingY?: Length;
    paddingTop?: Length;
    paddingRight?: Length;
    paddingBottom?: Length;
    paddingLeft?: Length;
    margin?: Length;
    marginX?: Length;
    marginY?: Length;
    marginTop?: Length;
    marginRight?: Length;
    marginBottom?: Length;
    marginLeft?: Length;
    flexDirection?: 'row' | 'column' | 'row-reverse' | 'column-reverse';
    flexWrap?: 'nowrap' | 'wrap' | 'wrap-reverse';
    justifyContent?:
      | 'flex-start'
      | 'flex-end'
      | 'center'
      | 'space-between'
      | 'space-around'
      | 'space-evenly';
    alignItems?: 'auto' | 'flex-start' | 'flex-end' | 'center' | 'stretch' | 'baseline';
    alignSelf?: 'auto' | 'flex-start' | 'flex-end' | 'center' | 'stretch' | 'baseline';
    alignContent?:
      | 'auto'
      | 'flex-start'
      | 'flex-end'
      | 'center'
      | 'stretch'
      | 'baseline'
      | 'space-between'
      | 'space-around'
      | 'space-evenly';
    flex?: number;
    flexGrow?: number;
    flexShrink?: number;
    flexBasis?: Length;
    gap?: number;
    rowGap?: number;
    columnGap?: number;
    position?: 'relative' | 'absolute';
    top?: Length;
    right?: Length;
    bottom?: Length;
    left?: Length;
    aspectRatio?: number;
    overflow?: 'visible' | 'hidden' | 'scroll';
  }

  export interface Style extends LayoutStyle {
    backgroundColor?: Color;
    opacity?: number;
    tint?: Color;
    borderWidth?: number;
    borderColor?: Color;
    // Per-side borders (override borderWidth/borderColor when set)
    borderTopWidth?: number;
    borderRightWidth?: number;
    borderBottomWidth?: number;
    borderLeftWidth?: number;
    borderTopColor?: Color;
    borderRightColor?: Color;
    borderBottomColor?: Color;
    borderLeftColor?: Color;
    borderStyle?: 'solid' | 'dashed' | 'dotted' | 'none';
    // Box shadow
    shadowColor?: Color;
    shadowOffsetX?: number;
    shadowOffsetY?: number;
    shadowBlur?: number;
    shadowSpread?: number;
    borderRadius?: number;
    borderTopLeftRadius?: number;
    borderTopRightRadius?: number;
    borderBottomLeftRadius?: number;
    borderBottomRightRadius?: number;
    color?: Color;
    fontSize?: number;
    fontFamily?: string;
    fontWeight?: number;
    lineHeight?: number;
    textAlign?: 'left' | 'center' | 'right';
    letterSpacing?: number;
    cursor?: CursorKind;
    pointerEvents?: 'auto' | 'none';
  }

  export type StyleValue = Style | readonly StyleValue[] | null | undefined | false;

  export interface PaintStyle {
    backgroundColor?: RuntimeColor;
    opacity?: number;
    tint?: RuntimeColor;
    borderWidth?: number;
    borderColor?: RuntimeColor;
    // Per-side borders (override borderWidth/borderColor when set)
    borderTopWidth?: number;
    borderRightWidth?: number;
    borderBottomWidth?: number;
    borderLeftWidth?: number;
    borderTopColor?: RuntimeColor;
    borderRightColor?: RuntimeColor;
    borderBottomColor?: RuntimeColor;
    borderLeftColor?: RuntimeColor;
    borderStyle?: 'solid' | 'dashed' | 'dotted' | 'none';
    // Box shadow
    shadowColor?: RuntimeColor;
    shadowOffsetX?: number;
    shadowOffsetY?: number;
    shadowBlur?: number;
    shadowSpread?: number;
    borderRadius?: number;
    cursor?: CursorKind;
    pointerEvents?: 'auto' | 'none';
  }

  export interface TextStyle {
    color?: RuntimeColor;
    fontSize?: number;
    fontFamily?: string;
    fontWeight?: number;
    lineHeight?: number;
    textAlign?: 'left' | 'center' | 'right';
    letterSpacing?: number;
  }

  export interface LayoutNode {
    style?: LayoutStyle;
    children?: readonly LayoutNode[];
    measure?: (constraint: Constraint) => { width: number; height: number };
    key?: string;
  }

  export interface LayoutResult {
    x: number;
    y: number;
    width: number;
    height: number;
    paddingLeft: number;
    paddingTop: number;
    paddingRight: number;
    paddingBottom: number;
    children: LayoutResult[];
  }

  export function layout(root: LayoutNode, available?: Constraint): LayoutResult;
  export function measureText(
    text: string,
    fontSize?: number,
  ): (constraint: Constraint) => { width: number; height: number };
  export function measureImage(
    width: number,
    height: number,
  ): (constraint: Constraint) => { width: number; height: number };

  export interface WrappedText {
    lines: string[];
    width: number;
    height: number;
    lineHeight: number;
    lineStartIndices: number[];
  }
  export interface WrapOptions {
    maxWidth?: number;
    breakWords?: boolean;
  }
  export function wrapText(text: string, style: TextStyle, options?: WrapOptions): WrappedText;
  export function glyphIndexAt(text: string, style: TextStyle, x: number): number;
  export function xAtGlyphIndex(text: string, style: TextStyle, idx: number): number;
  export function parseColor(
    value: Color | undefined,
    fallback?: RuntimeColor,
  ): RuntimeColor | undefined;

  export type SvgAffine = readonly [number, number, number, number, number, number];
  export interface SvgGradientStop {
    offset: number;
    color: number;
  }
  export type SvgPaint =
    | { kind: 'solid'; color: number }
    | {
        kind: 'linear-gradient';
        x1: number;
        y1: number;
        x2: number;
        y2: number;
        stops: SvgGradientStop[];
        spread: 'pad' | 'reflect' | 'repeat';
        gradientUnits: 'userSpaceOnUse' | 'objectBoundingBox';
        transform: SvgAffine;
      }
    | {
        kind: 'radial-gradient';
        cx: number;
        cy: number;
        r: number;
        fx: number;
        fy: number;
        stops: SvgGradientStop[];
        spread: 'pad' | 'reflect' | 'repeat';
        gradientUnits: 'userSpaceOnUse' | 'objectBoundingBox';
        transform: SvgAffine;
      };
  export interface SvgShape {
    path: import('fxe').Path;
    fill?: number | SvgPaint;
    stroke?: number | SvgPaint;
    strokeWidth?: number;
    fillRule?: 'nonzero' | 'evenodd';
  }

  export interface SvgDocument {
    viewBox: [number, number, number, number];
    width: number;
    height: number;
    shapes: SvgShape[];
  }

  export function parseSvg(source: string): SvgDocument;
  export function flattenStyle(value: StyleValue): Style;
  export function splitStyle(value: StyleValue): {
    layout: LayoutStyle;
    paint: PaintStyle;
    text: TextStyle;
  };
  export namespace StyleSheet {
    function create<T extends Record<string, Style>>(
      styles: T,
    ): { readonly [K in keyof T]: Readonly<T[K]> };
  }

  export const STYLE_SHEET_BRAND: unique symbol;

  export interface LayerProps {
    key?: string;
    transform?: Mat4;
    tint?: Vec4;
    deps?: ReadonlyArray<unknown>;
    children: readonly Node[];
  }

  export interface DrawProps {
    fn: (cb: CommandBuffer) => void;
    deps?: ReadonlyArray<unknown>;
  }

  export type PropsEqual<P> = (prev: Readonly<P>, next: Readonly<P>) => boolean;
  export type BoundaryChild = Node | readonly BoundaryChild[] | null | undefined | boolean;
  export type TextChild =
    | Node
    | string
    | number
    | readonly TextChild[]
    | null
    | undefined
    | boolean;

  export interface Context<T> {
    readonly defaultValue: T;
    readonly Provider: (props: { key?: string; value: T; children?: BoundaryChild }) => Node;
  }

  export interface PortalProps {
    key?: string;
    to: CommandBuffer | Renderer;
    children?: BoundaryChild;
  }

  export interface ErrorBoundaryProps {
    key?: string;
    children?: BoundaryChild;
    fallback?: BoundaryChild | ((error: unknown) => BoundaryChild);
    onError?: (error: unknown) => void;
  }

  export interface SuspenseProps {
    key?: string;
    children?: BoundaryChild;
    fallback?: BoundaryChild;
  }

  interface ProviderNodeProps {
    key?: string;
    ctx: Context<unknown>;
    value: unknown;
    children?: BoundaryChild;
  }

  export type Node =
    | { type: 'layer'; props: LayerProps; key?: string }
    | { type: 'draw'; props: DrawProps; key?: string }
    | {
        type: 'component';
        componentType?: object;
        render: (props: unknown) => Node;
        props: unknown;
        displayName?: string;
        key?: string;
        memo?: { areEqual: (prev: unknown, next: unknown) => boolean };
        internalLayout?: LayoutResult;
        internalTextStyle?: TextStyle;
      }
    | { type: 'provider'; props: ProviderNodeProps; key?: string }
    | { type: 'portal'; props: PortalProps; key?: string }
    | { type: 'error-boundary'; props: ErrorBoundaryProps; key?: string }
    | { type: 'suspense'; props: SuspenseProps; key?: string };

  export type FiberCacheHitMiss = 'hit' | 'miss' | null;
  export interface FiberNode {
    id: number;
    type: string;
    displayName: string | null;
    key: string;
    props: string;
    propsSummary: string;
    dirty: boolean;
    lastRebuildFrame: number;
    deps: unknown[][];
    cacheHit: boolean | null;
    cacheHitMiss: FiberCacheHitMiss;
    children: FiberNode[];
  }
  export type DevtoolsFiberCacheHit = boolean | null;
  export interface DevtoolsFiberNode extends FiberNode {}
  export interface DevtoolsFiberTreeSnapshot {
    tree: DevtoolsFiberNode[];
  }
  export function reconcilerSnapshot(): { tree: FiberNode[] };
  export function snapshotFiberTree(): DevtoolsFiberTreeSnapshot;
  export function setPaintFlash(enabled: boolean): void;

  export function Layer(props: LayerProps): Node;
  export function Draw(fn: (cb: CommandBuffer) => void, deps?: ReadonlyArray<unknown>): Node;
  export function Component<P>(
    render: (props: P) => Node,
    displayName?: string,
  ): (props: P & { key?: string }) => Node;
  export function memo<P>(
    component: (props: P & { key?: string }) => Node,
    areEqual?: PropsEqual<P & { key?: string }>,
  ): (props: P & { key?: string }) => Node;
  export function ErrorBoundary(props: ErrorBoundaryProps): Node;
  export function Suspense(props: SuspenseProps): Node;
  export function Portal(props: PortalProps): Node;
  export function createContext<T>(defaultValue: T): Context<T>;

  export function useState<S>(initial: S): [S, (next: S | ((s: S) => S)) => void];
  export function useReducer<S, A>(
    reducer: (state: S, action: A) => S,
    initial: S,
  ): [S, (action: A) => void];
  export function useReducer<S, A, I>(
    reducer: (state: S, action: A) => S,
    initial: I,
    init: (initial: I) => S,
  ): [S, (action: A) => void];
  export function useRef<T>(initial: T): { current: T };
  export function useId(): string;
  export function useContext<T>(context: Context<T>): T;
  export function useInternalLayout(): LayoutResult | null;
  export function useInternalTextStyle(): TextStyle | null;
  export function useMemo<T>(fn: () => T, deps: ReadonlyArray<unknown>): T;
  export function useEffect(
    fn: () => undefined | (() => void),
    deps?: ReadonlyArray<unknown>,
  ): void;
  export function useFrame(fn: (dtMs: number) => void): void;
  export function useEvent<K extends WindowEventName>(
    win: Window,
    kind: K,
    handler: (ev: WindowEventMap[K]) => void,
  ): void;
  export function useTransition(): [boolean, (fn: () => void) => void];
  export function useDeferredValue<T>(value: T): T;

  export function useSyncExternalStore<T>(
    subscribe: (cb: () => void) => () => void,
    getSnapshot: () => T,
  ): T;
  export interface FetchResult<T> {
    data: T | undefined;
    error: unknown;
    loading: boolean;
  }
  export interface WebSocketStore {
    send: (data: string | ArrayBuffer | ArrayBufferView) => void;
    lastMessage: unknown;
    readyState: number;
  }
  export function useSqliteQuery<T>(db: Database, sql: string, params?: SQLBindings): T[];
  export function useFetch<T>(url: string, init?: RequestInit): FetchResult<T>;
  export function useWebSocket(url: string): WebSocketStore;
  export type AnimatedOutput = number | string;

  export type AnimatedListener<T extends AnimatedOutput> = (value: T) => void;
  export type ExtrapolateMode = 'extend' | 'clamp' | 'identity';
  export interface InterpolationConfig<T extends AnimatedOutput = AnimatedOutput> {
    inputRange: readonly number[];
    outputRange: readonly T[];
    extrapolate?: ExtrapolateMode;
  }
  export class AnimatedValue<T extends AnimatedOutput = number> {
    current: T;
    constructor(initial: AnimatedOutput);
    setValue(value: T): void;
    getValue(): T;
    addListener(fn: AnimatedListener<T>): () => void;
    interpolate<U extends AnimatedOutput>(config: InterpolationConfig<U>): AnimatedValue<U>;
  }
  export type EasingName = 'linear' | 'ease' | 'ease-in' | 'ease-out' | 'ease-in-out';
  export type EasingFunction = (t: number) => number;
  export type Easing = EasingName | EasingFunction;
  export const Easings: {
    readonly linear: EasingFunction;
    readonly ease: EasingFunction;
    readonly easeIn: EasingFunction;
    readonly easeOut: EasingFunction;
    readonly easeInOut: EasingFunction;
    readonly caEaseIn: EasingFunction;
    readonly caEaseOut: EasingFunction;
    readonly caEaseInEaseOut: EasingFunction;
    readonly caDefault: EasingFunction;
    readonly materialStandard: EasingFunction;
    readonly materialDecelerate: EasingFunction;
    readonly materialAccelerate: EasingFunction;
  };
  export function cubicBezier(x1: number, y1: number, x2: number, y2: number): EasingFunction;
  export interface TimingAnimationConfig {
    to: number;
    duration: number;
    easing?: Easing;
    delay?: number;
  }
  export interface SpringAnimationConfig {
    to: number;
    stiffness?: number;
    damping?: number;
    mass?: number;
    restThreshold?: number;
    velocity?: number;
  }
  export type SpringPresetName = 'snappy' | 'gentle' | 'wobbly' | 'stiff';
  export const springPresets: Readonly<
    Record<SpringPresetName, { stiffness: number; damping: number; mass: number }>
  >;
  export function springPreset(name: SpringPresetName): {
    stiffness: number;
    damping: number;
    mass: number;
  };
  export interface AnimationEndResult {
    finished: boolean;
  }
  export type AnimationEndCallback = (result: AnimationEndResult) => void;
  export interface CompositeAnimation {
    start(cb?: AnimationEndCallback): void;
    stop(): void;
  }
  export function timing(
    value: AnimatedValue<number>,
    config: TimingAnimationConfig,
  ): CompositeAnimation;
  export function spring(
    value: AnimatedValue<number>,
    config: SpringAnimationConfig,
  ): CompositeAnimation;
  export const Animated: {
    readonly Value: new (initial: number) => AnimatedValue<number>;
    readonly timing: typeof timing;
    readonly spring: typeof spring;
    readonly Easings: typeof Easings;
  };
  export function useAnimatedValue(initial: number): AnimatedValue<number>;

  export interface FrameLoopOptions {
    requestAnimationFrame?: (fn: (timeMs: number) => void) => unknown;
    cancelAnimationFrame?: (id: unknown) => void;
  }
  export type FrameLoopDisposer = () => void;
  export interface RenderOptions {
    animate?: boolean;
    frameLoop?: FrameLoopOptions;
  }
  export function render(
    root: Node,
    target: CommandBuffer | Renderer,
    options?: RenderOptions,
  ): void;
  export function setRenderTarget(win: Window | null): void;
  export function startFrameLoop(options?: FrameLoopOptions): FrameLoopDisposer;
  export function tickFrame(dtMs: number): void;

  export type SchedulerLane = 'sync' | 'transition';
  export const DEFAULT_SCHEDULER_FRAME_BUDGET_MS: 8;
  export function scheduleWork(fiberId: number, lane?: SchedulerLane): void;
  export function flushSync(): void;
  export function schedulerFrameBudgetMs(): number;

  export function createSignal<T>(initial: T): [() => T, (next: T | ((prev: T) => T)) => void];
  export function createMemo<T>(fn: () => T): () => T;
  export function createEffect(fn: () => void): void;
  export function untrack<T>(fn: () => T): T;
  export function batch<T>(fn: () => T): T;

  export interface Theme {
    colors: Record<string, Color>;
    spacing: Record<string, number>;
    radii: Record<string, number>;
    fontSizes: Record<string, number>;
  }
  export const defaultTheme: Theme;
  export function ThemeProvider(props: {
    value?: Theme;
    theme?: Theme;
    children?: BoundaryChild;
    key?: string;
  }): Node;
  export function useTheme(): Theme;

  export interface ViewProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    children?: BoundaryChild;
    __traceTag?: string;
  }
  export interface TextProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    children?: TextChild;
    selectable?: boolean;
  }
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
  export interface PressableState {
    hovered: boolean;
    pressed: boolean;
    focused: boolean;
  }
  export interface SyntheticEvent<T = unknown> {
    nativeEvent: T;
    x: number;
    y: number;
    defaultPrevented: boolean;
    propagationStopped: boolean;
    preventDefault(): void;
    stopPropagation(): void;
  }
  export interface PressableProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue | ((state: PressableState) => StyleValue);
    children?: BoundaryChild | ((state: PressableState) => BoundaryChild);
    disabled?: boolean;
    onPress?: (ev: SyntheticEvent) => void;
    onPressIn?: (ev: SyntheticEvent) => void;
    onPressOut?: (ev: SyntheticEvent) => void;
    onHoverIn?: (ev: SyntheticEvent) => void;
    onHoverOut?: (ev: SyntheticEvent) => void;
    onFocus?: () => void;
    /** Right-click / contextmenu handler. */
    onContextMenu?: (ev: SyntheticEvent) => void;
    onBlur?: () => void;
    onLongPress?: (ev: SyntheticEvent) => void;
  }
  export interface ButtonProps extends Omit<PressableProps, 'children'>, AccessibilityProps {
    title?: string;
    children?: string;
    textStyle?: StyleValue;
  }
  export interface ScrollViewProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    contentStyle?: StyleValue;
    children?: BoundaryChild;
    onScroll?: (offset: { x: number; y: number }) => void;
  }
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
  }
  export interface TextInputProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    value?: string;
    placeholder?: string;
    onChange?: (value: string) => void;
    onSubmit?: (value: string) => void;
    onCompose?: (preedit: string, cursor: number) => void;
    onCommit?: (committed: string) => void;
    /** Cursor caret blink interval in ms; 0 disables blink. Default 530. */
    caretBlinkMs?: number;
    /** Mask the value with bullet characters; preserves real value internally. */
    secureTextEntry?: boolean;
    /** Maximum length in code units. Inserts past this limit are clipped. */
    maxLength?: number;
    /** Select all text on focus. */
    selectAllOnFocus?: boolean;
    /** Hook fired before paste; return null to reject, return a string to override. */
    onPaste?: (text: string) => string | null;
    /** Fired when caret/selection changes. */
    onSelectionChange?: (selection: { start: number; end: number }) => void;
    /** Read-only: focus + selection + copy allowed; edits blocked. */
    readOnly?: boolean;
    /** Disabled: prevents focus and edits; cursor shows not_allowed. */
    disabled?: boolean;
    /** What pressing Tab does: 'focus' (default) advances focus; 'insert' inserts \t. */
    tabBehavior?: 'focus' | 'insert';
    /** Selection highlight color (RRGGBBAA). Default 0x3b82f654. */
    selectionColor?: number;
    /** Show a focus outline when focused. Default true. */
    focusRing?: boolean;
    /** Mobile keyboard hint (ignored on desktop today, declared for app forward-compat). */
    inputMode?: 'text' | 'numeric' | 'decimal' | 'email' | 'tel' | 'url' | 'search' | 'none';
    /** Spell-check hint (no-op today, declared for app forward-compat). */
    spellCheck?: boolean;
    /** Auto-capitalize hint (no-op today). */
    autoCapitalize?: 'none' | 'sentences' | 'words' | 'characters';
    /** Auto-correct hint (no-op today). */
    autoCorrect?: boolean;
  }

  export interface FindReplaceBarProps {
    document: FXE.TextDocument;
    initialQuery?: string;
    initialReplacement?: string;
    caseSensitive?: boolean;
    useRegex?: boolean;
    regexFlags?: string;
    searchDeadlineMs?: number;
    searchMaxMatches?: number;
    showReplace?: boolean;
    onActiveMatchChange?: (
      range: { start: number; end: number; index: number; total: number } | null,
    ) => void;
    onReplaced?: (count: number) => void;
    onClose?: () => void;
    dispatch?: (
      edits: Array<{ start: number; removed: number; inserted: string }>,
      opts?: { origin?: string },
    ) => void;
    style?: StyleValue;
  }
  export interface TextAreaProps extends Omit<TextInputProps, 'tabBehavior'>, AccessibilityProps {
    /** Suggested visible row count for sizing. Default 4. */
    numberOfLines?: number;
    /** Soft-wrap content at the box width. Default true. */
    softWrap?: boolean;
    /** Tab behavior. Default 'insert' for TextArea (browser convention for multi-line). */
    tabBehavior?: 'focus' | 'insert';
  }

  export type CaptureName = string;
  export interface HighlightStyle {
    color?: number;
    bold?: boolean;
    italic?: boolean;
    underline?: boolean;
    strikethrough?: boolean;
  }
  export type HighlightTheme =
    | ReadonlyMap<CaptureName, HighlightStyle>
    | Record<CaptureName, HighlightStyle>;
  export interface IncrementalHighlighterOptions {
    document: import('fxe').TextDocument;
    language: string;
    theme: HighlightTheme;
    defaultStyle?: HighlightStyle;
  }
  export interface IncrementalHighlighter {
    getLineDecorations: (line: number) => LineDecorations | null;
    invalidate(): void;
    revision(): number;
    dispose(): void;
  }

  // Editor primitives ------------------------------------------------------
  export interface LineSpan {
    start: number;
    end: number;
    color?: number;
    bold?: boolean;
    italic?: boolean;
    underline?: boolean;
    strikethrough?: boolean;
  }
  export interface DiagnosticUnderline {
    x1: number;
    x2: number;
    style: 'solid' | 'dashed' | 'dotted' | 'wavy';
    color?: number;
    thickness?: number;
  }
  export interface LineDecorations {
    spans?: ReadonlyArray<LineSpan>;
    selectionRects?: Float32Array;
    background?: number;
    diagnostics?: ReadonlyArray<DiagnosticUnderline>;
  }
  export type LineDecorationFn = (line: number) => LineDecorations | null;
  export interface LineViewportProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    document: FXE.TextDocument;
    lineHeight: number;
    getLineDecorations?: LineDecorationFn;
    overscan?: number;
    tabSize?: number;
    showWhitespace?: boolean;
    textColor?: number;
    scrollY?: number;
    onClickPosition?: (line: number, col: number, ev: unknown) => void;
  }
  export interface MinimapProps {
    key?: string;
    doc: FXE.TextDocument;
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
  export interface GutterMark {
    color: number;
    size?: number;
  }
  export type GutterMarkFn = (line: number) => GutterMark | null;
  export interface GutterProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    document: FXE.TextDocument;
    lineHeight: number;
    scrollY?: number;
    startLineNumber?: number;
    textColor?: number;
    focusedLineColor?: number;
    focusedLine?: number;
    getMark?: GutterMarkFn;
  }
  export interface OutlineEntry {
    line: number;
    depth: number;
    label?: string;
  }
  export interface OutlineProvider {
    getStickyEntries(
      doc: FXE.TextDocument,
      topVisibleLine: number,
      maxDepth: number,
    ): OutlineEntry[];
    revision?(doc: FXE.TextDocument): number;
  }
  export interface TreeSitterOutlineOptions {
    language: string;
    /** Capture names that mark a 'definition' line. Defaults to ['function','type','class','method','constructor','property']. */
    definitionCaptures?: ReadonlyArray<string>;
    tabWidth?: number;
  }
  export interface StickyScrollProps {
    key?: string;
    doc: FXE.TextDocument;
    scrollOffset: number;
    lineHeight: number;
    width: number;
    outline: OutlineProvider;
    maxDepth?: number;
    getLineSpans?: (line: number) => readonly LineSpan[];
    textColor?: number;
    backgroundColor?: number;
    borderColor?: number;
    style?: Style;
    testID?: string;
  }
  export function createIndentOutlineProvider(opts?: { tabWidth?: number }): OutlineProvider;
  export function createTreeSitterOutlineProvider(): OutlineProvider;
  export function createTreeSitterOutlineProvider(opts: TreeSitterOutlineOptions): OutlineProvider;
  export function createTreeSitterOutlineProvider(
    cb: (doc: FXE.TextDocument, topVisibleLine: number, maxDepth: number) => OutlineEntry[],
  ): OutlineProvider;

  export interface BracketContextProvider {
    isStringOrComment?(off: number): boolean;
  }
  export interface EditableAreaProps extends AccessibilityProps {
    key?: string;
    style?: StyleValue;
    document: FXE.TextDocument;
    history?: {
      dispatch(
        edits: ReadonlyArray<{ start: number; removed: number; inserted: string }>,
        opts?: { origin?: string; break?: boolean },
      ): unknown;
      undo(): boolean;
      redo(): boolean;
      breakCoalescing?(): void;
    };
    lineHeight: number;
    tabString?: string;
    tabSize?: number;
    showWhitespace?: boolean;
    textColor?: number;
    getLineDecorations?: LineDecorationFn;
    bracketProvider?: BracketContextProvider;
    indentLines?: boolean;
    indentUnit?: 'tab' | number;
    onCursorChange?: (line: number, col: number) => void;
    scrollY?: number;
    onScrollChange?: (scrollY: number) => void;
  }
  export interface SyntaxPalette {
    comment: number;
    string: number;
    number: number;
    constant: number;
    keyword: number;
    type: number;
    function: number;
    property: number;
    tag: number;
    attribute: number;
  }
  export interface MarkdownTheme extends Theme {
    colors: Theme['colors'] & {
      code: number;
      codeBg: number;
      quote: number;
      link: number;
      headingRule: number;
      tableHeaderBg: number;
    };
    fonts: { body: string; mono: string };
    syntax: SyntaxPalette;
  }
  export interface MarkdownProps {
    key?: string;
    source: string;
    style?: StyleValue;
    theme?: Partial<MarkdownTheme>;
    onLinkPress?(href: string): void;
    onWikilinkPress?(target: string): void;
  }

  export function View(props: ViewProps): Node;
  export function Text(props: TextProps): Node;
  export function Image(props: ImageProps): Node;
  export function Pressable(props: PressableProps): Node;
  export function Button(props: ButtonProps): Node;
  export function ScrollView(props: ScrollViewProps): Node;
  export function VirtualList<T>(props: VirtualListProps<T>): Node;
  export function TextInput(props: TextInputProps): Node;
  export function TextArea(props: TextAreaProps): Node;
  export function LineViewport(props: LineViewportProps): Node;
  export function Gutter(props: GutterProps): Node;
  export function EditableArea(props: EditableAreaProps): Node;
  export function FindReplaceBar(props: FindReplaceBarProps): Node;
  export const Markdown: (props: MarkdownProps) => Node;
  export function Minimap(props: MinimapProps): Node;
  export function StickyScroll(props: StickyScrollProps): Node;
  export function usePressableState(): PressableState;
  export function useHover(): boolean;
  export function useFocus(): boolean;
  export function createIncrementalHighlighter(
    opts: IncrementalHighlighterOptions,
  ): IncrementalHighlighter;
  export function defaultHighlightTheme(theme: Theme): HighlightTheme;

  export interface DevToolsShortcutOptions {
    accelerator?: string;
    window?: Window;
    onError?: (err: unknown) => void;
  }

  export interface DevToolsShortcutHandle {
    dispose(): void;
    accelerator: string;
  }

  export function defaultDevToolsAccelerator(): string;
  export function installDevToolsShortcut(
    opts?: DevToolsShortcutOptions,
  ): DevToolsShortcutHandle | null;
  export interface MountOptions {
    renderer?: Renderer;
    theme?: Theme;
    /**
     * When `false`, mount runs a continuous requestAnimationFrame loop that
     * re-renders every tick — necessary for animated/realtime apps that
     * read clocks or external state outside the reconciler.
     *
     * When `undefined` (default) or `true`, mount renders **on demand**:
     * once at startup, then again only when state changes (setState,
     * external store updates, focus / hover / press dispatch, or an
     * explicit `window.requestRedraw()`). Idle CPU cost is zero.
     *
     * Static UIs and form-style apps want the default. Only flip to
     * `lazy: false` if you genuinely need every frame.
     */
    lazy?: boolean;
    /**
     * When `undefined` (default) or `true`, mount auto-registers the standard
     * DevTools shortcut if `FXE_DEBUG_PORT` is set in the process environment.
     * Pass `false` to opt out, or provide a custom accelerator string.
     */
    devTools?: boolean | { accelerator?: string };
  }
  export function mount(root: Node, window: Window, opts?: MountOptions): () => void;
  export function useFocusTrap(groupId: string | null, enabled?: boolean): void;

  export interface HitTarget {
    id: string;
    rect: LayoutResult;
    z: number;
    tabIndex?: number;
    focusGroup?: string;
    componentType?: string;
    a11y?: AccessibilityProps;
    onFocus?: () => void;
    onBlur?: () => void;
    onKeyDown?: (ev: unknown) => void;
    onKeyPress?: (ev: unknown) => void;
    onDrag?: (ev: SyntheticEvent) => void;
    onContextMenu?: (ev: SyntheticEvent) => void;
    onPressIn?: (ev: SyntheticEvent) => void;
    onPressOut?: (ev: SyntheticEvent) => void;
    onPress?: (ev: SyntheticEvent) => void;
    onHoverIn?: (ev: SyntheticEvent) => void;
    onHoverOut?: (ev: SyntheticEvent) => void;
    onWheel?: (ev: SyntheticEvent & { dx: number; dy: number }) => void;
    onCompose?: (ev: WindowEventMap['compose']) => void;
    onEditCommand?: (action: EditMenuAction) => void;
    cursor?: CursorKind;
  }
  export function clearHitTargets(): void;
  export function registerHitTarget(
    target: Omit<HitTarget, 'z'> & { z?: number } & Record<string, unknown>,
  ): void;
  export interface ClipboardSink {
    onCopy(text: string): void;
    onCut(text: string): void;
    onPaste(): string | Promise<string>;
  }
  export interface FocusAdvancePreempt {
    shouldPreemptFocusAdvance(ev: WindowEventMap['keydown']): boolean;
  }
  export function attachClipboardSink(sink: ClipboardSink): () => void;
  export function detachClipboardSink(): void;
  export function attachFocusAdvancePreempt(preempt: FocusAdvancePreempt): () => void;
  export function detachFocusAdvancePreempt(preempt: FocusAdvancePreempt): void;
  export function hitTest(x: number, y: number): HitTarget | null;
  export function dispatchMouseMove(
    ev: WindowEventMap['mousemove'],
    cursorSink?: { setCursor?(kind: CursorKind): void },
    dragSink?: { startDrag?(payload: DragOutPayload): boolean },
  ): void;
  export function dispatchMouseDown(ev: WindowEventMap['mousedown']): void;
  export function dispatchMouseUp(ev: WindowEventMap['mouseup']): void;
  export function dispatchWheel(ev: WindowEventMap['wheel'] & { x?: number; y?: number }): void;
  export function dispatchKeyDown(
    ev: WindowEventMap['keydown'],
    clipboardSink?: ClipboardSink,
  ): void;
  export function dispatchKeyPress(ev: WindowEventMap['keypress']): void;
  export function resetEventPipeline(): void;
  export function setFocusTrapGroup(id: string | null): void;
  export function focusTarget(): HitTarget | null;
  export function focusTarget(id: string | 'next' | 'previous'): HitTarget | null;
  export function focusedTargetId(): string | null;
  export function clearFocus(): void;

  export type EditMenuAction = 'undo' | 'redo' | 'cut' | 'copy' | 'paste' | 'selectAll';
  export interface EditMenuOptions {
    hasSelection?: boolean;
    canUndo?: boolean;
    canRedo?: boolean;
    canPaste?: boolean;
    readOnly?: boolean;
    disabled?: boolean;
  }
  export interface InstallApplicationEditMenuOptions {
    extras?: ReadonlyArray<unknown>;
    dispatch?: (action: EditMenuAction) => void;
  }
  export interface DragOutPayload {
    files?: readonly string[];
    text?: string;
    html?: string;
  }
  export function buildEditMenuItems(opts?: EditMenuOptions): unknown[];
  export function buildApplicationEditSubmenu(): unknown;
  export function editActionFromMenuId(id: string): EditMenuAction | null;
  export function popupEditMenu(
    x: number,
    y: number,
    opts?: EditMenuOptions,
  ): Promise<EditMenuAction | null>;
  export function installApplicationEditMenu(opts?: InstallApplicationEditMenuOptions): () => void;
}

declare module 'fxe-ui/jsx-runtime' {
  export type JSXChild =
    | import('fxe-ui').Node
    | string
    | number
    | readonly JSXChild[]
    | null
    | undefined
    | boolean;
  export type JSXNode = import('fxe-ui').Node;
  export const Fragment: symbol;
  export type FunctionComponentType<P extends object = Record<string, unknown>> = (
    props: P,
  ) => JSXNode;
  export type ClassComponentType<P extends object = Record<string, unknown>> = new (
    props: P,
  ) => {
    render: () => JSXNode;
    props?: unknown;
  };
  export function jsx<P extends Record<string, unknown>>(
    type:
      | 'view'
      | 'text'
      | 'image'
      | 'pressable'
      | 'scroll'
      | 'input'
      | typeof Fragment
      | FunctionComponentType<P>
      | ClassComponentType<P>,
    props: (P & { children?: JSXChild; key?: string }) | null,
    key?: string,
  ): JSXNode;
  export function jsxs<P extends Record<string, unknown>>(
    type:
      | 'view'
      | 'text'
      | 'image'
      | 'pressable'
      | 'scroll'
      | 'input'
      | typeof Fragment
      | FunctionComponentType<P>
      | ClassComponentType<P>,
    props: (P & { children?: JSXChild; key?: string }) | null,
    key?: string,
  ): JSXNode;

  export namespace JSX {
    type Element = JSXNode;
    interface ElementChildrenAttribute {
      children: Record<keyof any, never>;
    }
    interface IntrinsicElements {
      view: import('fxe-ui').ViewProps;
      text: import('fxe-ui').TextProps;
      image: import('fxe-ui').ImageProps;
      pressable: import('fxe-ui').PressableProps;
      scroll: import('fxe-ui').ScrollViewProps;
      input: import('fxe-ui').TextInputProps;
    }
  }
}

declare global {
  namespace FXEUI {
    type EasingFunction = import('fxe-ui').EasingFunction;
    type SpringPresetName = import('fxe-ui').SpringPresetName;
    const Easings: typeof import('fxe-ui').Easings;
    function cubicBezier(
      x1: number,
      y1: number,
      x2: number,
      y2: number,
    ): import('fxe-ui').EasingFunction;
    const springPresets: typeof import('fxe-ui').springPresets;
    function springPreset(name: import('fxe-ui').SpringPresetName): {
      stiffness: number;
      damping: number;
      mass: number;
    };
  }
  namespace FXE {
    type RenderStatsSnapshot = {
      verticesSubmitted: number;
      indicesSubmitted: number;
      queueCalls: number;
      cacheHits: number;
      cacheMisses: number;
      rebuilds: number;
      frames: number;
    };
    interface RenderStatsNamespace {
      snapshot(): RenderStatsSnapshot;
      reset(): void;
      recordCacheHit(): void;
      recordCacheMiss(): void;
      recordRebuild(): void;
      recordQueueCall(): void;
      beginFrame(): void;
    }
  }
  var __fxe_devtools:
    | {
        fiberTree: () => import('fxe-ui').DevtoolsFiberTreeSnapshot;
        setPaintFlash: (enabled: boolean) => void;
      }
    | undefined;
}
