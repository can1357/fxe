declare namespace FXE {
  type Vec2 = readonly [number, number] | Float32Array;
  type Vec3 = readonly [number, number, number] | Float32Array;
  type Vec4 = readonly [number, number, number, number] | Float32Array;
  type Mat4 = Float32Array;
  type Color = number | readonly [number, number, number, number];
  interface GradientPaint {
    readonly __fxePaint: number;
    readonly kind: 1 | 2 | 3;
    readonly p0: readonly [number, number, number, number];
    readonly p1: readonly [number, number, number, number];
    readonly stops: Float32Array;
  }
  type Paint = Color | GradientPaint;

  enum VertexTopology {
    Triangle = 0,
    Line = 1,
  }

  interface BufferViews {
    verts: Float32Array;
    idxs: Uint32Array;
    epoch: number;
  }

  interface Allocation extends BufferViews {
    base: number;
    indexBase: number;
  }

  class CommandBuffer {
    constructor();
    readonly __fxe_v_len: number;
    readonly __fxe_tri_len: number;
    readonly __fxe_line_len: number;
    readonly __fxe_epoch: number;
    clear(): void;
    epoch(): number;
    vertexCount(): number;
    /**
     * Axis-aligned bounding box over all queued vertex positions.
     * Returns null when the buffer is empty. Used by surface caching to
     * size an offscreen target around the cached subtree.
     */
    bounds(): { x: number; y: number; width: number; height: number } | null;
    indexCount(topology?: VertexTopology): number;
    transform(matrix: Mat4): void;
    queue(other: CommandBuffer | Renderer, matrix?: Mat4, tint?: Vec4): void;
    buffers(topology?: VertexTopology): BufferViews;
    vertexBuffer(): Float32Array;
    indexBuffer(topology?: VertexTopology): Uint32Array;
    allocate(vertexCount: number, indexCount: number, topology: VertexTopology): Allocation;
  }

  /**
   * Native rope-style text buffer for editor-grade documents. Stores text
   * as UTF-16 code units (offsets line up with V8 string indices). Backed
   * by a piece array over an immutable seed + append-only edit buffer; all
   * O(log P) for offset/line lookups, O(log P + edit length) for replace.
   *
   * No JS-side string is ever rebuilt for an edit — the V8 trampoline cost
   * is the only per-keystroke overhead. For a 10 MB file load+single-char
   * insert the budget is < 0.5 ms on M-series.
   */
  class TextDocument {
    constructor(initial?: string);
    /** Total UTF-16 code-unit length. */
    length(): number;
    /** Number of lines (a doc with N newlines has N+1 lines). */
    lineCount(): number;
    /** Bumped on every replace / applyBatch. */
    revision(): number;
    /** Number of pieces in the underlying rope. Diagnostic only. */
    pieceCount(): number;
    /** Whole-document UTF-16 string. Allocates. */
    text(): string;
    /** Slice [start, end) of the buffer. */
    slice(start: number, end?: number): string;
    /** UTF-16 code-unit at offset, or 0 if out of range. */
    charCodeAt(offset: number): number;
    /** Offset of the start of the given line (0-indexed). */
    lineToOffset(line: number): number;
    /** 0-indexed line number containing `offset`. */
    offsetToLine(offset: number): number;
    /** {start, end} of `line`. `end` excludes the trailing '\n'. */
    lineRange(line: number): { start: number; end: number };
    /** Line text, no trailing '\n'. */
    lineText(line: number): string;
    /** Convert offset → {line, col} in one call (faster than two). */
    offsetToLineCol(offset: number): { line: number; col: number };
    /** Convert {line, col} → offset. col is clamped to the line length. */
    lineColToOffset(line: number, col: number): number;
    /**
     * Replace [start, end) with `text`. Returns the captured edit, including
     * deleted text for undo. Bumps revision and notifies subscribers.
     */
    replace(start: number, end: number, text: string): TextDocumentEdit;
    /**
     * Apply N replaces in one revision. Edits MUST be sorted ascending by
     * `start` and non-overlapping (overlap throws RangeError).
     */
    applyBatch(
      edits: ReadonlyArray<{ start: number; removed: number; inserted: string }>,
    ): TextDocumentEdit[];
    /**
     * Listen for edits. Callback receives the array of applied edits in
     * ascending start order. Returns a subscription id usable with
     * `unsubscribe()`.
     */
    subscribe(fn: (edits: TextDocumentEdit[]) => void): bigint;
    unsubscribe(id: bigint | number): void;
    /**
     * Linear literal substring scan over the document. Use
     * `caseInsensitive` for ASCII-folded matching. `limit` caps the result
     * count; the scan stops as soon as `limit` matches are produced.
     */
    searchLiteral(
      needle: string,
      opts?: { from?: number; limit?: number; caseInsensitive?: boolean },
    ): Array<{ start: number; end: number }>;
  }

  interface TextDocumentEdit {
    /** Pre-edit start offset. */
    start: number;
    /** Number of characters removed. */
    removed: number;
    /** Inserted text. */
    inserted: string;
    /** Removed text — captured for undo. */
    deleted: string;
  }

  class Path {
    constructor();
    moveTo(x: number, y: number): this;
    lineTo(x: number, y: number): this;
    quadTo(cx: number, cy: number, x: number, y: number): this;
    cubicTo(c1x: number, c1y: number, c2x: number, c2y: number, x: number, y: number): this;
    arc(
      cx: number,
      cy: number,
      radius: number,
      startAngle: number,
      endAngle: number,
      ccw?: boolean,
    ): this;
    close(): this;
    reset(): this;
  }

  interface RendererOptions {
    multisampleCount?: number;
    enableBloom?: boolean;
    vsync?: boolean;
  }

  interface Viewport {
    at: [number, number];
    size: [number, number];
  }

  class Renderer {
    constructor(window: Window, options?: RendererOptions);
    clear(): void;
    epoch(): number;
    vertexCount(): number;
    bounds(): { x: number; y: number; width: number; height: number } | null;
    indexCount(topology?: VertexTopology): number;
    transform(matrix: Mat4): void;
    queue(other: CommandBuffer | Renderer, matrix?: Mat4, tint?: Vec4): void;
    buffers(topology?: VertexTopology): BufferViews;
    vertexBuffer(): Float32Array;
    indexBuffer(topology?: VertexTopology): Uint32Array;
    allocate(vertexCount: number, indexCount: number, topology: VertexTopology): Allocation;
    isEmpty(): boolean;
    beginFrame(eyePosition?: Vec3, eyeDirection?: Vec3, worldViewProjection?: Mat4): void;
    endFrame(): void;
    setClearColor(r: number, g: number, b: number, a?: number): void;
    setClearColor(rgba: Vec4): void;
    setMultisample(count: number): void;
    setBloom(enabled: boolean): void;
    screen(): [number, number];
    worldToScreen(position: Vec3): [number, number, number, number];
    viewport(): Viewport;
    /**
     * Bind an OffscreenRenderer's color attachment to user-texture slot
     * `slot` (0..3). Pass `null` to clear. Subsequent
     * `Primitives.drawTextureQuad(cb, slot, ...)` calls sample from this
     * texture. Used by surface caching to bake stable subtrees.
     */
    bindUserTexture(slot: number, source: OffscreenRenderer | null): void;
  }

  interface OffscreenRendererOptions {
    width: number;
    height: number;
    multisample?: number;
    mipLevels?: number;
    enableDepth?: boolean;
    /**
     * Existing Renderer (or OffscreenRenderer) whose GPU device this
     * offscreen will share. Required when the offscreen's color
     * attachment will be sampled by another renderer via
     * `bindUserTexture(...)` — cross-device texture sharing is not
     * supported in WebGPU/Dawn.
     */
    parent?: Renderer;
  }

  class OffscreenRenderer extends Renderer {
    constructor(options: OffscreenRendererOptions);
    readPixels(): Uint8Array;
  }

  type VertexFormat = 'f32' | 'f32x2' | 'f32x3' | 'f32x4' | 'u32' | 'u8x4-norm';
  interface VertexAttribute {
    location: number;
    offset: number;
    format: VertexFormat;
  }
  interface PipelineDesc {
    wgsl: string;
    vsEntry?: string;
    fsEntry?: string;
    vertexStride: number;
    attrs: VertexAttribute[];
    depthTest?: boolean;
    blend?: boolean;
  }
  class Pipeline {
    constructor(renderer: Renderer, desc: PipelineDesc);
    updateUniforms(data: Float32Array | Uint8Array): void;
    bindTexture(binding: number, image: ImageHandle | number): void;
    draw(
      cb: CommandBuffer | Renderer,
      vertices: Float32Array,
      indices: Uint32Array,
      matrix: Mat4,
    ): void;
  }

  interface WindowOptions {
    width?: number;
    height?: number;
    x?: number;
    y?: number;
    fullscreen?: boolean;
    visible?: boolean;
    resizable?: boolean;
    decorated?: boolean;
    transparent?: boolean;
    alwaysOnTop?: boolean;
    maximized?: boolean;
    minWidth?: number;
    minHeight?: number;
    maxWidth?: number;
    maxHeight?: number;
    title?: string;
    /**
     * Path to a JS/TS file to execute inside the same isolate before the
     * window's main script runs. The preload script has full access to all
     * fxe globals; it can install a curtained API onto globalThis to mediate
     * what the main script can do.
     */
    preload?: string;
    /**
     * Run this window in its own V8 isolate. Default 'shared'. v1 starts a
     * dedicated isolate thread for 'own', while GLFW/render objects still stay
     * on the shared main-thread path until cross-thread marshaling lands.
     */
    isolate?: 'shared' | 'own';
    /**
     * Capability allowlists. When unset, all operations are permitted (legacy
     * default). When set, every binding consults the allowlist before performing
     * the operation; disallowed calls throw an Error tagged with name 'PermissionDenied'.
     *
     * - `fs`: true (allow all) | false (deny all) | string[] (allow paths under any of these prefixes; checked after canonicalisation).
     * - `net`: true | false | string[] (allow only requests whose URL host matches any entry; entries may be 'example.com' or 'sub.example.com:8080').
     * - `shell`: true | false (gates shell.openExternal/showItemInFolder/trashItem/beep).
     * - `native`: true | false (reserved for future native-plugin loading; no enforcement yet).
     * - `webauthn`: true | false | object (gates navigator.credentials.{create,get} for the virtual authenticator).
     */
    permissions?: {
      fs?: boolean | string[];
      net?: boolean | string[];
      shell?: boolean;
      native?: boolean;
      webauthn?:
        | boolean
        | {
            rpIds: string[];
            attestation?: 'none' | 'indirect' | 'direct';
            userVerification?: 'discouraged' | 'preferred' | 'required';
            transports?: Array<'usb' | 'nfc' | 'ble' | 'internal' | 'hybrid'>;
            allowVirtualAuthenticator?: boolean;
          };
    };
  }

  interface ClipboardImage {
    width: number;
    height: number;
    data: Uint8Array;
  }

  interface DragImage {
    width: number;
    height: number;
    data: Uint8Array | Uint8ClampedArray;
  }
  interface WindowRunOptions {
    fps?: number;
    animate?: boolean;
  }

  type CursorKind =
    | 'arrow'
    | 'ibeam'
    | 'crosshair'
    | 'hand'
    | 'hresize'
    | 'vresize'
    | 'allResize'
    | 'neswResize'
    | 'nwseResize'
    | 'notAllowed'
    | 'hidden';

  type TitleBarStyle = 'default' | 'hidden' | 'hiddenInset' | 'customButtons';
  type VibrancyKind = 'sidebar' | 'titlebar' | 'menu';

  interface KeyEvent {
    type: 'keydown' | 'keyup';
    key: number;
    scancode: number;
    modifiers: number;
  }
  interface KeypressEvent {
    type: 'keypress';
    key: number;
    scancode: number;
    modifiers: number;
    codepoint: number;
  }
  interface MouseMoveEvent {
    type: 'mousemove';
    x: number;
    y: number;
    dx: number;
    dy: number;
    modifiers: number;
  }
  interface MouseButtonEvent {
    type: 'mousedown' | 'mouseup';
    x: number;
    y: number;
    button: number;
    modifiers: number;
  }
  interface WheelEvent {
    type: 'wheel';
    dx: number;
    dy: number;
    modifiers: number;
  }
  interface ResizeEvent {
    type: 'resize';
    width: number;
    height: number;
  }
  interface MoveEvent {
    type: 'move';
    x: number;
    y: number;
  }
  interface ScaleEvent {
    type: 'scale';
    scaleX: number;
    scaleY: number;
  }
  interface DropEvent {
    type: 'drop';
    paths: string[];
  }
  interface ComposeEvent {
    type: 'compose';
    preedit: string;
    cursor: number;
    committed: string;
  }
  interface SimpleWindowEvent<T extends string> {
    type: T;
  }
  interface DragEnterEvent {
    type: 'dragenter';
    x: number;
    y: number;
    paths: string[];
  }
  interface DragOverEvent {
    type: 'dragover';
    x: number;
    y: number;
    paths: string[];
  }
  interface DragLeaveEvent {
    type: 'dragleave';
  }

  interface WindowMessageEvent {
    type: 'message';
    channel: string;
    args: unknown[];
  }

  interface WindowEventMap {
    keydown: KeyEvent;
    keyup: KeyEvent;
    keypress: KeypressEvent;
    message: WindowMessageEvent;
    mousemove: MouseMoveEvent;
    mousedown: MouseButtonEvent;
    mouseup: MouseButtonEvent;
    wheel: WheelEvent;
    cursorenter: SimpleWindowEvent<'cursorenter'>;
    cursorleave: SimpleWindowEvent<'cursorleave'>;
    resize: ResizeEvent;
    move: MoveEvent;
    scale: ScaleEvent;
    focus: SimpleWindowEvent<'focus'>;
    blur: SimpleWindowEvent<'blur'>;
    minimize: SimpleWindowEvent<'minimize'>;
    restore: SimpleWindowEvent<'restore'>;
    maximize: SimpleWindowEvent<'maximize'>;
    unmaximize: SimpleWindowEvent<'unmaximize'>;
    close: SimpleWindowEvent<'close'>;
    drop: DropEvent;
    dragenter: DragEnterEvent;
    dragover: DragOverEvent;
    dragleave: DragLeaveEvent;
    compose: ComposeEvent;
  }

  type WindowEventName = keyof WindowEventMap;
  type WindowEventHandler<K extends WindowEventName> = (ev: WindowEventMap[K]) => void;
  type WindowDisposer = () => void;

  class Window {
    constructor(options?: WindowOptions);
    readonly isolateMode: 'shared' | 'own';
    /** 0 for the shared/main isolate, otherwise the dedicated runtime id. */
    readonly isolateId: number;
    static exit(): void;

    poll(): void;
    close(): void;
    shouldClose(): boolean;
    framebufferSize(): [number, number];
    setVsync(enabled: boolean): void;
    waitEvents(): void;
    waitEventsTimeout(seconds: number): void;
    requestRedraw(): void;
    takeRedrawRequest(): boolean;
    run(callback: (window: Window) => void | Promise<void>, options?: WindowRunOptions): void;
    /**
     * Register a per-frame callback without entering App.run / Window.run.
     * Call once during setup (typically from a UI compositor); pass `null`
     * to clear. The callback is invoked once per `requestRedraw()` ack
     * while the OS event loop is running, so a separate `App.run()` is
     * still required to drive ticks.
     */
    setFrameCallback(cb: ((window: Window) => void) | null): void;

    title(): string;
    getTitle(): string;
    setTitle(title: string): void;
    setSize(width: number, height: number): void;
    size(): [number, number];
    setPosition(x: number, y: number): void;
    position(): [number, number];
    bounds(): { x: number; y: number; width: number; height: number };
    getBounds(): { x: number; y: number; width: number; height: number };
    setMinSize(width: number, height: number): void;
    minSize(): [number, number] | null;
    getMinSize(): [number, number] | null;
    setMaxSize(width: number, height: number): void;
    maxSize(): [number, number] | null;
    getMaxSize(): [number, number] | null;
    setOpacity(alpha: number): void;
    /**
     * Sets the platform compositor's backdrop colour for any region of the
     * surface not yet covered by a freshly presented frame. Use to suppress
     * the brief flash during live resize where the layer bounds grow on one
     * compositor transaction but the new frame lands on the next.
     *
     * Pass either a packed `0xRRGGBBAA` integer or four 0..1 RGBA floats.
     * Maps to `CAMetalLayer.backgroundColor` on macOS; no-op on platforms
     * without an equivalent surface property.
     */
    setBackgroundColor(rgba: number): void;
    setBackgroundColor(r: number, g: number, b: number, a: number): void;
    opacity(): number;
    setAlwaysOnTop(enabled: boolean): void;
    isAlwaysOnTop(): boolean;
    setResizable(enabled: boolean): void;
    isResizable(): boolean;
    setDecorated(enabled: boolean): void;
    isDecorated(): boolean;
    setTitleBarStyle(style: TitleBarStyle): void;
    setTrafficLightPosition(x: number, y: number): boolean;
    setWindowControlsOverlay(enabled: boolean): boolean;
    setVibrancy(kind: VibrancyKind | null): boolean;
    setBlurBehind(enabled: boolean): boolean;
    isTransparent(): boolean;
    setVisible(visible: boolean): void;
    setIcon(rgba: Uint8Array | Uint8ClampedArray, width: number, height: number): boolean;
    minimize(): void;
    maximize(): void;
    restore(): void;
    focus(): void;
    requestAttention(): void;
    center(): void;
    isFocused(): boolean;
    isMinimized(): boolean;
    isMaximized(): boolean;
    isVisible(): boolean;
    setFullscreen(enabled: boolean, monitorIndex?: number): void;
    isFullscreen(): boolean;
    setCursor(kind: CursorKind): void;
    setCursorVisible(visible: boolean): void;
    setCursorPos(x: number, y: number): void;
    cursorPos(): [number, number];
    setCursorLock(locked: boolean): void;
    setRawMouseMotion(enabled: boolean): void;
    isRawMouseMotionSupported(): boolean;
    /**
     * Tell the OS compositor to exclude this window from screen capture / recording.
     * macOS uses NSWindowSharingNone; Windows uses WDA_EXCLUDEFROMCAPTURE; Linux
     * returns false (no portable X11/Wayland equivalent) and warns once.
     *
     * The FXE debug screenshot path (Page.captureFrame / Page.screenshot) reads
     * the framebuffer directly and is NOT affected — this hook only blocks OS
     * capture tools (screen recorders, AirPlay, Win+G overlay, etc.).
     *
     * @returns true when the platform applied the request.
     */
    setContentProtection(enabled: boolean): boolean;
    isContentProtectionEnabled(): boolean;
    setCursorImage(
      rgba: Uint8Array | Uint8ClampedArray,
      width: number,
      height: number,
      hotX: number,
      hotY: number,
    ): boolean;
    clearCursorImage(): void;
    clipboardText(): string;
    setClipboardText(text: string): void;
    readClipboardImage(): ClipboardImage | null;
    writeClipboardImage(image: ClipboardImage | DragImage): boolean;
    clipboardHtml(): string | null;
    setClipboardHtml(html: string): boolean;
    clipboardRtf(): string | null;
    setClipboardRtf(rtf: string): boolean;
    clipboardMime(mime: string): Uint8Array | null;
    setClipboardMime(mime: string, bytes: Uint8Array | Uint8ClampedArray): boolean;
    setDragRegion(
      rects:
        | Array<readonly [number, number, number, number]>
        | Array<{ x: number; y: number; width: number; height: number }>,
    ): void;
    startDrag(payload: {
      files?: string[];
      text?: string;
      html?: string;
      image?: DragImage;
      icon?: DragImage;
    }): boolean;
    send(channel: string, ...args: unknown[]): void;

    on<K extends WindowEventName>(event: K, handler: WindowEventHandler<K>): WindowDisposer;
    off<K extends WindowEventName>(event: K, handler?: WindowEventHandler<K>): void;
    removeAllListeners(event?: WindowEventName): void;
  }

  interface MonitorInfo {
    name: string;
    x: number;
    y: number;
    width: number;
    height: number;
    workX: number;
    workY: number;
    workWidth: number;
    workHeight: number;
    scaleX: number;
    scaleY: number;
    refreshHz: number;
    primary: boolean;
  }

  interface MonitorsNamespace {
    list(): MonitorInfo[];
    primary(): MonitorInfo;
  }

  type UpdateChannel = 'stable' | 'beta' | 'alpha';

  interface AutoUpdateOptions extends RequestInit {
    expectedPublicKey?: string;
    expectedFeedPublicKey?: string;
    publicKey?: string;
    deviceId?: string;
  }

  interface InstallUpdateOptions extends RequestInit {
    manifestUrl?: string;
    expectedPublicKey?: string;
    expectedFeedPublicKey?: string;
    publicKey?: string;
    relaunch?: boolean;
  }

  interface InstallUpdateResult {
    installed: boolean;
    pendingPath?: string;
    reason?: string;
  }

  interface AutoUpdateResult {
    available: boolean;
    version?: string;
    url?: string;
    sha256?: string;
    channel?: UpdateChannel;
    rolloutPercent?: number;
    canInstall: boolean;
    reason?: string;
    installUnavailableReason?: string;
    missingCapabilities: string[];
  }

  interface UpdateNamespace {
    setChannel(channel: UpdateChannel): void;
    getChannel(): UpdateChannel;
    rollback(): boolean;
    history(): string[];
    markReady(): boolean;
    hasPendingFirstLaunch(): boolean;
    checkForUpdates(url: string, opts?: AutoUpdateOptions): Promise<AutoUpdateResult>;
    install(opts?: InstallUpdateOptions): Promise<InstallUpdateResult>;
  }

  interface AppRunOptions {
    animate?: boolean;
    fps?: number;
  }

  interface CrashReporter {
    start(options: {
      productName: string;
      productVersion?: string;
      submitURL?: string;
      crashDir?: string;
      uploadToServer?: boolean;
    }): boolean;
    listDumps(): string[];
    getLastDumpPath(): string | null;
    selfTest(): boolean;
  }

  interface PowerMonitor {
    on(
      event:
        | 'suspend'
        | 'resume'
        | 'lock-screen'
        | 'unlock-screen'
        | 'on-battery'
        | 'on-ac'
        | 'idle'
        | 'active',
      cb: () => void,
    ): () => void;
    on(event: 'online' | 'offline', cb: () => void): () => void;
    isOnBattery(): boolean;
    isOnline(): boolean;
    systemIdleSeconds(): number;
  }

  type SleepInhibitWhat = 'idle' | 'sleep';

  interface PowerNamespace {
    inhibitSleep(options: { reason: string; what: SleepInhibitWhat }): () => void;
  }

  interface RecentDocumentsNamespace {
    add(path: string): boolean;
    list(): string[];
    clear(): boolean;
  }

  interface BookmarkNamespace {
    persist(path: string): string;
    resolve(blob: string): { path: string; isStale: boolean };
    startAccessing(blob: string): boolean;
    stopAccessing(blob: string): void;
  }

  type CookieSameSite = 'Strict' | 'Lax' | 'None' | 'strict' | 'lax' | 'none';

  interface Cookie {
    name: string;
    value: string;
    url?: string;
    domain?: string;
    path?: string;
    expires?: number;
    secure?: boolean;
    httpOnly?: boolean;
    hostOnly?: boolean;
    sameSite?: CookieSameSite;
  }

  interface CookieFilter {
    name?: string;
    domain?: string;
    url?: string;
  }

  interface CookiesNamespace {
    getAll(filter?: CookieFilter): Cookie[];
    set(cookie: Cookie): void;
    remove(name: string, url: string): void;
    clear(): void;
    persist(path: string): void;
  }

  interface SessionNamespace {
    cookies: CookiesNamespace;
  }

  interface AppNamespace {
    /**
     * Run the canonical event loop. With `animate: true` (or a positive `fps`)
     * the loop drives every registered window's onFrame at the requested
     * cadence and starts the shared fxe-ui frame bridge when fxe-ui is
     * loaded. The first call (Window.run or App.run) drives; subsequent calls
     * from inside an onFrame are no-ops that just register their callback.
     */
    run(): void;
    run(opts: AppRunOptions): void;
    /** Close every registered window. The active loop exits on the next iteration. */
    quit(): void;
    /** Live snapshot of registered windows (window 0 = the first opened). */
    windows(): Window[];
    openWindow(options?: WindowOptions): Window;
    openDevTools(window?: Window): Window | null;
    checkForUpdates(url: string, opts?: AutoUpdateOptions): Promise<AutoUpdateResult>;
    installUpdate(opts?: InstallUpdateOptions): Promise<InstallUpdateResult>;
    update: UpdateNamespace;
    bookmark: BookmarkNamespace;
    powerMonitor: PowerMonitor;
    power: PowerNamespace;
    recentDocuments: RecentDocumentsNamespace;
    session: SessionNamespace;
    crashReporter: CrashReporter;
    crashReport: CrashReporter;
    /** @internal Raw accessibility snapshot bridge for native providers. */
    __fxeUpdateAccessibilityTree(windowId: number, snapshotJson: string): void;
  }

  interface PrimitivesNamespace {
    readonly OP_FILL_RECT: 1;
    readonly OP_DRAW_RECT: 2;
    readonly OP_FILL_TRIANGLE: 3;
    readonly OP_DRAW_LINE: 4;
    readonly OP_DRAW_TEXT: 5;
    readonly OP_FILL_PATH: 6;
    readonly OP_STROKE_PATH: 7;

    drawLine(
      cb: CommandBuffer | Renderer,
      src: Vec4,
      dst: Vec4,
      color?: Color,
      thickness?: number,
    ): void;
    fillTriangle(cb: CommandBuffer | Renderer, a: Vec4, b: Vec4, c: Vec4, color?: Color): void;
    fillRect(
      cb: CommandBuffer | Renderer,
      at: Vec2,
      size: Vec2,
      depth?: number,
      color?: Paint,
    ): void;
    fillRect(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      width: number,
      height: number,
      depth?: number,
      color?: Paint,
    ): void;
    drawRect(
      cb: CommandBuffer | Renderer,
      at: Vec2,
      size: Vec2,
      depth?: number,
      color?: Color,
      thickness?: number,
    ): void;
    drawRect(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      width: number,
      height: number,
      depth?: number,
      color?: Color,
      thickness?: number,
    ): void;
    fillEllipse(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      color?: Color,
      percent?: number,
      edges?: number,
    ): void;
    drawEllipse(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      color?: Color,
      thickness?: number,
      percent?: number,
      edges?: number,
    ): void;
    fillBox(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color): void;
    drawBox(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color, thickness?: number): void;
    fillCbox(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color): void;
    drawCbox(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color, thickness?: number): void;
    fillSphere(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      color?: Color,
      percentX?: number,
      percentY?: number,
      edges?: number,
    ): void;
    fillCylinder(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      color?: Color,
      percent?: number,
      edges?: number,
    ): void;
    fillPyramid(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color): void;
    drawPyramid(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      color?: Color,
      thickness?: number,
    ): void;
    fillQuad(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color): void;
    drawQuad(cb: CommandBuffer | Renderer, matrix: Mat4, color?: Color, thickness?: number): void;
    fillQuadRounded(
      cb: CommandBuffer | Renderer,
      p1: Vec4,
      p2: Vec4,
      p3: Vec4,
      p4: Vec4,
      radii?: Float32Array,
      color?: Color,
    ): void;
    drawQuadRounded(
      cb: CommandBuffer | Renderer,
      p1: Vec4,
      p2: Vec4,
      p3: Vec4,
      p4: Vec4,
      radii?: Float32Array,
      color?: Color,
      thickness?: number,
    ): void;
    fillRectRounded(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      radii?: Float32Array,
      shift?: number,
      color?: Paint,
    ): void;
    drawRectRounded(
      cb: CommandBuffer | Renderer,
      matrix: Mat4,
      radii?: Float32Array,
      shift?: number,
      color?: Color,
      thickness?: number,
    ): void;
    /**
     * UV-layout generation for the shared glyph atlas. Increments when atlas
     * growth or repack can make previously emitted text UVs stale. Callers that
     * cache vertex or UV data derived from text layout should record this value
     * and treat a mismatch on replay as a cache miss.
     */
    atlasEpoch(): number;
    drawText(
      cb: CommandBuffer | Renderer,
      at: Vec2,
      depth: number,
      text: string,
      opts?: {
        color?: Color;
        size?: number;
        pt?: number;
        fontId?: number;
        /**
         * Override line height in pixels. Falls back to the face's
         * metrics-derived line height when 0/unset.
         */
        lineHeight?: number;
        /**
         * OpenType feature flags. Each entry is either the 4-char tag
         * (defaults to value=1) or a `[tag, value]` pair (e.g. `["ss01", 2]`).
         */
        features?: ReadonlyArray<string | readonly [string, number]>;
        /**
         * OpenType variation axes. Keys are 4-char axis tags
         * (e.g. `wght`, `width`) mapped to numeric values.
         */
        variations?: { readonly [axisTag: string]: number };
        /**
         * Visual tab stop width in pixels (logical). When > 0, tab characters
         * advance the pen to the next multiple of `tabSize` from `tabOriginX`.
         * Zero (default) renders TAB as the underlying glyph.
         */
        tabSize?: number;
        /**
         * Tab origin x (logical px). Defaults to the draw origin x. Used by
         * editors painting indented lines that need a global stop alignment.
         */
        tabOriginX?: number;
        /**
         * When true, tab characters draw a faint horizontal arrow marker.
         */
        showWhitespace?: boolean;
      },
    ): [number, number, number, number];
    drawText(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      depth: number,
      text: string,
      pointSize?: number,
      color?: Color,
    ): [number, number, number, number];
    /**
     * Editor-grade text paint: paints N styled spans on the same baseline in
     * a single V8 trampoline. Spans are laid out left-to-right starting at
     * `(x, y)`. Each span may override color/size/font/weight/features.
     * Underline & strikethrough decorations are drawn after the glyph run.
     *
     * Returns `[width, height, advanceX, glyphCount]` (same shape as
     * `drawText`).
     */
    drawTextSpans(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      depth: number,
      spans: ReadonlyArray<{
        text: string;
        color?: Color;
        size?: number;
        fontId?: number;
        bold?: boolean;
        italic?: boolean;
        underline?: boolean;
        strikethrough?: boolean;
        features?: ReadonlyArray<string | readonly [string, number]>;
      }>,
      opts?: {
        tabSize?: number;
        tabOriginX?: number;
        lineHeight?: number;
        showWhitespace?: boolean;
        size?: number;
        color?: Color;
        fontId?: number;
      },
    ): [number, number, number, number];
    /**
     * Paint many axis-aligned rects in one trampoline. `rects` is a packed
     * Float32Array `[x0,y0,w0,h0, x1,y1,w1,h1, …]`. Used by editor selections
     * (one rect per visible line per cursor).
     */
    drawSelectionRects(
      cb: CommandBuffer | Renderer,
      rects: Float32Array,
      color?: Color,
      depth?: number,
    ): void;
    /**
     * Underline decoration for diagnostics, spell-check, hyperlinks. `y` is
     * the decoration baseline (typically the text baseline + 1–2 px).
     */
    drawDecorationUnderline(
      cb: CommandBuffer | Renderer,
      x1: number,
      x2: number,
      y: number,
      style: 'solid' | 'dashed' | 'dotted' | 'wavy',
      color?: Color,
      thickness?: number,
      depth?: number,
    ): void;
    /**
     * Batched draw_text. Collapses N V8 trampolines into one. Each run is
     * drawn as if `drawText(cb, run.x, run.y, run.depth ?? 0, run.text,
     * run.size ?? 16, run.color ?? <default>)` were called individually,
     * but without the per-call boundary cost.
     *
     * Does NOT support OpenType features / variations / fontId — keep using
     * the scalar drawText() for those.
     */
    drawTextRun(
      cb: CommandBuffer | Renderer,
      runs: ReadonlyArray<{
        x: number;
        y: number;
        text: string;
        size?: number;
        color?: Color;
        depth?: number;
      }>,
    ): void;
    /**
     * Emit a textured quad sampling from user-texture slot `slot` (0..3),
     * bound on the renderer via `Renderer.bindUserTexture(slot, source)`.
     * UV defaults to `[0,0,1,1]`; tint defaults to white. Used by surface
     * caching to draw a baked subtree as a single quad.
     */
    drawTextureQuad(
      cb: CommandBuffer | Renderer,
      slot: number,
      x: number,
      y: number,
      width: number,
      height: number,
      uv?: readonly [number, number, number, number],
      tint?: Color,
      depth?: number,
    ): void;
    linearGradient(p0: Vec2, p1: Vec2, stops: Float32Array): GradientPaint;
    radialGradient(center: Vec2, radius: number, stops: Float32Array): GradientPaint;
    conicGradient(center: Vec2, angle: number, stops: Float32Array): GradientPaint;
    fillPath(
      cb: CommandBuffer | Renderer,
      path: Path,
      paint?: Paint,
      fillRule?: 'nonzero' | 'evenodd',
      depth?: number,
    ): void;
    strokePath(
      cb: CommandBuffer | Renderer,
      path: Path,
      paint?: Paint,
      lineWidth?: number,
      lineJoin?: 'miter' | 'bevel' | 'round',
      lineCap?: 'butt' | 'square' | 'round',
      depth?: number,
    ): void;
    drawShadowRect(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      width: number,
      height: number,
      depth: number,
      color: Color,
      blur: number,
      spread: number,
      offsetX: number,
      offsetY: number,
      screenWidth: number,
      screenHeight: number,
    ): void;
    drawShadowRectRounded(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      width: number,
      height: number,
      depth: number,
      radii: Float32Array,
      color: Color,
      blur: number,
      spread: number,
      offsetX: number,
      offsetY: number,
      screenWidth: number,
      screenHeight: number,
    ): void;
    calcText(text: string, pointSize?: number): [number, number];
    wrapTextNative(
      text: string,
      pointSize?: number,
      letterSpacing?: number,
      maxWidth?: number,
      lineHeight?: number,
      breakWords?: boolean,
    ): {
      lines: string[];
      width: number;
      height: number;
      lineHeight: number;
      lineStartIndices: number[];
    } | null;
    xAtGlyphIndexNative(
      text: string,
      pointSize?: number,
      letterSpacing?: number,
      index?: number,
    ): number | null;
    glyphIndexAtNative(
      text: string,
      pointSize?: number,
      letterSpacing?: number,
      x?: number,
    ): number | null;
    blurRect(
      cb: CommandBuffer | Renderer,
      x: number,
      y: number,
      width: number,
      height: number,
      depth: number,
      color: Color,
      dispersion: number,
      screenWidth: number,
      screenHeight: number,
    ): void;
    blurQuad(
      cb: CommandBuffer | Renderer,
      p1: Vec4,
      p2: Vec4,
      p3: Vec4,
      p4: Vec4,
      color?: Color,
      dispersion?: number,
      screenWidth?: number,
      screenHeight?: number,
    ): void;
    drain(cb: CommandBuffer | Renderer, opcodes: Uint32Array, params: Float32Array): number;
  }

  namespace Print {
    function toPdf(
      path: string,
      pages: Array<{ widthPt: number; heightPt: number; commandBuffer: CommandBuffer }>,
    ): boolean;
  }

  const Primitives: PrimitivesNamespace;
  const Monitors: MonitorsNamespace;
  const App: AppNamespace;
}

type COSEAlgorithmIdentifier = number;
type AttestationConveyancePreference = 'none' | 'indirect' | 'direct' | 'enterprise';
type AuthenticatorAttachment = 'platform' | 'cross-platform';
type AuthenticatorTransport = 'usb' | 'nfc' | 'ble' | 'internal' | 'hybrid';
type ResidentKeyRequirement = 'discouraged' | 'preferred' | 'required';
type UserVerificationRequirement = 'discouraged' | 'preferred' | 'required';
type CredentialMediationRequirement = 'silent' | 'optional' | 'required' | 'conditional';
type PublicKeyCredentialType = 'public-key';
type BufferSource = ArrayBufferView | ArrayBuffer;

interface PublicKeyCredentialRpEntity {
  id?: string;
  name: string;
}

interface PublicKeyCredentialUserEntity {
  id: BufferSource;
  name: string;
  displayName: string;
}

interface PublicKeyCredentialParameters {
  type: PublicKeyCredentialType;
  alg: COSEAlgorithmIdentifier;
}

interface PublicKeyCredentialDescriptor {
  type: PublicKeyCredentialType;
  id: BufferSource;
  transports?: AuthenticatorTransport[];
}

interface AuthenticatorSelectionCriteria {
  authenticatorAttachment?: AuthenticatorAttachment;
  residentKey?: ResidentKeyRequirement;
  requireResidentKey?: boolean;
  userVerification?: UserVerificationRequirement;
}

interface AuthenticationExtensionsClientInputs {
  [k: string]: unknown;
}

interface AuthenticationExtensionsClientOutputs {
  [k: string]: unknown;
}

interface PublicKeyCredentialCreationOptions {
  rp: PublicKeyCredentialRpEntity;
  user: PublicKeyCredentialUserEntity;
  challenge: BufferSource;
  pubKeyCredParams: PublicKeyCredentialParameters[];
  timeout?: number;
  excludeCredentials?: PublicKeyCredentialDescriptor[];
  authenticatorSelection?: AuthenticatorSelectionCriteria;
  attestation?: AttestationConveyancePreference;
  extensions?: AuthenticationExtensionsClientInputs;
}

interface PublicKeyCredentialRequestOptions {
  challenge: BufferSource;
  timeout?: number;
  rpId?: string;
  allowCredentials?: PublicKeyCredentialDescriptor[];
  userVerification?: UserVerificationRequirement;
  extensions?: AuthenticationExtensionsClientInputs;
}

interface AuthenticatorResponse {
  readonly clientDataJSON: ArrayBuffer;
}

interface AuthenticatorAttestationResponse extends AuthenticatorResponse {
  readonly attestationObject: ArrayBuffer;
  getAuthenticatorData(): ArrayBuffer;
  getPublicKey(): ArrayBuffer | null;
  getPublicKeyAlgorithm(): COSEAlgorithmIdentifier;
  getTransports(): AuthenticatorTransport[];
}

interface AuthenticatorAssertionResponse extends AuthenticatorResponse {
  readonly authenticatorData: ArrayBuffer;
  readonly signature: ArrayBuffer;
  readonly userHandle: ArrayBuffer | null;
}

interface Credential {
  readonly id: string;
  readonly type: string;
}

interface PublicKeyCredential extends Credential {
  readonly type: 'public-key';
  readonly rawId: ArrayBuffer;
  readonly response: AuthenticatorAttestationResponse | AuthenticatorAssertionResponse;
  readonly authenticatorAttachment: AuthenticatorAttachment | null;
  getClientExtensionResults(): AuthenticationExtensionsClientOutputs;
}

declare const PublicKeyCredential: {
  prototype: PublicKeyCredential;
  isUserVerifyingPlatformAuthenticatorAvailable(): Promise<boolean>;
  isConditionalMediationAvailable(): Promise<boolean>;
};

interface CredentialCreationOptions {
  publicKey?: PublicKeyCredentialCreationOptions;
  signal?: AbortSignal;
}

interface CredentialRequestOptions {
  publicKey?: PublicKeyCredentialRequestOptions;
  signal?: AbortSignal;
  mediation?: CredentialMediationRequirement;
}

interface CredentialsContainer {
  create(options: CredentialCreationOptions): Promise<PublicKeyCredential | null>;
  get(options: CredentialRequestOptions): Promise<PublicKeyCredential | null>;
}

interface Navigator {
  readonly credentials: CredentialsContainer;
}

declare const navigator: Navigator;

declare module 'fxe' {
  export type Vec2 = FXE.Vec2;
  export type Vec3 = FXE.Vec3;
  export type Vec4 = FXE.Vec4;
  export type Mat4 = FXE.Mat4;
  export type Color = FXE.Color;
  export type GradientPaint = FXE.GradientPaint;
  export type Paint = FXE.Paint;
  export import VertexTopology = FXE.VertexTopology;
  export import CommandBuffer = FXE.CommandBuffer;
  export import TextDocument = FXE.TextDocument;
  export type TextDocumentEdit = FXE.TextDocumentEdit;
  export import Renderer = FXE.Renderer;
  export import OffscreenRenderer = FXE.OffscreenRenderer;
  export type VertexFormat = FXE.VertexFormat;
  export type VertexAttribute = FXE.VertexAttribute;
  export type PipelineDesc = FXE.PipelineDesc;
  export import Pipeline = FXE.Pipeline;
  export import Path = FXE.Path;
  export import Window = FXE.Window;
  export type BufferViews = FXE.BufferViews;
  export type Allocation = FXE.Allocation;
  export type RendererOptions = FXE.RendererOptions;
  export type OffscreenRendererOptions = FXE.OffscreenRendererOptions;
  export type Viewport = FXE.Viewport;
  export type WindowOptions = FXE.WindowOptions;
  export type WindowRunOptions = FXE.WindowRunOptions;
  export type ClipboardImage = FXE.ClipboardImage;
  export type DragImage = FXE.DragImage;
  export type CursorKind = FXE.CursorKind;
  export type TitleBarStyle = FXE.TitleBarStyle;
  export type VibrancyKind = FXE.VibrancyKind;
  export type WindowEventMap = FXE.WindowEventMap;
  export type WindowMessageEvent = FXE.WindowMessageEvent;
  export type ComposeEvent = FXE.ComposeEvent;
  export type MouseButtonEvent = FXE.MouseButtonEvent;
  export type DragEnterEvent = FXE.DragEnterEvent;
  export type DragOverEvent = FXE.DragOverEvent;
  export type DragLeaveEvent = FXE.DragLeaveEvent;
  export type WindowEventName = FXE.WindowEventName;
  export type WindowEventHandler<K extends FXE.WindowEventName> = FXE.WindowEventHandler<K>;
  export type WindowDisposer = FXE.WindowDisposer;
  export type MonitorInfo = FXE.MonitorInfo;
  export type MonitorsNamespace = FXE.MonitorsNamespace;
  export type AppRunOptions = FXE.AppRunOptions;
  export type AppSecondInstanceCallback = FXE.AppSecondInstanceCallback;
  export type AppOpenUrlCallback = FXE.AppOpenUrlCallback;
  export type AppOpenFileCallback = FXE.AppOpenFileCallback;
  export type UpdateChannel = FXE.UpdateChannel;
  export type AutoUpdateOptions = FXE.AutoUpdateOptions;
  export type AutoUpdateResult = FXE.AutoUpdateResult;
  export type InstallUpdateOptions = FXE.InstallUpdateOptions;
  export type InstallUpdateResult = FXE.InstallUpdateResult;
  export type UpdateNamespace = FXE.UpdateNamespace;
  export type BookmarkNamespace = FXE.BookmarkNamespace;
  export type PowerMonitor = FXE.PowerMonitor;
  export type SleepInhibitWhat = FXE.SleepInhibitWhat;
  export type PowerNamespace = FXE.PowerNamespace;
  export type RecentDocumentsNamespace = FXE.RecentDocumentsNamespace;
  export type CookieSameSite = FXE.CookieSameSite;
  export type Cookie = FXE.Cookie;
  export type CookieFilter = FXE.CookieFilter;
  export type CookiesNamespace = FXE.CookiesNamespace;
  export type SessionNamespace = FXE.SessionNamespace;
  export type CrashReporter = FXE.CrashReporter;
  export type AppNamespace = FXE.AppNamespace;
  export type PrimitivesNamespace = FXE.PrimitivesNamespace;
  export const Primitives: FXE.PrimitivesNamespace;
  export const Monitors: FXE.MonitorsNamespace;
  export const App: FXE.AppNamespace;
  export import Print = FXE.Print;
  export const powerMonitor: FXE.PowerMonitor;
}

interface Storage {
  readonly length: number;
  clear(): void;
  getItem(key: string): string | null;
  key(index: number): string | null;
  removeItem(key: string): void;
  setItem(key: string, value: string): void;
  [name: string]: unknown;
}

declare const CommandBuffer: typeof FXE.CommandBuffer;
declare const Renderer: typeof FXE.Renderer;
declare const OffscreenRenderer: typeof FXE.OffscreenRenderer;
declare const Window: typeof FXE.Window;
declare const Path: typeof FXE.Path;
declare const TextDocument: typeof FXE.TextDocument;
declare const Primitives: FXE.PrimitivesNamespace;
declare const Monitors: FXE.MonitorsNamespace;
declare const App: FXE.AppNamespace;
declare const Print: typeof FXE.Print;
declare const powerMonitor: FXE.PowerMonitor;

declare namespace Layout {
  type Length = number | `${number}%` | 'auto';
  interface Constraint {
    width?: number;
    height?: number;
  }
  interface Style {
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
  type MeasureDescriptor =
    | { kind: 'text'; text: string; fontSize: number }
    | { kind: 'image'; width: number; height: number }
    | { kind: 'js'; fn: (c: Constraint) => { width: number; height: number } };
  interface NodeDescriptor {
    style?: Style;
    children?: NodeDescriptor[];
    measure?: MeasureDescriptor;
  }
  interface Result {
    x: number;
    y: number;
    width: number;
    height: number;
    paddingLeft: number;
    paddingTop: number;
    paddingRight: number;
    paddingBottom: number;
    children: Result[];
  }
  function solve(root: NodeDescriptor, constraint?: Constraint): Result;
}
declare var localStorage: Storage;
declare var sessionStorage: Storage;

declare const console: {
  log(...values: unknown[]): void;
  info(...values: unknown[]): void;
  warn(...values: unknown[]): void;
  error(...values: unknown[]): void;
  debug(...values: unknown[]): void;
};

declare const performance: {
  now(): number;
  timeline: PerformanceTimeline;
};

// === io begin ===

interface FxeStats {
  size: number;
  isFile: boolean;
  isDirectory: boolean;
  isSymbolicLink?: boolean;
  mtimeMs: number;
  atimeMs: number;
  ctimeMs: number;
}
interface FxeDirent {
  name: string;
  isFile: boolean;
  isDirectory: boolean;
}
interface FxeCpOpts {
  recursive?: boolean;
  dereference?: boolean;
  signal?: AbortSignal;
}
interface FxeGlobOpts {
  cwd?: string;
}
interface FxeLockOpts {
  exclusive?: boolean;
  nonBlocking?: boolean;
}
interface FxeReadOpts {
  encoding?: 'utf8' | 'utf-8' | null;
  signal?: AbortSignal;
}
interface FxeMkdirOpts {
  recursive?: boolean;
  signal?: AbortSignal;
}
interface FxeRmOpts {
  recursive?: boolean;
  force?: boolean;
  signal?: AbortSignal;
}
interface FxeReaddirOpts {
  withFileTypes?: boolean;
  signal?: AbortSignal;
}
interface FxeFsWatcher {
  close(): void;
  on(event: 'change', listener: (eventType: 'change' | 'rename', filename: string) => void): this;
  on(event: 'close', listener: () => void): this;
}
interface FxeWatchOpts {
  interval?: number;
  recursive?: boolean;
}
type FxeOpenFlags = 'r' | 'r+' | 'w' | 'w+' | 'a' | 'a+' | number;
interface FxeReadResult<TBuffer extends ArrayBufferView = Uint8Array> {
  bytesRead: number;
  buffer: TBuffer;
}
interface FxeWriteResult<TBuffer extends ArrayBufferView = Uint8Array> {
  bytesWritten: number;
  buffer: TBuffer;
}
interface FxeReadableStream {
  on(event: 'data', listener: (chunk: Uint8Array | string) => void): this;
  on(event: 'end' | 'close' | 'open', listener: (...args: any[]) => void): this;
  on(event: 'error', listener: (error: Error) => void): this;
  pipe<TDest extends { write(chunk: any): any; end?: () => any }>(dest: TDest): TDest;
  pause(): this;
  resume(): this;
  close(callback?: () => void): void;
}
interface FxeWritableStream {
  on(event: 'finish' | 'close' | 'open' | 'drain', listener: (...args: any[]) => void): this;
  on(event: 'error', listener: (error: Error) => void): this;
  write(
    chunk: string | ArrayBufferView | ArrayBuffer,
    encoding?: string,
    callback?: (err?: Error | null) => void,
  ): boolean;
  end(
    chunk?: string | ArrayBufferView | ArrayBuffer,
    encoding?: string,
    callback?: (err?: Error | null) => void,
  ): this;
  close(callback?: () => void): this;
}

declare const fs: {
  readFileSync(path: string, opts: 'utf8' | 'utf-8' | { encoding: 'utf8' | 'utf-8' }): string;
  readFileSync(path: string, opts?: FxeReadOpts | null): Uint8Array;
  writeFileSync(path: string, data: string | ArrayBufferView | ArrayBuffer): void;
  appendFileSync(path: string, data: string | ArrayBufferView | ArrayBuffer): void;
  existsSync(path: string): boolean;
  statSync(path: string): FxeStats;
  readdirSync(path: string, opts: { withFileTypes: true }): FxeDirent[];
  readdirSync(path: string, opts?: FxeReaddirOpts | null): string[];
  mkdirSync(path: string, opts?: FxeMkdirOpts): void;
  rmSync(path: string, opts?: FxeRmOpts): void;
  renameSync(from: string, to: string): void;
  realpathSync(path: string): string;
  copyFileSync(src: string, dest: string, mode?: number): void;
  cpSync(src: string, dest: string, opts?: FxeCpOpts): void;
  symlinkSync(target: string, path: string, type?: 'file' | 'dir' | 'junction'): void;
  readlinkSync(path: string): string;
  linkSync(existingPath: string, newPath: string): void;
  lstatSync(path: string): FxeStats;
  accessSync(path: string, mode?: number): void;
  chmodSync(path: string, mode: number): void;
  chownSync(path: string, uid: number, gid: number): void;
  lchmodSync(path: string, mode: number): void;
  utimesSync(path: string, atime: number | Date, mtime: number | Date): void;
  lutimesSync(path: string, atime: number | Date, mtime: number | Date): void;
  globSync(pattern: string, opts?: FxeGlobOpts): string[];
  writeFileAtomicSync(path: string, data: string | ArrayBufferView | ArrayBuffer): void;
  lockSync(fd: number, opts?: FxeLockOpts): void;
  unlockSync(fd: number): void;
  openSync(path: string, flags?: FxeOpenFlags, mode?: number): number;
  readSync(
    fd: number,
    buffer: ArrayBufferView,
    offset?: number,
    length?: number,
    position?: number | null,
  ): number;
  writeSync(
    fd: number,
    buffer: string | ArrayBufferView | ArrayBuffer,
    offset?: number,
    length?: number,
    position?: number | null,
  ): number;
  closeSync(fd: number): void;
  fstatSync(fd: number): FxeStats;
  ftruncateSync(fd: number, len?: number): void;
  fdatasyncSync(fd: number): void;
  readFile(path: string, opts: 'utf8' | 'utf-8' | { encoding: 'utf8' | 'utf-8' }): Promise<string>;
  readFile(path: string, opts?: FxeReadOpts | null): Promise<Uint8Array>;
  readFile(
    path: string,
    encoding: string,
    callback: (err: Error | null, data: string) => void,
  ): void;
  writeFile(
    path: string,
    data: string | ArrayBufferView | ArrayBuffer,
    opts?: { signal?: AbortSignal } | null,
  ): Promise<void>;
  writeFile(
    path: string,
    data: string | ArrayBufferView | ArrayBuffer,
    callback: (err: Error | null) => void,
  ): void;
  appendFile(
    path: string,
    data: string | ArrayBufferView | ArrayBuffer,
    opts?: { signal?: AbortSignal } | null,
  ): Promise<void>;
  appendFile(
    path: string,
    data: string | ArrayBufferView | ArrayBuffer,
    callback: (err: Error | null) => void,
  ): void;
  stat(path: string, opts?: { signal?: AbortSignal } | null): Promise<FxeStats>;
  readdir(path: string, opts: { withFileTypes: true }): Promise<FxeDirent[]>;
  readdir(path: string, opts?: FxeReaddirOpts | null): Promise<string[]>;
  mkdir(path: string, opts?: FxeMkdirOpts): Promise<void>;
  rm(path: string, opts?: FxeRmOpts): Promise<void>;
  rename(from: string, to: string, opts?: { signal?: AbortSignal } | null): Promise<void>;
  realpath(path: string, opts?: { signal?: AbortSignal } | null): Promise<string>;
  exists(path: string, opts?: { signal?: AbortSignal } | null): Promise<boolean>;
  copyFile(src: string, dest: string, mode?: number): Promise<void>;
  cp(src: string, dest: string, opts?: FxeCpOpts): Promise<void>;
  symlink(target: string, path: string, type?: 'file' | 'dir' | 'junction'): Promise<void>;
  readlink(path: string): Promise<string>;
  link(existingPath: string, newPath: string): Promise<void>;
  lstat(path: string): Promise<FxeStats>;
  access(path: string, mode?: number): Promise<void>;
  chmod(path: string, mode: number): Promise<void>;
  chown(path: string, uid: number, gid: number): Promise<void>;
  lchmod(path: string, mode: number): Promise<void>;
  utimes(path: string, atime: number | Date, mtime: number | Date): Promise<void>;
  lutimes(path: string, atime: number | Date, mtime: number | Date): Promise<void>;
  glob(pattern: string, opts?: FxeGlobOpts): AsyncIterableIterator<string>;
  writeFileAtomic(path: string, data: string | ArrayBufferView | ArrayBuffer): Promise<void>;
  lock(fd: number, opts?: FxeLockOpts): Promise<void>;
  unlock(fd: number): Promise<void>;
  open(path: string, flags?: FxeOpenFlags, mode?: number): Promise<number>;
  open(
    path: string,
    flags: FxeOpenFlags,
    callback: (err: Error | null, fd: number) => void,
  ): Promise<number>;
  open(
    path: string,
    flags: FxeOpenFlags,
    mode: number,
    callback: (err: Error | null, fd: number) => void,
  ): Promise<number>;
  read<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset: number,
    length: number,
    position: number | null,
  ): Promise<FxeReadResult<TBuffer>>;
  read<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset: number,
    length: number,
    position: number | null,
    callback: (err: Error | null, bytesRead: number, buffer: TBuffer) => void,
  ): Promise<FxeReadResult<TBuffer>>;
  write<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset?: number,
    length?: number,
    position?: number | null,
  ): Promise<FxeWriteResult<TBuffer>>;
  write<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset: number,
    length: number,
    position: number | null,
    callback: (err: Error | null, bytesWritten: number, buffer: TBuffer) => void,
  ): Promise<FxeWriteResult<TBuffer>>;
  close(fd: number, callback?: (err?: Error | null) => void): Promise<void>;
  fstat(fd: number, callback?: (err: Error | null, stats: FxeStats) => void): Promise<FxeStats>;
  ftruncate(fd: number, len?: number, callback?: (err?: Error | null) => void): Promise<void>;
  fdatasync(fd: number, callback?: (err?: Error | null) => void): Promise<void>;
  watch(
    path: string,
    listener: (eventType: 'change' | 'rename', filename: string) => void,
  ): FxeFsWatcher;
  watch(
    path: string,
    opts: FxeWatchOpts,
    listener: (eventType: 'change' | 'rename', filename: string) => void,
  ): FxeFsWatcher;
  watch(path: string, opts: FxeWatchOpts): FxeFsWatcher;
  promises: typeof fs;
  createReadStream(
    path: string,
    opts?: {
      flags?: FxeOpenFlags;
      fd?: number;
      start?: number;
      end?: number;
      highWaterMark?: number;
      encoding?: string;
      autoClose?: boolean;
    },
  ): FxeReadableStream;
  createWriteStream(
    path: string,
    opts?: {
      flags?: FxeOpenFlags;
      fd?: number;
      start?: number;
      mode?: number;
      autoClose?: boolean;
    },
  ): FxeWritableStream;
};

declare module 'node:fs' {
  export default fs;
  export const promises: typeof import('node:fs/promises');
  export const readFileSync: typeof fs.readFileSync;
  export const writeFileSync: typeof fs.writeFileSync;
  export const appendFileSync: typeof fs.appendFileSync;
  export const existsSync: typeof fs.existsSync;
  export const statSync: typeof fs.statSync;
  export const lstatSync: typeof fs.lstatSync;
  export const readdirSync: typeof fs.readdirSync;
  export const mkdirSync: typeof fs.mkdirSync;
  export const rmSync: typeof fs.rmSync;
  export const renameSync: typeof fs.renameSync;
  export const realpathSync: typeof fs.realpathSync;
  export const copyFileSync: typeof fs.copyFileSync;
  export const cpSync: typeof fs.cpSync;
  export const symlinkSync: typeof fs.symlinkSync;
  export const readlinkSync: typeof fs.readlinkSync;
  export const linkSync: typeof fs.linkSync;
  export const accessSync: typeof fs.accessSync;
  export const chmodSync: typeof fs.chmodSync;
  export const chownSync: typeof fs.chownSync;
  export const lchmodSync: typeof fs.lchmodSync;
  export const utimesSync: typeof fs.utimesSync;
  export const lutimesSync: typeof fs.lutimesSync;
  export const globSync: typeof fs.globSync;
  export const writeFileAtomicSync: typeof fs.writeFileAtomicSync;
  export const lockSync: typeof fs.lockSync;
  export const unlockSync: typeof fs.unlockSync;
  export const openSync: typeof fs.openSync;
  export const readSync: typeof fs.readSync;
  export const writeSync: typeof fs.writeSync;
  export const closeSync: typeof fs.closeSync;
  export const fstatSync: typeof fs.fstatSync;
  export const ftruncateSync: typeof fs.ftruncateSync;
  export const fdatasyncSync: typeof fs.fdatasyncSync;
  export const readFile: typeof fs.readFile;
  export const writeFile: typeof fs.writeFile;
  export const appendFile: typeof fs.appendFile;
  export const stat: typeof fs.stat;
  export const lstat: typeof fs.lstat;
  export const readdir: typeof fs.readdir;
  export const mkdir: typeof fs.mkdir;
  export const rm: typeof fs.rm;
  export const rename: typeof fs.rename;
  export const realpath: typeof fs.realpath;
  export const exists: typeof fs.exists;
  export const copyFile: typeof fs.copyFile;
  export const cp: typeof fs.cp;
  export const symlink: typeof fs.symlink;
  export const readlink: typeof fs.readlink;
  export const link: typeof fs.link;
  export const access: typeof fs.access;
  export const chmod: typeof fs.chmod;
  export const chown: typeof fs.chown;
  export const lchmod: typeof fs.lchmod;
  export const utimes: typeof fs.utimes;
  export const lutimes: typeof fs.lutimes;
  export const glob: typeof fs.glob;
  export const writeFileAtomic: typeof fs.writeFileAtomic;
  export const lock: typeof fs.lock;
  export const unlock: typeof fs.unlock;
  export const watch: typeof fs.watch;
  export const open: typeof fs.open;
  export const read: typeof fs.read;
  export const write: typeof fs.write;
  export const close: typeof fs.close;
  export const fstat: typeof fs.fstat;
  export const ftruncate: typeof fs.ftruncate;
  export const fdatasync: typeof fs.fdatasync;
  export const createReadStream: typeof fs.createReadStream;
  export const createWriteStream: typeof fs.createWriteStream;
}

declare module 'node:fs/promises' {
  export const readFile: typeof fs.readFile;
  export const writeFile: typeof fs.writeFile;
  export const appendFile: typeof fs.appendFile;
  export const stat: typeof fs.stat;
  export const lstat: typeof fs.lstat;
  export const readdir: typeof fs.readdir;
  export const mkdir: typeof fs.mkdir;
  export const rm: typeof fs.rm;
  export const rename: typeof fs.rename;
  export const realpath: typeof fs.realpath;
  export const exists: typeof fs.exists;
  export const copyFile: typeof fs.copyFile;
  export const cp: typeof fs.cp;
  export const symlink: typeof fs.symlink;
  export const readlink: typeof fs.readlink;
  export const link: typeof fs.link;
  export const access: typeof fs.access;
  export const chmod: typeof fs.chmod;
  export const chown: typeof fs.chown;
  export const lchmod: typeof fs.lchmod;
  export const utimes: typeof fs.utimes;
  export const lutimes: typeof fs.lutimes;
  export const glob: typeof fs.glob;
  export const writeFileAtomic: typeof fs.writeFileAtomic;
  export const lock: typeof fs.lock;
  export const unlock: typeof fs.unlock;
  export const open: typeof fs.open;
  export const read: typeof fs.read;
  export const write: typeof fs.write;
  export const close: typeof fs.close;
  export const fstat: typeof fs.fstat;
  export const ftruncate: typeof fs.ftruncate;
  export const fdatasync: typeof fs.fdatasync;
}

declare const path: {
  join(...parts: string[]): string;
  resolve(...parts: string[]): string;
  dirname(p: string): string;
  basename(p: string, ext?: string): string;
  extname(p: string): string;
  relative(from: string, to: string): string;
  normalize(p: string): string;
  isAbsolute(p: string): boolean;
  readonly sep: string;
  readonly delimiter: string;
};

type FxeProcessEvent = 'exit' | 'unhandledRejection' | 'rejectionHandled';
type FxeStdinEvent = 'data' | 'end' | 'error';

type FxeStdin = {
  readonly fd: 0;
  readonly isTTY: boolean;
  readonly readable?: boolean;
  readonly readableEncoding?: string | null;
  setEncoding(enc: string): FxeStdin;
  on(event: FxeStdinEvent, handler: (...args: any[]) => void): FxeStdin;
  once(event: FxeStdinEvent, handler: (...args: any[]) => void): FxeStdin;
  off(event: FxeStdinEvent, handler: (...args: any[]) => void): FxeStdin;
  removeListener(event: FxeStdinEvent, handler: (...args: any[]) => void): FxeStdin;
  resume(): FxeStdin;
  pause(): FxeStdin;
  [Symbol.asyncIterator]?: () => AsyncIterator<unknown>;
};

type FxeHrtime = {
  (previous?: [number, number]): [number, number];
  bigint(): bigint;
};
declare const process: {
  readonly argv: string[];
  env: { [k: string]: string | undefined } & Record<string, string | undefined>;
  cwd(): string;
  chdir(p: string): void;
  readonly platform: 'darwin' | 'linux' | 'win32' | string;
  readonly arch: 'arm64' | 'x64' | 'ia32' | string;
  readonly pid: number;
  exit(code?: number): void;
  kill(pid: number, signal?: string | number): boolean;
  umask(mask?: number): number;
  hrtime: FxeHrtime;
  readonly release: { name: string; [key: string]: string | undefined };
  readonly versions: { fxe: string; v8: string; dawn: string };
  readonly stdin: FxeStdin;
  readonly stdout: { write(s: string | Uint8Array): boolean };
  readonly stderr: { write(s: string | Uint8Array): boolean };
  on(event: FxeProcessEvent, handler: (...args: any[]) => void): typeof process;
  off(event: FxeProcessEvent, handler: (...args: any[]) => void): typeof process;
  nextTick(fn: (...args: any[]) => void, ...args: any[]): void;
};

declare function setTimeout(fn: (...args: any[]) => void, ms?: number, ...args: any[]): number;
declare function setInterval(fn: (...args: any[]) => void, ms?: number, ...args: any[]): number;
declare function setImmediate(fn: (...args: any[]) => void, ...args: any[]): number;
declare function clearTimeout(id: number): void;
declare function clearInterval(id: number): void;
declare function queueMicrotask(fn: () => void): void;
declare function requestAnimationFrame(fn: (timeMs: number) => void): number;
declare function cancelAnimationFrame(id: number): void;

// === io end ===

// === reactive begin ===
interface RenderStatsSnapshot {
  verticesSubmitted: number;
  indicesSubmitted: number;
  queueCalls: number;
  cacheHits: number;
  cacheMisses: number;
  rebuilds: number;
  frames: number;
}

interface RenderStatsNamespace {
  snapshot(): RenderStatsSnapshot;
  reset(): void;
  recordCacheHit(): void;
  recordCacheMiss(): void;
  recordRebuild(): void;
  recordQueueCall(): void;
  beginFrame(): void;
}

declare namespace FXE {
  interface CommandBuffer {
    clone(): CommandBuffer;
    isEmpty(): boolean;
  }
  const RenderStats: RenderStatsNamespace;
}
declare module 'fxe' {
  export const RenderStats: FXE.RenderStatsNamespace;
}
declare const RenderStats: FXE.RenderStatsNamespace;
// === reactive end ===

// === assets begin ===
declare module 'fxe' {
  export interface ImageHandle {
    width(): number;
    height(): number;
    bytes(): Uint8Array;
    dispose(): void;
  }
  export interface ImageNamespace {
    load(path: string): ImageHandle;
    loadAsync(path: string): Promise<ImageHandle>;
    fromBytes(bytes: Uint8Array): ImageHandle;
    fromBytes(bytes: Uint8Array, width: number, height: number): ImageHandle;
  }
  export const Image: ImageNamespace;

  export interface SpriteResolved {
    textureId: number;
    u0: number;
    v0: number;
    u1: number;
    v1: number;
    width: number;
    height: number;
  }
  export class Spritesheet {
    constructor();
    add(image: ImageHandle, rect?: [number, number, number, number]): number;
    addAnimated(images: ImageHandle[], delaysMs: number[]): number;
    resolve(spriteId: number, timeMs?: number): SpriteResolved;
    dispose(): void;
  }

  export interface FontNamespace {
    load(path: string, sizePx: number): number;
    builtin(name: 'default'): number;
    dispose(fontId: number): void;
    /**
     * Discover a system font by family + style. Returns a font id usable
     * with `Primitives.drawText({ fontId })`. The platform font discovery
     * backend is fixed at build time (CoreText on macOS, Fontconfig on
     * Linux, directory scan on Windows).
     */
    system?(
      family: string,
      opts?: { style?: 'regular' | 'bold' | 'italic' | 'bold-italic'; sizePx?: number },
    ): number;
    /**
     * Measure text using the same shaper that `Primitives.drawText` uses,
     * including ligatures and kerning. Returns `[width, height]` in pixels.
     */
    measureText?(
      text: string,
      opts?: {
        fontId?: number;
        sizePx?: number;
        features?: ReadonlyArray<string | readonly [string, number]>;
      },
    ): [number, number];
  }
  export const Font: FontNamespace;
}

interface ImageHandle {
  width(): number;
  height(): number;
  bytes(): Uint8Array;
  dispose(): void;
}
interface ImageNamespace {
  load(path: string): ImageHandle;
  loadAsync(path: string): Promise<ImageHandle>;
  fromBytes(bytes: Uint8Array): ImageHandle;
  fromBytes(bytes: Uint8Array, width: number, height: number): ImageHandle;
}
declare const Image: ImageNamespace;

interface SpriteResolved {
  textureId: number;
  u0: number;
  v0: number;
  u1: number;
  v1: number;
  width: number;
  height: number;
}
declare class Spritesheet {
  constructor();
  add(image: ImageHandle, rect?: [number, number, number, number]): number;
  addAnimated(images: ImageHandle[], delaysMs: number[]): number;
  resolve(spriteId: number, timeMs?: number): SpriteResolved;
  dispose(): void;
}

interface FontNamespace {
  load(path: string, sizePx: number): number;
  builtin(name: 'default'): number;
  dispose(fontId: number): void;
  system?(
    family: string,
    opts?: { style?: 'regular' | 'bold' | 'italic' | 'bold-italic'; sizePx?: number },
  ): number;
  measureText?(
    text: string,
    opts?: {
      fontId?: number;
      sizePx?: number;
      features?: ReadonlyArray<string | readonly [string, number]>;
    },
  ): [number, number];
}
declare const Font: FontNamespace;
// === assets end ===

// === os begin ===
interface CrashReporter {
  start(options: {
    productName: string;
    productVersion?: string;
    submitURL?: string;
    crashDir?: string;
    uploadToServer?: boolean;
  }): boolean;
  listDumps(): string[];
  getLastDumpPath(): string | null;
}

type AppSecondInstanceCallback = (argv: string[], cwd: string) => void;
type AppOpenUrlCallback = (url: string) => void;
type AppOpenFileCallback = (path: string) => void;

interface PowerNamespace {
  inhibitSleep(options: { reason: string; what: FXE.SleepInhibitWhat }): () => void;
}

interface RecentDocumentsNamespace {
  add(path: string): boolean;
  list(): string[];
  clear(): boolean;
}

interface BookmarkNamespace {
  persist(path: string): string;
  resolve(blob: string): { path: string; isStale: boolean };
  startAccessing(blob: string): boolean;
  stopAccessing(blob: string): void;
}

type CookieSameSite = FXE.CookieSameSite;
interface Cookie extends FXE.Cookie {}
interface CookieFilter extends FXE.CookieFilter {}
interface CookiesNamespace extends FXE.CookiesNamespace {}
interface SessionNamespace extends FXE.SessionNamespace {}

interface SystemNamespace {
  /** OS preference: user has requested reduced motion. */
  prefersReducedMotion(): boolean;
  /** OS preference: high-contrast / increased-contrast accessibility setting. */
  prefersHighContrast(): boolean;
  /** OS text-scale factor. 1.0 = default. */
  fontScale(): number;
  /** OS color scheme: 'light' | 'dark' | 'no-preference'. */
  colorScheme(): 'light' | 'dark' | 'no-preference';
  /** Lowercase RRGGBB accent color hex string, or "" if unknown. */
  accentColor(): string;
  /** Platform identifier: 'macos' | 'win' | 'linux' | 'other'. */
  platform(): 'macos' | 'win' | 'linux' | 'other';
  /**
   * Event surface for OS preference changes. v1 uses a 5-second poll; native
   * NSNotificationCenter/WM_SETTINGCHANGE/gsettings hooks land later.
   * Runtime polling driver pending follow-up implementation.
   */
  on(
    event: 'change',
    cb: (ev: { kind: string; previous: unknown; current: unknown }) => void,
  ): () => void;
}

interface AppNamespace {
  getName(): string;
  getVersion(): string;
  getPath(kind: 'userData' | 'documents' | 'downloads' | 'temp' | 'home'): string;
  openDevTools(window?: InstanceType<typeof Window>): InstanceType<typeof Window> | null;
  requestSingleInstanceLock(appId?: string): boolean;
  on(event: 'second-instance', cb: AppSecondInstanceCallback): () => void;
  on(event: 'open-url', cb: AppOpenUrlCallback): () => void;
  on(event: 'open-file', cb: AppOpenFileCallback): () => void;
  setAsDefaultProtocolClient(scheme: string): boolean;
  setAsDefaultFileHandler(ext: string): boolean;
  setBadgeCount(n: number): void;
  whenReady(): Promise<void>;
  relaunch(opts?: { installUpdate?: boolean }): void;
  checkForUpdates(url: string, opts?: FXE.AutoUpdateOptions): Promise<FXE.AutoUpdateResult>;
  installUpdate(opts?: FXE.InstallUpdateOptions): Promise<FXE.InstallUpdateResult>;
  update: FXE.UpdateNamespace;
  bookmark: FXE.BookmarkNamespace;
  powerMonitor: FXE.PowerMonitor;
  power: FXE.PowerNamespace;
  recentDocuments: FXE.RecentDocumentsNamespace;
  session: FXE.SessionNamespace;
  crashReporter: FXE.CrashReporter;
  system: SystemNamespace;
  /** @internal Raw accessibility snapshot bridge for native providers. */
  __fxeUpdateAccessibilityTree(windowId: number, snapshotJson: string): void;
}
declare namespace FXE {
  type AppSecondInstanceCallback = (argv: string[], cwd: string) => void;
  type AppOpenUrlCallback = (url: string) => void;
  type AppOpenFileCallback = (path: string) => void;

  interface PowerNamespace {
    inhibitSleep(options: { reason: string; what: SleepInhibitWhat }): () => void;
  }

  interface RecentDocumentsNamespace {
    add(path: string): boolean;
    list(): string[];
    clear(): boolean;
  }

  interface BookmarkNamespace {
    persist(path: string): string;
    resolve(blob: string): { path: string; isStale: boolean };
    startAccessing(blob: string): boolean;
    stopAccessing(blob: string): void;
  }

  interface Cookie {
    name: string;
    value: string;
    url?: string;
    domain?: string;
    path?: string;
    expires?: number;
    secure?: boolean;
    httpOnly?: boolean;
    hostOnly?: boolean;
    sameSite?: CookieSameSite;
  }
  interface CookieFilter {
    name?: string;
    domain?: string;
    url?: string;
  }
  interface CookiesNamespace {
    getAll(filter?: CookieFilter): Cookie[];
    set(cookie: Cookie): void;
    remove(name: string, url: string): void;
    clear(): void;
    persist(path: string): void;
  }
  interface SessionNamespace {
    cookies: CookiesNamespace;
  }
  interface SystemNamespace {
    prefersReducedMotion(): boolean;
    prefersHighContrast(): boolean;
    fontScale(): number;
    colorScheme(): 'light' | 'dark' | 'no-preference';
    accentColor(): string;
    platform(): 'macos' | 'win' | 'linux' | 'other';
    /**
     * Event surface for OS preference changes. v1 uses a 5-second poll; native
     * NSNotificationCenter/WM_SETTINGCHANGE/gsettings hooks land later.
     */
    on(
      event: 'change',
      cb: (ev: { kind: string; previous: unknown; current: unknown }) => void,
    ): () => void;
  }

  interface AppNamespace {
    getName(): string;
    getVersion(): string;
    getPath(kind: 'userData' | 'documents' | 'downloads' | 'temp' | 'home'): string;
    openDevTools(window?: Window): Window | null;
    requestSingleInstanceLock(appId?: string): boolean;
    on(event: 'second-instance', cb: AppSecondInstanceCallback): () => void;
    on(event: 'open-url', cb: AppOpenUrlCallback): () => void;
    on(event: 'open-file', cb: AppOpenFileCallback): () => void;
    setAsDefaultProtocolClient(scheme: string): boolean;
    setAsDefaultFileHandler(ext: string): boolean;
    setBadgeCount(n: number): void;
    whenReady(): Promise<void>;
    relaunch(opts?: { installUpdate?: boolean }): void;
    crashReport: CrashReporter;
    openWindow(options?: WindowOptions): Window;
    checkForUpdates(url: string, opts?: AutoUpdateOptions): Promise<AutoUpdateResult>;
    installUpdate(opts?: InstallUpdateOptions): Promise<InstallUpdateResult>;
    update: UpdateNamespace;
    bookmark: BookmarkNamespace;
    powerMonitor: PowerMonitor;
    power: PowerNamespace;
    recentDocuments: RecentDocumentsNamespace;
    session: SessionNamespace;
    crashReporter: CrashReporter;
    system: SystemNamespace;
    /** @internal Raw accessibility snapshot bridge for native providers. */
    __fxeUpdateAccessibilityTree(windowId: number, snapshotJson: string): void;
  }
}

declare const shell: {
  openExternal(url: string): boolean;
  showItemInFolder(path: string): boolean;
  beep(): void;
  trashItem(path: string): boolean;
};

interface DialogFilter {
  name: string;
  extensions: string[];
}
interface OpenDialogOptions {
  title?: string;
  defaultPath?: string;
  filters?: DialogFilter[];
  multiple?: boolean;
  directories?: boolean;
}
interface OpenDialogResult {
  canceled: boolean;
  filePaths: string[];
}
interface SaveDialogOptions {
  title?: string;
  defaultPath?: string;
  filters?: DialogFilter[];
}
interface SaveDialogResult {
  canceled: boolean;
  filePath?: string;
}
interface MessageBoxOptions {
  title?: string;
  message?: string;
  detail?: string;
  buttons?: string[];
  type?: 'info' | 'warning' | 'error' | 'question';
}
interface MessageBoxResult {
  response: number;
}
declare const dialog: {
  showOpenDialog(opts: OpenDialogOptions): Promise<OpenDialogResult>;
  showSaveDialog(opts: SaveDialogOptions): Promise<SaveDialogResult>;
  showMessageBox(opts: MessageBoxOptions): Promise<MessageBoxResult>;
};

interface NotificationAction {
  id: string;
  title: string;
  kind?: 'button' | 'input';
}

interface NotificationActionEvent {
  id: string;
  input?: string;
}

interface NotificationOptions {
  title?: string;
  body?: string;
  icon?: string;
  image?: string;
  imagePath?: string;
  attachmentPath?: string;
  actions?: NotificationAction[];
  onAction?: (event: NotificationActionEvent) => void;
}
declare class Notification {
  constructor(opts: NotificationOptions);
  show(): Promise<void>;
  static readonly permission: 'granted' | 'denied' | 'default';
  static requestPermission(): Promise<'granted' | 'denied' | 'default'>;
}

interface MenuItem {
  id?: string;
  label?: string;
  accelerator?: string;
  enabled?: boolean;
  checked?: boolean;
  type?: 'normal' | 'separator' | 'checkbox' | 'submenu';
  submenu?: MenuItem[];
}
interface MenuItemHandle {
  id: string;
  setLabel(label: string): void;
  setEnabled(enabled: boolean): void;
  setChecked(checked: boolean): void;
  setVisible(visible: boolean): void;
  setAccelerator(accelerator: string): void;
}

declare const Menu: {
  setApplicationMenu(items: MenuItem[]): void;
  popup(items: MenuItem[], x: number, y: number): Promise<string | null>;
  updateItem(
    id: string,
    patch: {
      label?: string;
      enabled?: boolean;
      checked?: boolean;
      visible?: boolean;
      accelerator?: string;
    },
  ): boolean;
  findItem(id: string): MenuItemHandle | null;
  /**
   * Register a callback fired when the user activates an item in the
   * application menu (set via `setApplicationMenu`). The callback receives
   * the activated `MenuItem.id`. Single-slot: re-registering replaces the
   * previous handler. Pass `null` to clear.
   */
  onCommand(handler: ((id: string) => void) | null): void;
};

type TrayEvent = 'click' | 'right-click' | 'double-click';
declare class Tray {
  constructor(iconPath: string, tooltip?: string);
  setMenu(items: MenuItem[]): void;
  setImage(iconPath: string): boolean;
  setTitle(text: string): boolean;
  setToolTip(text: string): boolean;
  on(event: TrayEvent, cb: () => void): () => void;
  destroy(): void;
}

declare const globalShortcut: {
  register(accelerator: string, fn: () => void): boolean;
  unregister(accelerator: string): void;
  unregisterAll(): void;
};
// === os end ===

// === net begin ===
type BlobPart = string | ArrayBuffer | ArrayBufferView | Blob;

interface BlobPropertyBag {
  type?: string;
}

interface ReadableStreamReadResult<T> {
  value?: T;
  done: boolean;
}

interface ReadableStreamDefaultReader<T = Uint8Array> {
  read(): Promise<ReadableStreamReadResult<T>>;
}

declare class ReadableStream<T = Uint8Array> {
  constructor(underlyingSource?: {
    start?: (controller: ReadableStreamDefaultController<T>) => void | Promise<void>;
    pull?: (controller: ReadableStreamDefaultController<T>) => void | Promise<void>;
    cancel?: (reason?: unknown) => void | Promise<void>;
    type?: 'bytes';
  });
  getReader(): ReadableStreamDefaultReader<T>;
  cancel(reason?: unknown): Promise<void>;
}
interface ReadableStreamDefaultController<T> {
  enqueue(chunk: T): void;
  close(): void;
  error(reason?: unknown): void;
  readonly desiredSize: number | null;
}

declare class Blob {
  constructor(blobParts?: BlobPart[], options?: BlobPropertyBag);
  readonly size: number;
  readonly type: string;
  slice(start?: number, end?: number, contentType?: string): Blob;
  arrayBuffer(): Promise<ArrayBuffer>;
  text(): Promise<string>;
  stream(): ReadableStream<Uint8Array>;
}
declare class Headers {
  constructor(init?: HeadersInit);
  get(name: string): string | null;
  has(name: string): boolean;
  set(name: string, value: string): void;
  append(name: string, value: string): void;
  delete(name: string): void;
  forEach(cb: (value: string, key: string, parent: Headers) => void): void;
}
type HeadersInit = Headers | Record<string, string> | [string, string][];

interface RequestInit {
  method?: string;
  headers?: HeadersInit;
  body?: string | ArrayBuffer | ArrayBufferView | ReadableStream<Uint8Array>;
  signal?: AbortSignal;
  proxy?: string;
  range?: string;
  redirect?: 'follow' | 'manual' | 'error';
  credentials?: 'omit' | 'same-origin' | 'include';
}
declare class Request {
  constructor(input: string | Request, init?: RequestInit);
  readonly url: string;
  readonly method: string;
}
declare class Response {
  constructor(
    body?: string | ArrayBuffer | ArrayBufferView,
    init?: { status?: number; statusText?: string; headers?: HeadersInit },
  );
  readonly status: number;
  readonly statusText: string;
  readonly ok: boolean;
  readonly url: string;
  readonly headers: Headers;
  readonly bodyUsed: boolean;
  text(): Promise<string>;
  arrayBuffer(): Promise<ArrayBuffer>;
  json(): Promise<any>;
}
interface CookieJar {
  set(
    domain: string,
    name: string,
    value: string,
    path?: string,
    expires?: number,
    secure?: boolean,
    httpOnly?: boolean,
  ): void;
  get(url: string): string;
  clear(): void;
}
interface FxeFetch {
  (input: string | Request, init?: RequestInit): Promise<Response>;
  cookieJar?: (path?: string) => CookieJar;
}
declare var fetch: FxeFetch;

declare class AbortSignal {
  readonly aborted: boolean;
  readonly reason: string | undefined;
  addEventListener(type: 'abort', listener: () => void): void;
}
declare class AbortController {
  constructor();
  readonly signal: AbortSignal;
  abort(reason?: string): void;
}

declare class URL {
  constructor(input: string, base?: string);
  href: string;
  protocol: string;
  readonly host: string;
  hostname: string;
  port: string;
  pathname: string;
  search: string;
  hash: string;
  readonly origin: string;
  readonly username: string;
  readonly password: string;
  readonly searchParams: URLSearchParams;
  toString(): string;
}

type Transferable = ArrayBuffer | MessagePort;

interface FxeStructuredSerializeOptions {
  transfer?: Transferable[];
}

interface FxeMessageEvent<T = unknown> {
  readonly type: 'message' | 'messageerror';
  readonly data: T;
  readonly target: unknown;
  readonly currentTarget: unknown;
}

interface FxeErrorEvent {
  readonly type: 'error';
  readonly message: string;
  readonly error?: unknown;
  readonly target: unknown;
  readonly currentTarget: unknown;
}

type FxeMessageListener<T = unknown> = (event: FxeMessageEvent<T>) => void;
type FxeErrorListener = (event: FxeErrorEvent) => void;

declare function structuredClone<T>(value: T, options?: { transfer?: Transferable[] }): T;

declare class TextEncoder {
  constructor();
  encode(input?: string): Uint8Array;
}

declare class TextDecoder {
  constructor(label?: string);
  readonly encoding: string;
  decode(input?: ArrayBuffer | ArrayBufferView): string;
}

interface FxeWorkerOptions {
  type?: 'module' | 'classic';
  name?: string;
}

declare class Worker {
  constructor(specifier: string | URL, options?: FxeWorkerOptions);
  onmessage: FxeMessageListener | null;
  onmessageerror: FxeMessageListener | null;
  onerror: FxeErrorListener | null;
  postMessage(value: unknown, transfer?: Transferable[]): void;
  postMessage(value: unknown, options?: FxeStructuredSerializeOptions): void;
  terminate(): void;
  addEventListener(type: 'message' | 'messageerror', listener: FxeMessageListener): void;
  addEventListener(type: 'error', listener: FxeErrorListener): void;
  removeEventListener(type: 'message' | 'messageerror', listener: FxeMessageListener): void;
  removeEventListener(type: 'error', listener: FxeErrorListener): void;
}

declare class MessagePort {
  onmessage: FxeMessageListener | null;
  onmessageerror: FxeMessageListener | null;
  postMessage(value: unknown, transfer?: Transferable[]): void;
  postMessage(value: unknown, options?: FxeStructuredSerializeOptions): void;
  start(): void;
  close(): void;
  addEventListener(type: 'message' | 'messageerror', listener: FxeMessageListener): void;
  removeEventListener(type: 'message' | 'messageerror', listener: FxeMessageListener): void;
}

declare class MessageChannel {
  constructor();
  readonly port1: MessagePort;
  readonly port2: MessagePort;
}

declare class BroadcastChannel {
  constructor(name: string);
  readonly name: string;
  onmessage: FxeMessageListener | null;
  postMessage(value: unknown): void;
  close(): void;
  addEventListener(type: 'message', listener: FxeMessageListener): void;
  removeEventListener(type: 'message', listener: FxeMessageListener): void;
}

declare class URLSearchParams {
  constructor(init?: string | URLSearchParams | Record<string, string> | [string, string][]);
  get(name: string): string | null;
  getAll(name: string): string[];
  has(name: string): boolean;
  set(name: string, value: string): void;
  append(name: string, value: string): void;
  delete(name: string): void;
  toString(): string;
  forEach(cb: (value: string, key: string, parent: URLSearchParams) => void): void;
}

interface WebSocketEvent {
  type: string;
}
interface WebSocketMessageEvent extends WebSocketEvent {
  data: string | ArrayBuffer | Blob;
}
interface WebSocketCloseEvent extends WebSocketEvent {
  code: number;
  reason: string;
  wasClean: boolean;
}
interface WebSocketErrorEvent extends WebSocketEvent {
  message: string;
}
/** FXE-specific options for the global `WebSocket` constructor. Standard browsers accept only `(url, protocols)`. */
interface WebSocketOptions {
  /** Disable the `permessage-deflate` extension offer. Default true. */
  perMessageDeflate?: boolean;
  /** Maximum message size in bytes (post-decompression); larger triggers close 1009. */
  maxMessageBytes?: number;
  /** Maximum outbound fragment size in bytes for send-path fragmentation. */
  maxFragmentBytes?: number;
  /** Idle timeout in ms before sending a ping. */
  idleTimeoutMs?: number;
  /** Pong timeout in ms after a ping; missing pong closes 1011. */
  pongTimeoutMs?: number;
}

declare class WebSocket {
  static readonly CONNECTING: 0;
  static readonly OPEN: 1;
  static readonly CLOSING: 2;
  static readonly CLOSED: 3;
  constructor(url: string, protocols?: string | string[], options?: WebSocketOptions);
  readonly url: string;
  readonly readyState: 0 | 1 | 2 | 3;
  readonly bufferedAmount: number;
  readonly protocol: string;
  readonly extensions: string;
  binaryType: 'arraybuffer' | 'blob';
  onopen: ((ev: WebSocketEvent) => void) | null;
  onmessage: ((ev: WebSocketMessageEvent) => void) | null;
  onerror: ((ev: WebSocketErrorEvent) => void) | null;
  onclose: ((ev: WebSocketCloseEvent) => void) | null;
  send(data: string | ArrayBuffer | ArrayBufferView): void;
  close(code?: number, reason?: string): void;
  addEventListener(type: 'open' | 'message' | 'error' | 'close', listener: (ev: any) => void): void;
  removeEventListener(
    type: 'open' | 'message' | 'error' | 'close',
    listener: (ev: any) => void,
  ): void;
}
// === net end ===

// === audio begin ===
interface SoundPlayOptions {
  volume?: number;
  loop?: boolean;
  rate?: number;
}
declare class Sound {
  private constructor();
  play(options?: SoundPlayOptions): void;
  stop(): void;
  dispose(): void;
}

interface AudioDeviceInfo {
  id: string;
  name: string;
  isDefault: boolean;
}
type AudioDeviceKind = 'input' | 'output';
interface AudioCaptureOptions {
  sampleRate?: number;
  channels?: number;
  deviceId?: string;
}
interface AudioCaptureInfo {
  frameCount: number;
  channels: number;
  sampleRate: number;
}
type AudioCaptureCallback = (samples: Float32Array, info: AudioCaptureInfo) => void;
declare class CaptureSession {
  private constructor();
  stop(): void;
}
declare const Audio: {
  load(path: string): Promise<Sound>;
  loadFromBytes(bytes: Uint8Array): Promise<Sound>;
  setMasterVolume(v: number): void;
  enumerateDevices(kind: AudioDeviceKind): AudioDeviceInfo[];
  startCapture(options: AudioCaptureOptions, callback: AudioCaptureCallback): CaptureSession;
  startCapture(callback: AudioCaptureCallback): CaptureSession;
};
declare module 'fxe' {
  export const Audio: {
    load(path: string): Promise<Sound>;
    loadFromBytes(bytes: Uint8Array): Promise<Sound>;
    setMasterVolume(v: number): void;
    enumerateDevices(kind: AudioDeviceKind): AudioDeviceInfo[];
    startCapture(options: AudioCaptureOptions, callback: AudioCaptureCallback): CaptureSession;
    startCapture(callback: AudioCaptureCallback): CaptureSession;
  };
}
// === audio end ===

// === perf begin ===
interface PerformanceMarkSnapshot {
  count: number;
  totalMs: number;
  lastMs: number;
  minMs: number;
  maxMs: number;
}
interface PerformanceTimelineSnapshot {
  marks: Record<string, PerformanceMarkSnapshot>;
  render?: Record<string, number>;
}
interface PerformanceTimeline {
  beginMark(name: string): void;
  endMark(name: string): number;
  snapshot(): PerformanceTimelineSnapshot;
}
interface Performance {
  timeline: PerformanceTimeline;
}
interface FxeHmrRegistry {
  handlers: Record<string, Array<(path: string) => void>>;
  accept(handler: (path: string) => void): void;
  accept(path: string, handler: (path: string) => void): void;
  fire(path: string): number;
  invalidate(path: string): string[];
  reimport(path: string): Promise<void>;
  watch(path: string): FxeFsWatcher;
}
declare var __fxe_hmr: FxeHmrRegistry;
// === perf end ===

// === fxe:sqlite begin ===
declare module 'fxe:sqlite' {
  export type SQLValue = string | number | bigint | boolean | null | Uint8Array | ArrayBuffer;
  export type SQLBindings = SQLValue | Record<string, SQLValue> | readonly SQLValue[];

  export interface DatabaseOptions {
    /** Open without creating; refuses to create the file. */
    readonly?: boolean;
    /** Create the file if it does not exist (default true unless readonly). */
    create?: boolean;
    /** Open read-write (default true unless readonly). */
    readwrite?: boolean;
    /** Return INTEGER columns as bigint and validate 64-bit bigint inputs. */
    safeIntegers?: boolean;
    /** Bind named parameters by their bare key, error on missing keys. */
    strict?: boolean;
  }

  export interface RunResult {
    /** Last sqlite3_last_insert_rowid; bigint when safeIntegers. */
    lastInsertRowid: number | bigint;
    /** sqlite3_changes for the last statement. */
    changes: number;
  }

  export interface TransactionFunction<TArgs extends any[], TRet> {
    (...args: TArgs): TRet;
    deferred: (...args: TArgs) => TRet;
    immediate: (...args: TArgs) => TRet;
    exclusive: (...args: TArgs) => TRet;
    default: (...args: TArgs) => TRet;
  }

  export class Statement<TRow = any, TParams extends SQLBindings = SQLBindings> {
    private constructor();
    /** Run the statement and return all rows as objects. */
    all(...params: [TParams] | SQLValue[]): TRow[];
    /** Run the statement and return the first row, or null. */
    get(...params: [TParams] | SQLValue[]): TRow | null;
    /** Run the statement; returns { lastInsertRowid, changes }. */
    run(...params: [TParams] | SQLValue[]): RunResult;
    /** Run the statement and return all rows as raw value arrays. */
    values(...params: [TParams] | SQLValue[]): unknown[][];
    /** Step row-by-row; usable with `for..of`. */
    iterate(...params: [TParams] | SQLValue[]): IterableIterator<TRow>;
    [Symbol.iterator](): IterableIterator<TRow>;
    /** Bind result rows onto instances of `Class` via Object.create-style mapping. */
    as<U>(Class: new (...args: any[]) => U): Statement<U, TParams>;
    /** sqlite3_finalize the underlying statement. Idempotent. */
    finalize(): void;
    /** Expanded SQL with bound parameters substituted. */
    toString(): string;
    /** Column names from the most recent step / column metadata. */
    readonly columnNames: string[];
    /** sqlite3_bind_parameter_count. */
    readonly paramsCount: number;
    /** Reserved for future FFI use; currently null. */
    readonly native: unknown;
    [Symbol.dispose](): void;
  }

  export class Database {
    constructor(filename?: string | null, options?: DatabaseOptions);
    static deserialize(
      data: Uint8Array | ArrayBufferView,
      options?: DatabaseOptions | boolean,
    ): Database;
    /** No-op outside of macOS-with-Apple-SQLite; returns false. */
    static setCustomSQLite(path: string): boolean;

    /** Cached prepared statement (compilation memoised by SQL text). */
    query<TRow = any, TParams extends SQLBindings = SQLBindings>(
      sql: string,
    ): Statement<TRow, TParams>;
    /** Fresh prepared statement, never cached. */
    prepare<TRow = any, TParams extends SQLBindings = SQLBindings>(
      sql: string,
    ): Statement<TRow, TParams>;
    /** Execute one or more statements. With params, only one statement is allowed. */
    run(sql: string, params?: SQLBindings): RunResult;
    /** Alias for `run`. */
    exec(sql: string, params?: SQLBindings): RunResult;
    /** Wrap `fn` in BEGIN/COMMIT/ROLLBACK; nested calls become savepoints. */
    transaction<TArgs extends any[], TRet>(
      fn: (...args: TArgs) => TRet,
    ): TransactionFunction<TArgs, TRet>;
    /** sqlite3_serialize of `main` (or the named schema). */
    serialize(schema?: string): Uint8Array;
    /** sqlite3_load_extension. Throws when sqlite was built without extension support. */
    loadExtension(path: string, entry?: string): void;
    /** sqlite3_file_control on `main`. Returns the rc. */
    fileControl(cmd: number, value: number | ArrayBufferView | null): number;
    /** Close the database. Pass `true` to throw if statements remain pending. */
    close(throwOnError?: boolean): void;
    [Symbol.dispose](): void;

    readonly inTransaction: boolean;
    readonly filename: string;
    readonly handle: number;
  }

  export const constants: {
    readonly SQLITE_OPEN_READONLY: number;
    readonly SQLITE_OPEN_READWRITE: number;
    readonly SQLITE_OPEN_CREATE: number;
    readonly SQLITE_OPEN_FULLMUTEX: number;
    readonly SQLITE_OPEN_URI: number;
    readonly SQLITE_OPEN_MEMORY: number;
    readonly SQLITE_OPEN_NOMUTEX: number;
    readonly SQLITE_OPEN_SHAREDCACHE: number;
    readonly SQLITE_OPEN_PRIVATECACHE: number;
    readonly SQLITE_FCNTL_PERSIST_WAL: number;
    readonly SQLITE_FCNTL_CHUNK_SIZE: number;
    readonly SQLITE_FCNTL_LOCKSTATE: number;
    readonly SQLITE_FCNTL_FILE_POINTER: number;
    readonly SQLITE_FCNTL_SYNC_OMITTED: number;
    readonly SQLITE_FCNTL_VFSNAME: number;
    readonly SQLITE_PREPARE_PERSISTENT: number;
  };

  /** Returns the linked sqlite3 library version, e.g. "3.53.1". */
  export function version(): string;

  /** Default export equals `Database` for `import db from "fxe:sqlite"` ergonomics. */
  const _default: typeof Database;
  export default _default;
}
// === fxe:sqlite end ===
// === fxe:fs begin ===
declare module 'fxe:fs' {
  export function readFileSync(
    path: string,
    opts: 'utf8' | 'utf-8' | { encoding: 'utf8' | 'utf-8' },
  ): string;
  export function readFileSync(path: string, opts?: FxeReadOpts | null): Uint8Array;
  export function writeFileSync(path: string, data: string | ArrayBufferView | ArrayBuffer): void;
  export function appendFileSync(path: string, data: string | ArrayBufferView | ArrayBuffer): void;
  export function existsSync(path: string): boolean;
  export function statSync(path: string): FxeStats;
  export function readdirSync(path: string, opts: { withFileTypes: true }): FxeDirent[];
  export function readdirSync(path: string, opts?: FxeReaddirOpts | null): string[];
  export function mkdirSync(path: string, opts?: FxeMkdirOpts): void;
  export function rmSync(path: string, opts?: FxeRmOpts): void;
  export function renameSync(from: string, to: string): void;
  export function realpathSync(path: string): string;
  export function openSync(path: string, flags?: FxeOpenFlags, mode?: number): number;
  export function readSync(
    fd: number,
    buffer: ArrayBufferView,
    offset?: number,
    length?: number,
    position?: number | null,
  ): number;
  export function writeSync(
    fd: number,
    buffer: string | ArrayBufferView | ArrayBuffer,
    offset?: number,
    length?: number,
    position?: number | null,
  ): number;
  export function closeSync(fd: number): void;
  export function fstatSync(fd: number): FxeStats;
  export function ftruncateSync(fd: number, len?: number): void;
  export function fdatasyncSync(fd: number): void;
  export function readFile(
    path: string,
    opts: 'utf8' | 'utf-8' | { encoding: 'utf8' | 'utf-8' },
  ): Promise<string>;
  export function readFile(path: string, opts?: FxeReadOpts | null): Promise<Uint8Array>;
  export function writeFile(
    path: string,
    data: string | ArrayBufferView | ArrayBuffer,
    opts?: { signal?: AbortSignal } | null,
  ): Promise<void>;
  export function appendFile(
    path: string,
    data: string | ArrayBufferView | ArrayBuffer,
    opts?: { signal?: AbortSignal } | null,
  ): Promise<void>;
  export function stat(path: string, opts?: { signal?: AbortSignal } | null): Promise<FxeStats>;
  export function readdir(path: string, opts: { withFileTypes: true }): Promise<FxeDirent[]>;
  export function readdir(path: string, opts?: FxeReaddirOpts | null): Promise<string[]>;
  export function mkdir(path: string, opts?: FxeMkdirOpts): Promise<void>;
  export function rm(path: string, opts?: FxeRmOpts): Promise<void>;
  export function rename(
    from: string,
    to: string,
    opts?: { signal?: AbortSignal } | null,
  ): Promise<void>;
  export function realpath(path: string, opts?: { signal?: AbortSignal } | null): Promise<string>;
  export function exists(path: string, opts?: { signal?: AbortSignal } | null): Promise<boolean>;
  export function open(path: string, flags?: FxeOpenFlags, mode?: number): Promise<number>;
  export function open(
    path: string,
    flags: FxeOpenFlags,
    callback: (err: Error | null, fd: number) => void,
  ): Promise<number>;
  export function open(
    path: string,
    flags: FxeOpenFlags,
    mode: number,
    callback: (err: Error | null, fd: number) => void,
  ): Promise<number>;
  export function read<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset: number,
    length: number,
    position: number | null,
  ): Promise<FxeReadResult<TBuffer>>;
  export function read<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset: number,
    length: number,
    position: number | null,
    callback: (err: Error | null, bytesRead: number, buffer: TBuffer) => void,
  ): Promise<FxeReadResult<TBuffer>>;
  export function write<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset?: number,
    length?: number,
    position?: number | null,
  ): Promise<FxeWriteResult<TBuffer>>;
  export function write<TBuffer extends ArrayBufferView>(
    fd: number,
    buffer: TBuffer,
    offset: number,
    length: number,
    position: number | null,
    callback: (err: Error | null, bytesWritten: number, buffer: TBuffer) => void,
  ): Promise<FxeWriteResult<TBuffer>>;
  export function close(fd: number, callback?: (err?: Error | null) => void): Promise<void>;
  export function fstat(
    fd: number,
    callback?: (err: Error | null, stats: FxeStats) => void,
  ): Promise<FxeStats>;
  export function ftruncate(
    fd: number,
    len?: number,
    callback?: (err?: Error | null) => void,
  ): Promise<void>;
  export function fdatasync(fd: number, callback?: (err?: Error | null) => void): Promise<void>;
  export function watch(
    path: string,
    listener: (eventType: 'change' | 'rename', filename: string) => void,
  ): FxeFsWatcher;
  export function watch(
    path: string,
    opts: FxeWatchOpts,
    listener: (eventType: 'change' | 'rename', filename: string) => void,
  ): FxeFsWatcher;
  export function watch(path: string, opts: FxeWatchOpts): FxeFsWatcher;
  export function createReadStream(
    path: string,
    opts?: {
      flags?: FxeOpenFlags;
      fd?: number;
      start?: number;
      end?: number;
      highWaterMark?: number;
      encoding?: string;
      autoClose?: boolean;
    },
  ): FxeReadableStream;
  export function createWriteStream(
    path: string,
    opts?: {
      flags?: FxeOpenFlags;
      fd?: number;
      start?: number;
      mode?: number;
      autoClose?: boolean;
    },
  ): FxeWritableStream;
}
// === fxe:fs end ===

// === fxe:net begin ===
declare module 'fxe:net' {
  // TODO(impl): add declarations once src/js/bind_net.cpp defines the module surface.
}
// === fxe:net end ===

// === fxe:os begin ===
declare module 'fxe:os' {
  // TODO(impl): add declarations once src/js/bind_os.cpp defines the module surface.
}
// === fxe:os end ===

// === fxe:shell begin ===
declare module 'fxe:shell' {
  export function openExternal(url: string): boolean;
  export function showItemInFolder(path: string): boolean;
  export function beep(): void;
  export function trashItem(path: string): boolean;
}
// === fxe:shell end ===

// === fxe:ipc begin ===
declare module 'fxe:ipc' {
  /** A registered handler. Resolved value is sent back to the caller; thrown errors are reported as a rejection. */
  export type IPCHandler<TPayload = unknown, TResult = unknown> = (
    payload: TPayload,
  ) => TResult | Promise<TResult>;

  /** Register a request handler for `channel`. Replaces any existing handler. */
  export function handle<TPayload = unknown, TResult = unknown>(
    channel: string,
    fn: IPCHandler<TPayload, TResult>,
  ): void;

  /** Remove a handler. Returns true if a handler existed. */
  export function removeHandler(channel: string): boolean;

  /** Invoke a handler. The handler may live in this isolate (synchronous dispatch) or in a sibling worker (routed via MessagePort). Resolves with the handler's return value; rejects on missing handler or thrown error. */
  export function invoke<TPayload = unknown, TResult = unknown>(
    channel: string,
    payload?: TPayload,
  ): Promise<TResult>;

  /** Subscribe to a fire-and-forget channel. Multiple listeners per channel are allowed; listeners run in registration order. Returns a disposer. */
  export function on<TPayload = unknown>(
    channel: string,
    listener: (payload: TPayload) => void,
  ): () => void;

  /** Remove a single listener registered via on(). */
  export function off<TPayload = unknown>(
    channel: string,
    listener: (payload: TPayload) => void,
  ): void;

  /** Remove every listener for a channel (or all channels if no channel given). */
  export function removeAllListeners(channel?: string): void;

  /** Fire a fire-and-forget message to all listeners on `channel` (in this isolate and any connected worker). */
  export function send<TPayload = unknown>(channel: string, payload?: TPayload): void;

  /** Snapshot of the local routing table for diagnostics. Returns channel name -> { handler: boolean, listeners: number }. */
  export function debug(): Record<string, { handler: boolean; listeners: number }>;
}
// === fxe:ipc end ===
// === node:net compatibility begin ===
declare module 'node:net' {
  export class Socket {
    write(data: string | ArrayBufferView | ArrayBuffer, callback?: (err?: Error) => void): boolean;
    end(data?: string | ArrayBufferView | ArrayBuffer): this;
    destroy(error?: Error): this;
    connect(path: string, callback?: () => void): this;
    connect(options: { host?: string; port: number; path?: string }, callback?: () => void): this;
    on(event: 'data', listener: (chunk: Uint8Array) => void): this;
    on(event: 'end', listener: () => void): this;
    on(event: 'close', listener: () => void): this;
    on(event: 'connect', listener: () => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
    once(event: 'data', listener: (chunk: Uint8Array) => void): this;
    once(event: 'end', listener: () => void): this;
    once(event: 'close', listener: () => void): this;
    once(event: 'connect', listener: () => void): this;
    once(event: 'error', listener: (error: Error) => void): this;
    once(event: string, listener: (...args: unknown[]) => void): this;
    off(event: string, listener: (...args: unknown[]) => void): this;
  }
  export class Server {
    listen(path: string, callback?: () => void): this;
    listen(options: { path: string }, callback?: () => void): this;
    listen(port: number, host?: string, callback?: () => void): this;
    address(): { address: string; family: string; port: number } | string | null;
    close(callback?: () => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
  }
  export function connect(path: string, callback?: () => void): Socket;
  export function connect(port: number, host?: string, callback?: () => void): Socket;
  export function connect(
    options: { port: number; host?: string; path?: string },
    callback?: () => void,
  ): Socket;
  export function createConnection(path: string, callback?: () => void): Socket;
  export function createConnection(port: number, host?: string, callback?: () => void): Socket;
  export function createConnection(
    options: { port: number; host?: string; path?: string },
    callback?: () => void,
  ): Socket;
  export function createServer(connectionListener?: (socket: Socket) => void): Server;
  export function isIP(input: string): 0 | 4 | 6;
  export function isIPv4(input: string): boolean;
  export function isIPv6(input: string): boolean;
  const _default: {
    Socket: typeof Socket;
    Server: typeof Server;
    connect: typeof connect;
    createConnection: typeof createConnection;
    createServer: typeof createServer;
    isIP: typeof isIP;
    isIPv4: typeof isIPv4;
    isIPv6: typeof isIPv6;
  };
  export default _default;
}
// === node:net compatibility end ===
// === node:child_process compatibility begin ===
declare module 'node:child_process' {
  export interface ReadableChildStream {
    setEncoding(encoding: string): this;
    on(event: 'data', listener: (chunk: string | Uint8Array) => void): this;
    on(event: 'end', listener: () => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
  }
  export interface ChildProcessWithoutNullStreams {
    stdin: {
      write(chunk: string, encoding?: string, cb?: (err?: Error) => void): boolean;
      end(chunk?: string, cb?: () => void): boolean;
    };
    stdout: ReadableChildStream;
    stderr: ReadableChildStream;
    readonly pid?: number;
    readonly killed?: boolean;
    on(event: 'close', listener: (code: number | null, signal?: unknown) => void): this;
    on(event: 'exit', listener: (code: number | null, signal?: unknown) => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
    kill(signal?: string): boolean;
  }
  export function spawn(
    command: string,
    args?: readonly string[],
    options?: Record<string, unknown>,
  ): ChildProcessWithoutNullStreams;
  export function spawnSync(
    command: string,
    args?: readonly string[],
    options?: Record<string, unknown>,
  ): unknown;
  export function execFileSync(
    file: string,
    args?: readonly string[],
    options?: Record<string, unknown>,
  ): string | Uint8Array;
}
// === node:child_process compatibility end ===
// === node:worker_threads compatibility begin ===
declare module 'node:worker_threads' {
  export interface WorkerOptions {
    workerData?: unknown;
    eval?: boolean;
    stdout?: boolean;
    stderr?: boolean;
    [key: string]: unknown;
  }
  export class Worker {
    constructor(filename: string | URL, options?: WorkerOptions);
    on(event: 'message', listener: (data: unknown) => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
    on(event: 'exit', listener: (code: number) => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
    postMessage(value: unknown): void;
    terminate(): Promise<number>;
  }
  export class MessagePort {
    onmessage: ((event: { data: unknown }) => void) | null;
    postMessage(value: unknown): void;
    start?(): void;
    close?(): void;
  }
  export class MessageChannel {
    port1: MessagePort;
    port2: MessagePort;
  }
  export class BroadcastChannelImpl {
    constructor(name: string);
    readonly name: string;
    onmessage: ((event: { data: unknown }) => void) | null;
    postMessage(value: unknown): void;
    close(): void;
  }
  export const BroadcastChannel: (new (name: string) => BroadcastChannelImpl) | undefined;
  export const isMainThread: boolean;
  export const threadId: number;
  export const parentPort: MessagePort | null;
  export const workerData: unknown;
  export const capabilities: {
    worker: boolean;
    sameIsolateMessageChannel: boolean;
    sameIsolateBroadcastChannel: boolean;
    native: unknown;
  };
}
// === node:worker_threads compatibility end ===
// === node:dns compatibility begin ===
declare module 'node:dns' {
  export type LookupAddress = { address: string; family: number };
  export type LookupCallback = (err: Error | null, address?: string, family?: number) => void;
  export type LookupAllCallback = (err: Error | null, addresses?: LookupAddress[]) => void;
  export function lookup(
    hostname: string,
    options: { family?: number; all: true },
    callback: LookupAllCallback,
  ): void;
  export function lookup(hostname: string, callback: LookupCallback): void;
  export function lookup(
    hostname: string,
    options: { family?: number; all?: false },
    callback: LookupCallback,
  ): void;
  export function resolve4(
    hostname: string,
    callback: (err: Error | null, addresses?: string[]) => void,
  ): void;
  export function resolve(
    hostname: string,
    rrtype: string,
    callback: (err: Error | null, addresses?: string[]) => void,
  ): void;
  export function resolveAny(
    hostname: string,
    callback: (
      err: Error | null,
      records?: Array<{ address: string; family: number; type: string }>,
    ) => void,
  ): void;
  export function resolve6(
    hostname: string,
    callback: (err: Error | null, addresses?: string[]) => void,
  ): void;
  export function resolveTxt(
    hostname: string,
    callback: (err: Error | null, records?: string[][]) => void,
  ): void;
  export function resolveMx(
    hostname: string,
    callback: (err: Error | null, records?: Record<string, unknown>[]) => void,
  ): void;
  export function resolveSrv(
    hostname: string,
    callback: (err: Error | null, records?: Record<string, unknown>[]) => void,
  ): void;
  export function resolveCname(
    hostname: string,
    callback: (err: Error | null, records?: string[]) => void,
  ): void;
  export function resolveNs(
    hostname: string,
    callback: (err: Error | null, records?: string[]) => void,
  ): void;
  export function reverse(
    ip: string,
    callback: (err: Error | null, hostnames?: string[]) => void,
  ): void;
  export function lookupService(
    address: string,
    port: number,
    callback: (err: Error | null, hostname?: string, service?: string) => void,
  ): void;
  const _default: {
    lookup: typeof lookup;
    resolve: typeof resolve;
    resolveAny: typeof resolveAny;
    resolve4: typeof resolve4;
    resolve6: typeof resolve6;
    resolveTxt: typeof resolveTxt;
    resolveMx: typeof resolveMx;
    resolveSrv: typeof resolveSrv;
    resolveCname: typeof resolveCname;
    resolveNs: typeof resolveNs;
    reverse: typeof reverse;
    lookupService: typeof lookupService;
  };
  export default _default;
}
declare module 'node:dns/promises' {
  export function lookup(
    hostname: string,
    options?: { family?: number },
  ): Promise<{ address: string; family: number }>;
  export function resolve4(hostname: string): Promise<string[]>;
  const _default: {
    lookup: typeof lookup;
    resolve4: typeof resolve4;
  };
  export default _default;
}
// === node:dns compatibility end ===
// === node:os / node:tty compatibility begin ===
declare module 'node:os' {
  export function arch(): string;
  export function cpus(): unknown[];
  export function endianness(): 'LE' | 'BE';
  export function freemem(): number;
  export function homedir(): string;
  export function networkInterfaces(): Record<string, unknown> | null;
  export function platform(): string;
  export function release(): string;
  export function tmpdir(): string;
  export function totalmem(): number;
  export function type(): string;
  export function userInfo(): Record<string, unknown>;
  const _default: {
    arch: typeof arch;
    cpus: typeof cpus;
    endianness: typeof endianness;
    freemem: typeof freemem;
    homedir: typeof homedir;
    networkInterfaces: typeof networkInterfaces;
    platform: typeof platform;
    release: typeof release;
    tmpdir: typeof tmpdir;
    totalmem: typeof totalmem;
    type: typeof type;
    userInfo: typeof userInfo;
    hostname(): string;
    uptime(): number;
  };
  export default _default;
}
declare module 'node:tty' {
  export function isatty(fd: number): boolean;
  export function getWindowSize(fd: number): [number, number];
  const _default: {
    isatty: typeof isatty;
    getWindowSize: typeof getWindowSize;
  };
  export default _default;
}
declare module 'os' {
  export * from 'node:os';
  export { default } from 'node:os';
}
declare module 'tty' {
  export * from 'node:tty';
  export { default } from 'node:tty';
}
// === node:os / node:tty compatibility end ===
// === node:vm compatibility begin ===
interface FxeVmOptions {
  filename?: string;
  lineOffset?: number;
  columnOffset?: number;
  microtaskMode?: string;
}
declare module 'node:vm' {
  export type Context = Record<string, unknown>;
  export class Script {
    constructor(code: string, options?: FxeVmOptions);
    runInThisContext(options?: FxeVmOptions): unknown;
    runInContext(contextifiedObject: Context, options?: FxeVmOptions): unknown;
    runInNewContext(contextObject?: Record<string, unknown>, options?: FxeVmOptions): unknown;
  }
  export function createContext(sandbox?: Record<string, unknown>): Context;
  export function isContext(obj: unknown): obj is Context;
  export function runInThisContext(code: string, options?: FxeVmOptions): unknown;
  export function runInContext(
    code: string,
    contextifiedObject: Context,
    options?: FxeVmOptions,
  ): unknown;
  export function runInNewContext(
    code: string,
    contextObject?: Record<string, unknown>,
    options?: FxeVmOptions,
  ): unknown;
  export function compileFunction(
    code: string,
    params?: string[],
    options?: FxeVmOptions,
  ): (...args: unknown[]) => unknown;
  export function measureMemory(): Promise<never>;
  const _default: {
    Script: typeof Script;
    createContext: typeof createContext;
    isContext: typeof isContext;
    runInThisContext: typeof runInThisContext;
    runInContext: typeof runInContext;
    runInNewContext: typeof runInNewContext;
    compileFunction: typeof compileFunction;
    measureMemory: typeof measureMemory;
  };
  export default _default;
}
// === node:vm compatibility end ===
// === node:crypto compatibility begin ===
type FxeBufferSource = ArrayBuffer | ArrayBufferView;
type FxeKeyFormat = 'raw' | 'jwk' | 'spki' | 'pkcs8';
interface FxeJsonWebKey {
  kty?: string;
  crv?: string;
  x?: string;
  y?: string;
  d?: string;
  n?: string;
  e?: string;
  k?: string;
  key_ops?: string[];
  ext?: boolean;
  alg?: string;
}
interface FxeCryptoKey {
  readonly type: 'secret' | 'public' | 'private';
  readonly extractable: boolean;
  readonly algorithm: { readonly name: string; readonly [key: string]: unknown };
  readonly usages: readonly string[];
}
interface FxeSubtleCrypto {
  digest(algorithm: string | { name: string }, data: FxeBufferSource): Promise<ArrayBuffer>;
  importKey(
    format: FxeKeyFormat,
    keyData: FxeBufferSource | FxeJsonWebKey,
    algorithm: { name: string; [key: string]: unknown },
    extractable: boolean,
    keyUsages: string[],
  ): Promise<FxeCryptoKey>;
  exportKey(format: 'raw' | 'spki' | 'pkcs8', key: FxeCryptoKey): Promise<ArrayBuffer>;
  exportKey(format: 'jwk', key: FxeCryptoKey): Promise<FxeJsonWebKey>;
  generateKey(
    algorithm: { name: string; [key: string]: unknown },
    extractable: boolean,
    keyUsages: string[],
  ): Promise<FxeCryptoKey | { privateKey: FxeCryptoKey; publicKey: FxeCryptoKey }>;
  encrypt(
    algorithm: { name: string; [key: string]: unknown },
    key: FxeCryptoKey,
    data: FxeBufferSource,
  ): Promise<ArrayBuffer>;
  decrypt(
    algorithm: { name: string; [key: string]: unknown },
    key: FxeCryptoKey,
    data: FxeBufferSource,
  ): Promise<ArrayBuffer>;
  sign(
    algorithm: string | { name: string; [key: string]: unknown },
    key: FxeCryptoKey,
    data: FxeBufferSource,
  ): Promise<ArrayBuffer>;
  verify(
    algorithm: string | { name: string; [key: string]: unknown },
    key: FxeCryptoKey,
    signature: FxeBufferSource,
    data: FxeBufferSource,
  ): Promise<boolean>;
  deriveBits(
    algorithm: { name: string; [key: string]: unknown },
    baseKey: FxeCryptoKey,
    length: number,
  ): Promise<ArrayBuffer>;
  deriveKey(
    algorithm: { name: string; [key: string]: unknown },
    baseKey: FxeCryptoKey,
    derivedKeyType: { name: string; [key: string]: unknown },
    extractable: boolean,
    keyUsages: string[],
  ): Promise<FxeCryptoKey>;
  wrapKey(
    format: FxeKeyFormat,
    key: FxeCryptoKey,
    wrappingKey: FxeCryptoKey,
    wrapAlgorithm: { name: string; [key: string]: unknown },
  ): Promise<ArrayBuffer>;
  unwrapKey(
    format: FxeKeyFormat,
    wrappedKey: FxeBufferSource,
    unwrappingKey: FxeCryptoKey,
    unwrapAlgorithm: { name: string; [key: string]: unknown },
    unwrappedKeyAlgorithm: { name: string; [key: string]: unknown },
    extractable: boolean,
    keyUsages: string[],
  ): Promise<FxeCryptoKey>;
}
interface FxeCrypto {
  getRandomValues<T extends ArrayBufferView>(array: T): T;
  readonly subtle: FxeSubtleCrypto;
}
declare module 'node:crypto' {
  export interface Hash {
    update(data: string | FxeBufferSource, inputEncoding?: string): this;
    digest(): Uint8Array;
    digest(encoding: 'hex' | 'base64' | 'base64url'): string;
    digest(encoding: string): string | Uint8Array;
  }
  export interface Hmac {
    update(data: string | FxeBufferSource, inputEncoding?: string): this;
    digest(): Uint8Array;
    digest(encoding: 'hex' | 'base64' | 'base64url'): string;
    digest(encoding: string): string | Uint8Array;
  }
  export interface Cipher {
    update(
      data: string | FxeBufferSource,
      inputEncoding?: string,
      outputEncoding?: string,
    ): Uint8Array | string;
    final(outputEncoding?: string): Uint8Array | string;
    setAutoPadding(enabled?: boolean): this;
    setAAD(data: FxeBufferSource): this;
    getAuthTag(): Uint8Array;
    setAuthTag(tag: FxeBufferSource): this;
  }
  export function createHash(algorithm: string): Hash;
  export function createHmac(algorithm: string, key: string | FxeBufferSource): Hmac;
  export function createCipheriv(
    algorithm: string,
    key: FxeBufferSource,
    iv: FxeBufferSource,
  ): Cipher;
  export function createDecipheriv(
    algorithm: string,
    key: FxeBufferSource,
    iv: FxeBufferSource,
  ): Cipher;
  export function pbkdf2Sync(
    password: string | FxeBufferSource,
    salt: string | FxeBufferSource,
    iterations: number,
    keylen: number,
    digest?: string,
  ): Uint8Array;
  export function pbkdf2(
    password: string | FxeBufferSource,
    salt: string | FxeBufferSource,
    iterations: number,
    keylen: number,
    digest: string,
    callback: (error: unknown, derivedKey: Uint8Array) => void,
  ): void;
  export function pbkdf2(
    password: string | FxeBufferSource,
    salt: string | FxeBufferSource,
    iterations: number,
    keylen: number,
    callback: (error: unknown, derivedKey: Uint8Array) => void,
  ): void;
  export function scryptSync(
    password: string | FxeBufferSource,
    salt: string | FxeBufferSource,
    keylen: number,
    options?: Record<string, number>,
  ): Uint8Array;
  export function scrypt(
    password: string | FxeBufferSource,
    salt: string | FxeBufferSource,
    keylen: number,
    options: Record<string, number>,
    callback: (error: unknown, derivedKey: Uint8Array) => void,
  ): void;
  export function scrypt(
    password: string | FxeBufferSource,
    salt: string | FxeBufferSource,
    keylen: number,
    callback: (error: unknown, derivedKey: Uint8Array) => void,
  ): void;
  export function randomFillSync<T extends ArrayBufferView>(
    buffer: T,
    offset?: number,
    size?: number,
  ): T;
  export function randomBytes(size: number): Uint8Array;
  export function getRandomValues<T extends ArrayBufferView>(typedArray: T): T;
  export const webcrypto: FxeCrypto;
}
// === node:crypto compatibility end ===
type FxeURLConstructor = { new (input: string, base?: string): URL; prototype: URL };
type FxeURLSearchParamsConstructor = {
  new (
    init?: string | URLSearchParams | Record<string, string> | [string, string][],
  ): URLSearchParams;
  prototype: URLSearchParams;
};
type FxeTextEncoderConstructor = { new (): TextEncoder; prototype: TextEncoder };
type FxeTextDecoderConstructor = { new (label?: string): TextDecoder; prototype: TextDecoder };
// === node builtin compatibility begin ===
declare module 'node:buffer' {
  export type BufferEncoding = 'utf8' | 'utf-8' | 'hex' | 'base64';
  export interface Buffer extends Uint8Array {
    toString(encoding?: BufferEncoding): string;
    readonly length: number;
    subarray(start?: number, end?: number): Buffer;
    set(array: ArrayLike<number>, offset?: number): void;
  }
  export const Buffer: {
    from(
      value: string | ArrayBuffer | ArrayBufferView | ArrayLike<number>,
      encoding?: BufferEncoding,
    ): Buffer;
    alloc(size: number, fill?: string | number, encoding?: BufferEncoding): Buffer;
    allocUnsafe(size: number): Buffer;
    concat(list: readonly (Buffer | Uint8Array)[], totalLength?: number): Buffer;
    isBuffer(value: unknown): value is Buffer;
    byteLength(value: string | ArrayBuffer | ArrayBufferView, encoding?: BufferEncoding): number;
  };
  export function SlowBuffer(size: number): Buffer;
  export const INSPECT_MAX_BYTES: number;
  const buffer: { Buffer: typeof Buffer; SlowBuffer: typeof SlowBuffer; INSPECT_MAX_BYTES: number };
  export default buffer;
}
declare const Buffer: typeof import('node:buffer').Buffer;

declare module 'node:events' {
  export class EventEmitter {
    constructor(options?: { captureRejections?: boolean });
    static defaultMaxListeners: number;
    static listenerCount(
      emitter: EventEmitter,
      eventName: string | symbol,
      listener?: (...args: any[]) => void,
    ): number;
    static once(
      emitter: EventEmitter,
      eventName: string | symbol,
      options?: { signal?: AbortSignal },
    ): Promise<any[]>;
    on(eventName: string | symbol, listener: (...args: any[]) => void): this;
    addListener(eventName: string | symbol, listener: (...args: any[]) => void): this;
    once(eventName: string | symbol, listener: (...args: any[]) => void): this;
    off(eventName: string | symbol, listener: (...args: any[]) => void): this;
    removeListener(eventName: string | symbol, listener: (...args: any[]) => void): this;
    removeAllListeners(eventName?: string | symbol): this;
    emit(eventName: string | symbol, ...args: any[]): boolean;
    listeners(eventName: string | symbol): ((...args: any[]) => any)[];
    rawListeners(eventName: string | symbol): ((...args: any[]) => any)[];
    listenerCount(eventName: string | symbol, listener?: (...args: any[]) => void): number;
    eventNames(): Array<string | symbol>;
    setMaxListeners(n: number): this;
    getMaxListeners(): number;
  }
  export function once(
    emitter: EventEmitter,
    eventName: string | symbol,
    options?: { signal?: AbortSignal },
  ): Promise<any[]>;
  export function listenerCount(
    emitter: EventEmitter,
    eventName: string | symbol,
    listener?: (...args: any[]) => void,
  ): number;
  export default EventEmitter;
}
declare module 'events' {
  export * from 'node:events';
  export { default } from 'node:events';
}

declare module 'node:path' {
  export interface PlatformPath {
    readonly sep: string;
    readonly delimiter: string;
    resolve(...paths: string[]): string;
    normalize(path: string): string;
    isAbsolute(path: string): boolean;
    join(...paths: string[]): string;
    relative(from: string, to: string): string;
    dirname(path: string): string;
    basename(path: string, suffix?: string): string;
    extname(path: string): string;
    parse(path: string): { root: string; dir: string; base: string; ext: string; name: string };
    format(pathObject: {
      root?: string;
      dir?: string;
      base?: string;
      ext?: string;
      name?: string;
    }): string;
    toNamespacedPath(path: string): string;
  }
  export const posix: PlatformPath;
  export const win32: PlatformPath;
  export const sep: string;
  export const delimiter: string;
  export const resolve: PlatformPath['resolve'];
  export const normalize: PlatformPath['normalize'];
  export const isAbsolute: PlatformPath['isAbsolute'];
  export const join: PlatformPath['join'];
  export const relative: PlatformPath['relative'];
  export const dirname: PlatformPath['dirname'];
  export const basename: PlatformPath['basename'];
  export const extname: PlatformPath['extname'];
  export const parse: PlatformPath['parse'];
  export const format: PlatformPath['format'];
  export const toNamespacedPath: PlatformPath['toNamespacedPath'];
  const path: PlatformPath & { posix: PlatformPath; win32: PlatformPath };
  export default path;
}
declare module 'node:path/posix' {
  import { posix } from 'node:path';
  export = posix;
}
declare module 'node:path/win32' {
  import { win32 } from 'node:path';
  export = win32;
}

declare module 'node:querystring' {
  export function parse(
    str: string,
    sep?: string,
    eq?: string,
    options?: { maxKeys?: number; decodeURIComponent?: (s: string) => string },
  ): Record<string, string | string[]>;
  export function stringify(
    obj: Record<string, unknown>,
    sep?: string,
    eq?: string,
    options?: { encodeURIComponent?: (s: string) => string },
  ): string;
  export const decode: typeof parse;
  export const encode: typeof stringify;
  // biome-ignore lint/suspicious/noShadowRestrictedNames: querystring API mirrors Node's escape/unescape
  export function escape(str: string): string;
  // biome-ignore lint/suspicious/noShadowRestrictedNames: querystring API mirrors Node's escape/unescape
  export function unescape(str: string): string;
  const querystring: {
    parse: typeof parse;
    stringify: typeof stringify;
    decode: typeof decode;
    encode: typeof encode;
    escape: typeof escape;
    unescape: typeof unescape;
  };
  export default querystring;
}

declare module 'node:url' {
  export const URL: FxeURLConstructor;
  export const URLSearchParams: FxeURLSearchParamsConstructor;
  export function parse(
    urlString: string,
    parseQueryString?: boolean,
    slashesDenoteHost?: boolean,
  ): Record<string, any>;
  export function format(value: string | URL | Record<string, any>): string;
  export function resolve(from: string, to: string): string;
  export function fileURLToPath(url: string | URL): string;
  export function pathToFileURL(path: string): URL;
  export function domainToASCII(domain: string): string;
  export function domainToUnicode(domain: string): string;
}

declare module 'node:util' {
  export function format(format?: any, ...args: any[]): string;
  export function formatWithOptions(options: unknown, format?: any, ...args: any[]): string;
  export function inspect(value: unknown, options?: unknown): string;
  export function inherits(ctor: (...args: any[]) => any, superCtor: (...args: any[]) => any): void;
  export function promisify(fn: (...args: any[]) => any): (...args: any[]) => any;
  export namespace promisify {
    const custom: symbol;
  }
  export function callbackify(fn: (...args: any[]) => any): (...args: any[]) => any;
  export const types: {
    isArrayBuffer(value: unknown): value is ArrayBuffer;
    isAnyArrayBuffer(value: unknown): value is ArrayBuffer;
    isArrayBufferView(value: unknown): value is ArrayBufferView;
    isTypedArray(value: unknown): value is ArrayBufferView;
    isUint8Array(value: unknown): value is Uint8Array;
    isDate(value: unknown): value is Date;
    isRegExp(value: unknown): value is RegExp;
    isMap(value: unknown): value is Map<unknown, unknown>;
    isSet(value: unknown): value is Set<unknown>;
    isPromise(value: unknown): value is Promise<unknown>;
    isNativeError(value: unknown): value is Error;
  };
  export const TextEncoder: FxeTextEncoderConstructor;
  export const TextDecoder: FxeTextDecoderConstructor;
}
declare module 'node:util/types' {
  import { types } from 'node:util';
  export const isArrayBuffer: typeof types.isArrayBuffer;
  export const isAnyArrayBuffer: typeof types.isAnyArrayBuffer;
  export const isArrayBufferView: typeof types.isArrayBufferView;
  export const isTypedArray: typeof types.isTypedArray;
  export const isUint8Array: typeof types.isUint8Array;
  export const isDate: typeof types.isDate;
  export const isRegExp: typeof types.isRegExp;
  export const isMap: typeof types.isMap;
  export const isSet: typeof types.isSet;
  export const isPromise: typeof types.isPromise;
  export const isNativeError: typeof types.isNativeError;
  export default types;
}

declare module 'node:console' {
  export class Console {
    constructor(stdout?: { write(s: string): unknown }, stderr?: { write(s: string): unknown });
    log(...args: any[]): void;
    info(...args: any[]): void;
    debug(...args: any[]): void;
    warn(...args: any[]): void;
    error(...args: any[]): void;
  }
  export const log: (...args: any[]) => void;
  export const info: (...args: any[]) => void;
  export const debug: (...args: any[]) => void;
  export const warn: (...args: any[]) => void;
  export const error: (...args: any[]) => void;
}
declare module 'console' {
  export * from 'node:console';
}

declare module 'node:timers' {
  export const setTimeout: typeof globalThis.setTimeout;
  export const clearTimeout: typeof globalThis.clearTimeout;
  export const setInterval: typeof globalThis.setInterval;
  export const clearInterval: typeof globalThis.clearInterval;
  export const setImmediate: typeof globalThis.setImmediate;
  export const queueMicrotask: typeof globalThis.queueMicrotask;
}
declare module 'node:timers/promises' {
  export function setTimeout<T = void>(
    delay?: number,
    value?: T,
    options?: { signal?: AbortSignal },
  ): Promise<T>;
  export function setImmediate<T = void>(value?: T, options?: { signal?: AbortSignal }): Promise<T>;
  export function setInterval<T = void>(
    delay?: number,
    value?: T,
    options?: { signal?: AbortSignal },
  ): AsyncIterable<T>;
  export const scheduler: { wait: typeof setTimeout; yield(): Promise<void> };
}

declare module 'node:process' {
  export const argv: typeof process.argv;
  export const env: typeof process.env;
  export const stdin: typeof process.stdin;
  export const stdout: typeof process.stdout;
  export const stderr: typeof process.stderr;
  export const versions: typeof process.versions;
  export const release: typeof process.release;
  export const pid: typeof process.pid;
  export const platform: typeof process.platform;
  export const arch: typeof process.arch;
  export const cwd: typeof process.cwd;
  export const chdir: typeof process.chdir;
  export const exit: typeof process.exit;
  export const kill: typeof process.kill;
  export const umask: typeof process.umask;
  export const hrtime: typeof process.hrtime;
  export const nextTick: typeof process.nextTick;
  export const on: typeof process.on;
  export const off: typeof process.off;
  export default process;
}

declare module 'node:stream' {
  import { EventEmitter } from 'node:events';
  export class Readable extends EventEmitter {
    constructor(options?: any);
    static from(iterable: Iterable<any> | AsyncIterable<any>): Readable;
    setEncoding(encoding: string): this;
    read(): any;
    pipe(dest: Writable): Writable;
  }
  export class Writable extends EventEmitter {
    constructor(options?: any);
    write(
      chunk: any,
      encoding?: string | ((error?: unknown) => void),
      callback?: (error?: unknown) => void,
    ): boolean;
    end(chunk?: any, encoding?: string | (() => void), callback?: () => void): this;
  }
  export class Transform extends Readable {
    constructor(options?: any);
    write(
      chunk: any,
      encoding?: string | ((error?: unknown) => void),
      callback?: (error?: unknown) => void,
    ): boolean;
    end(chunk?: any, encoding?: string | (() => void), callback?: () => void): this;
  }
  export class Duplex extends Transform {}
  export class PassThrough extends Transform {}
  export function finished(stream: EventEmitter, callback: (error?: unknown) => void): EventEmitter;
  export function finished(stream: EventEmitter): Promise<void>;
  export function pipeline(...streams: any[]): Promise<any>;
}
declare module 'node:stream/promises' {
  export function finished(stream: import('node:events').EventEmitter): Promise<void>;
  export function pipeline(...streams: any[]): Promise<any>;
}
// === node builtin compatibility end ===

// === IndexedDB begin ===
// Faithful subset of the IndexedDB spec backed by per-database SQLite under
// `${App.getPath('userData')}/idb/<safe(name)>.sqlite3`. Limitations vs the
// browser spec:
//   - Keys may be number, string, Date, or ArrayBuffer. Array keys are not
//     supported in v1 (will throw DataError on encode).
//   - cursor.continue(key) and cursor.advance(N>1) are forward-only and
//     non-spec-compliant for arbitrary key targets; cursor.continue() with no
//     argument advances to the next row.
//   - multiEntry indexes are stored but not fanned out across array members.
//   - Single-process: no `versionchange`/`blocked` events to other connections.
//   - Transactions are committed when `tx.commit()` is called explicitly OR
//     resolve via `await tx.done`. GC of an uncommitted tx ROLLBACKs.

type IDBValidKey = number | string | Date | ArrayBuffer;
type IDBKeyOrRange = IDBValidKey | IDBKeyRange;
type IDBTransactionMode = 'readonly' | 'readwrite' | 'versionchange';
type IDBCursorDirection = 'next' | 'nextunique' | 'prev' | 'prevunique';

interface IDBKeyRange {
  readonly lower: IDBValidKey | undefined;
  readonly upper: IDBValidKey | undefined;
  readonly lowerOpen: boolean;
  readonly upperOpen: boolean;
  includes(key: IDBValidKey): boolean;
}
interface IDBKeyRangeCtor {
  only(value: IDBValidKey): IDBKeyRange;
  lowerBound(lower: IDBValidKey, open?: boolean): IDBKeyRange;
  upperBound(upper: IDBValidKey, open?: boolean): IDBKeyRange;
  bound(
    lower: IDBValidKey,
    upper: IDBValidKey,
    lowerOpen?: boolean,
    upperOpen?: boolean,
  ): IDBKeyRange;
}
declare const IDBKeyRange: IDBKeyRangeCtor;

interface IDBEvent<T = unknown> {
  readonly type: string;
  readonly target: T;
}
interface IDBVersionChangeEvent extends IDBEvent<IDBOpenDBRequest> {
  readonly oldVersion: number;
  readonly newVersion: number;
}

interface IDBRequest<T = unknown> {
  readonly result: T;
  readonly error: Error | null;
  readonly source: IDBObjectStore | IDBIndex | null;
  readonly transaction: IDBTransaction | null;
  readonly readyState: 'pending' | 'done';
  onsuccess: ((this: IDBRequest<T>, ev: IDBEvent<IDBRequest<T>>) => unknown) | null;
  onerror: ((this: IDBRequest<T>, ev: IDBEvent<IDBRequest<T>>) => unknown) | null;
}

interface IDBOpenDBRequest extends IDBRequest<IDBDatabase> {
  onupgradeneeded: ((this: IDBOpenDBRequest, ev: IDBVersionChangeEvent) => unknown) | null;
  onblocked: ((this: IDBOpenDBRequest, ev: IDBEvent<IDBOpenDBRequest>) => unknown) | null;
}

interface IDBDatabase {
  readonly name: string;
  readonly version: number;
  readonly objectStoreNames: readonly string[];
  onversionchange: ((this: IDBDatabase, ev: IDBVersionChangeEvent) => unknown) | null;
  onclose: ((this: IDBDatabase, ev: IDBEvent<IDBDatabase>) => unknown) | null;
  createObjectStore(
    name: string,
    options?: { keyPath?: string | null; autoIncrement?: boolean },
  ): IDBObjectStore;
  deleteObjectStore(name: string): void;
  transaction(stores: string | readonly string[], mode?: IDBTransactionMode): IDBTransaction;
  close(): void;
}

interface IDBTransaction {
  readonly db: IDBDatabase;
  readonly mode: IDBTransactionMode;
  readonly objectStoreNames: readonly string[];
  readonly done: Promise<void>;
  oncomplete: ((this: IDBTransaction, ev: IDBEvent<IDBTransaction>) => unknown) | null;
  onerror: ((this: IDBTransaction, ev: IDBEvent<IDBTransaction>) => unknown) | null;
  onabort: ((this: IDBTransaction, ev: IDBEvent<IDBTransaction>) => unknown) | null;
  objectStore(name: string): IDBObjectStore;
  commit(): void;
  abort(): void;
}

interface IDBObjectStore {
  readonly name: string;
  readonly keyPath: string | null;
  readonly autoIncrement: boolean;
  readonly indexNames: readonly string[];
  readonly transaction: IDBTransaction;
  put<T = unknown>(value: T, key?: IDBValidKey): IDBRequest<IDBValidKey>;
  add<T = unknown>(value: T, key?: IDBValidKey): IDBRequest<IDBValidKey>;
  get<T = unknown>(key: IDBKeyOrRange): IDBRequest<T | undefined>;
  getKey(key: IDBKeyOrRange): IDBRequest<IDBValidKey | undefined>;
  getAll<T = unknown>(query?: IDBKeyOrRange | null, count?: number): IDBRequest<T[]>;
  getAllKeys(query?: IDBKeyOrRange | null, count?: number): IDBRequest<IDBValidKey[]>;
  delete(key: IDBKeyOrRange): IDBRequest<void>;
  clear(): IDBRequest<void>;
  count(query?: IDBKeyOrRange | null): IDBRequest<number>;
  createIndex(
    name: string,
    keyPath: string,
    options?: { unique?: boolean; multiEntry?: boolean },
  ): IDBIndex;
  deleteIndex(name: string): void;
  index(name: string): IDBIndex;
  openCursor(
    query?: IDBKeyOrRange | null,
    direction?: IDBCursorDirection,
  ): IDBRequest<IDBCursorWithValue | null>;
  openKeyCursor(
    query?: IDBKeyOrRange | null,
    direction?: IDBCursorDirection,
  ): IDBRequest<IDBCursor | null>;
}

interface IDBIndex {
  readonly name: string;
  readonly keyPath: string;
  readonly unique: boolean;
  readonly multiEntry: boolean;
  readonly objectStore: IDBObjectStore;
  get<T = unknown>(key: IDBKeyOrRange): IDBRequest<T | undefined>;
  getKey(key: IDBKeyOrRange): IDBRequest<IDBValidKey | undefined>;
  getAll<T = unknown>(query?: IDBKeyOrRange | null, count?: number): IDBRequest<T[]>;
  getAllKeys(query?: IDBKeyOrRange | null, count?: number): IDBRequest<IDBValidKey[]>;
  count(query?: IDBKeyOrRange | null): IDBRequest<number>;
  openCursor(
    query?: IDBKeyOrRange | null,
    direction?: IDBCursorDirection,
  ): IDBRequest<IDBCursorWithValue | null>;
  openKeyCursor(
    query?: IDBKeyOrRange | null,
    direction?: IDBCursorDirection,
  ): IDBRequest<IDBCursor | null>;
}

interface IDBCursor {
  readonly source: IDBObjectStore | IDBIndex;
  readonly direction: IDBCursorDirection;
  readonly key: IDBValidKey;
  readonly primaryKey: IDBValidKey;
  /** Advance to the next row. The `key` argument from the spec is NOT supported in v1. */
  continue(key?: IDBValidKey): void;
  /** Skip forward by N rows (N >= 1). */
  advance(count: number): void;
  /** Replace the current row's value. Re-derives keyPath if applicable. */
  update<T = unknown>(value: T): IDBRequest<IDBValidKey>;
  /** Delete the current row. */
  delete(): IDBRequest<void>;
}
interface IDBCursorWithValue extends IDBCursor {
  readonly value: unknown;
}

interface IDBFactory {
  open(name: string, version?: number): IDBOpenDBRequest;
  deleteDatabase(name: string): IDBOpenDBRequest;
  databases(): Promise<{ name: string; version: number }[]>;
  cmp(a: IDBValidKey, b: IDBValidKey): -1 | 0 | 1;
}
declare const indexedDB: IDBFactory;
// === IndexedDB end ===

// === Intl note begin ===
// `Intl` is provided by the embedded V8 (built with ICU; `icudtl.dat` staged at
// runtime via FXE_V8_ICUDTL). The full Intl namespace types — DateTimeFormat,
// NumberFormat, RelativeTimeFormat, DisplayNames, Collator, PluralRules,
// Segmenter, Locale, ListFormat — are pulled from `lib.es*.intl.d.ts` via the
// `lib: ["ESNext"]` setting in tsconfig.json. No explicit declaration is
// required here; this comment documents the wiring.
// === Intl note end ===

// === node:wasi ===
declare module 'node:wasi' {
  export type WasiImportObject = {
    wasi_snapshot_preview1: {
      proc_exit(code: number): never;
      fd_write(fd: number, iovsPtr: number, iovsLen: number, nwrittenPtr: number): number;
      fd_read(fd: number, iovsPtr: number, iovsLen: number, nreadPtr: number): number;
      fd_close(fd: number): number;
      fd_seek(
        fd: number,
        offsetLow: number,
        offsetHigh: number,
        whence: number,
        newOffsetPtr: number,
      ): number;
      fd_fdstat_get(fd: number, statPtr: number): number;
      fd_fdstat_set_flags(fd: number, flags: number): number;
      random_get(buf: number, len: number): number;
      clock_time_get(id: number, precision: bigint | number, timeOutPtr: number): number;
      clock_res_get(id: number, resOutPtr: number): number;
      environ_get(envPtr: number, envBufPtr: number): number;
      environ_sizes_get(countPtr: number, sizePtr: number): number;
      args_get(argvPtr: number, argvBufPtr: number): number;
      args_sizes_get(countPtr: number, sizePtr: number): number;
      path_open(
        fd: number,
        dirflags: number,
        pathPtr: number,
        pathLen: number,
        oflags: number,
        rightsBaseLow: number,
        rightsBaseHigh: number,
        rightsInheritingLow: number,
        rightsInheritingHigh: number,
        fdflags: number,
        openedFdPtr: number,
      ): number;
      [name: string]: ((...args: any[]) => number) | ((code: number) => never);
    };
  };

  export interface WASIOptions {
    args?: string[];
    env?: Record<string, string | number | boolean>;
  }

  export interface WASIMemory {
    buffer: ArrayBufferLike;
  }

  export interface WASIStartInstance {
    exports: {
      memory: WASIMemory;
      _start?: () => unknown;
    };
  }

  export interface WASIInitializeInstance {
    exports: {
      memory: WASIMemory;
      _initialize?: () => unknown;
    };
  }

  export class WASI {
    constructor(options?: WASIOptions);
    args: string[];
    env: string[];
    exitCode: number;
    start(instance: WASIStartInstance): void;
    initialize(instance: WASIInitializeInstance): void;
    getImportObject(): WasiImportObject;
  }
}
declare module 'wasi' {
  export { WASI } from 'node:wasi';
}

// === Internal Node-compat audit hook ===
//
// Returns a JSON string `{"specifier":"<s>","source":"native"|"unenv"|"unsupported","assetPath"?:"<p>"}`
// for the given Node builtin specifier. Test-only; may be removed without notice.
declare const __fxe_node_compat_status: (specifier: string) => string;

// === node:v8 minimal declaration ===
// Real implementation is provided by the host binding. This declaration covers
// only the named exports that consumer code references; broader Node parity
// is intentionally not declared here.
declare module 'node:v8' {
  export function getHeapStatistics(): {
    total_heap_size: number;
    total_heap_size_executable: number;
    total_physical_size: number;
    total_available_size: number;
    used_heap_size: number;
    heap_size_limit: number;
    malloced_memory: number;
    peak_malloced_memory: number;
    does_zap_garbage: number;
    number_of_native_contexts: number;
    number_of_detached_contexts: number;
    total_global_handles_size: number;
    used_global_handles_size: number;
    external_memory: number;
  };
  export function getHeapSpaceStatistics(): Array<{
    space_name: string;
    space_size: number;
    space_used_size: number;
    space_available_size: number;
    physical_space_size: number;
  }>;
  export function getHeapCodeStatistics(): {
    code_and_metadata_size: number;
    bytecode_and_metadata_size: number;
    external_script_source_size: number;
  };
  export function cachedDataVersionTag(): number;
  export function serialize(value: unknown): Uint8Array;
  export function deserialize(bytes: ArrayBufferView | ArrayBuffer): unknown;
  export function writeHeapSnapshot(filePath: string): string | null;
  const def: {
    getHeapStatistics: typeof getHeapStatistics;
    getHeapSpaceStatistics: typeof getHeapSpaceStatistics;
    getHeapCodeStatistics: typeof getHeapCodeStatistics;
    cachedDataVersionTag: typeof cachedDataVersionTag;
    serialize: typeof serialize;
    deserialize: typeof deserialize;
    writeHeapSnapshot: typeof writeHeapSnapshot;
  };
  export default def;
}

// ---------------------------------------------------------------------------
// Markdown — CommonMark + GFM parser exposed via the `Markdown` global.
// Backed by md4c (src/markdown/parser.cpp). Parses to a JSON-shaped AST;
// rendering lives in the fxe-ui Markdown component.
// ---------------------------------------------------------------------------

declare namespace FXEMarkdown {
  type NodeType =
    | 'document'
    | 'paragraph'
    | 'heading'
    | 'blockquote'
    | 'list'
    | 'list_item'
    | 'code_block'
    | 'html_block'
    | 'thematic_break'
    | 'table'
    | 'table_head'
    | 'table_body'
    | 'table_row'
    | 'table_cell'
    | 'emph'
    | 'strong'
    | 'strikethrough'
    | 'underline'
    | 'code_span'
    | 'link'
    | 'image'
    | 'latex_math'
    | 'wikilink'
    | 'raw_html'
    | 'text'
    | 'soft_break'
    | 'hard_break'
    | 'entity'
    | 'null_char';

  interface BaseNode {
    type: NodeType;
    children?: Node[];
  }
  interface DocumentNode extends BaseNode {
    type: 'document';
    children: Node[];
  }
  interface ParagraphNode extends BaseNode {
    type: 'paragraph';
    children: Node[];
  }
  interface HeadingNode extends BaseNode {
    type: 'heading';
    level: 1 | 2 | 3 | 4 | 5 | 6;
    children: Node[];
  }
  interface BlockquoteNode extends BaseNode {
    type: 'blockquote';
    children: Node[];
  }
  interface ListNode extends BaseNode {
    type: 'list';
    ordered: boolean;
    tight: boolean;
    start?: number;
    children: ListItemNode[];
  }
  interface ListItemNode extends BaseNode {
    type: 'list_item';
    task?: true;
    checked?: boolean;
    children: Node[];
  }
  interface CodeBlockNode extends BaseNode {
    type: 'code_block';
    info: string;
    lang: string;
    children: TextNode[];
  }
  interface HtmlBlockNode extends BaseNode {
    type: 'html_block';
    text: string;
  }
  interface ThematicBreakNode extends BaseNode {
    type: 'thematic_break';
  }
  interface TableNode extends BaseNode {
    type: 'table';
    children: Node[];
  }
  interface TableHeadNode extends BaseNode {
    type: 'table_head';
    children: TableRowNode[];
  }
  interface TableBodyNode extends BaseNode {
    type: 'table_body';
    children: TableRowNode[];
  }
  interface TableRowNode extends BaseNode {
    type: 'table_row';
    children: TableCellNode[];
  }
  interface TableCellNode extends BaseNode {
    type: 'table_cell';
    align?: 'left' | 'right' | 'center';
    children: Node[];
  }
  interface EmphNode extends BaseNode {
    type: 'emph';
    children: Node[];
  }
  interface StrongNode extends BaseNode {
    type: 'strong';
    children: Node[];
  }
  interface StrikethroughNode extends BaseNode {
    type: 'strikethrough';
    children: Node[];
  }
  interface UnderlineNode extends BaseNode {
    type: 'underline';
    children: Node[];
  }
  interface CodeSpanNode extends BaseNode {
    type: 'code_span';
    children: TextNode[];
  }
  interface LinkNode extends BaseNode {
    type: 'link';
    href: string;
    title?: string;
    autolink?: true;
    children: Node[];
  }
  interface ImageNode extends BaseNode {
    type: 'image';
    src: string;
    title?: string;
    children: Node[];
  }
  interface LatexMathNode extends BaseNode {
    type: 'latex_math';
  }
  interface WikilinkNode extends BaseNode {
    type: 'wikilink';
    target: string;
    children: Node[];
  }
  interface RawHtmlNode extends BaseNode {
    type: 'raw_html';
    text: string;
  }
  interface TextNode extends BaseNode {
    type: 'text';
    text: string;
  }
  interface SoftBreakNode extends BaseNode {
    type: 'soft_break';
  }
  interface HardBreakNode extends BaseNode {
    type: 'hard_break';
  }
  interface EntityNode extends BaseNode {
    type: 'entity';
    text: string;
  }
  interface NullCharNode extends BaseNode {
    type: 'null_char';
    text: string;
  }

  type Node =
    | DocumentNode
    | ParagraphNode
    | HeadingNode
    | BlockquoteNode
    | ListNode
    | ListItemNode
    | CodeBlockNode
    | HtmlBlockNode
    | ThematicBreakNode
    | TableNode
    | TableHeadNode
    | TableBodyNode
    | TableRowNode
    | TableCellNode
    | EmphNode
    | StrongNode
    | StrikethroughNode
    | UnderlineNode
    | CodeSpanNode
    | LinkNode
    | ImageNode
    | LatexMathNode
    | WikilinkNode
    | RawHtmlNode
    | TextNode
    | SoftBreakNode
    | HardBreakNode
    | EntityNode
    | NullCharNode;

  interface ParseOptions {
    /** Convenience preset. Defaults to 'github'. Overridden by `flags`. */
    dialect?: 'commonmark' | 'github' | 'gfm';
    /** Bitmask of `Markdown.FLAG_*` constants. Overrides `dialect`. */
    flags?: number;
  }

  /** A non-overlapping range tagged with a tree-sitter capture name
   *  (e.g. "keyword", "string", "type"). Offsets are JavaScript UTF-16
   *  code-unit indices suitable for `source.slice(start, end)`. Text between
   *  adjacent tokens, or before/after the first/last token, is unhighlighted
   *  plain text. */
  interface HighlightToken {
    /** UTF-16 code-unit index where this token begins. */
    start: number;
    /** UTF-16 code-unit index (exclusive) where this token ends. */
    end: number;
    /** Tree-sitter capture name. Common values produced by the built-in
     *  queries: `comment`, `string`, `number`, `constant`, `keyword`,
     *  `type`, `function`, `property`. */
    name: string;
  }

  interface HighlightResult {
    /** Canonical grammar name actually used (after alias resolution, e.g.
     *  `'ts'` → `'typescript'`). */
    language: string;
    /** Tokens sorted by `start`, non-overlapping. */
    tokens: HighlightToken[];
  }
}

declare const Markdown: {
  parse(source: string, opts?: FXEMarkdown.ParseOptions): FXEMarkdown.DocumentNode;
  readonly FLAG_COLLAPSE_WHITESPACE: number;
  readonly FLAG_PERMISSIVE_ATX_HEADERS: number;
  readonly FLAG_PERMISSIVE_URL_AUTOLINKS: number;
  readonly FLAG_PERMISSIVE_EMAIL_AUTOLINKS: number;
  readonly FLAG_NO_INDENTED_CODE_BLOCKS: number;
  readonly FLAG_NO_HTML_BLOCKS: number;
  readonly FLAG_NO_HTML_SPANS: number;
  readonly FLAG_TABLES: number;
  readonly FLAG_STRIKETHROUGH: number;
  readonly FLAG_PERMISSIVE_WWW_AUTOLINKS: number;
  readonly FLAG_TASKLISTS: number;
  readonly FLAG_LATEX_MATH_SPANS: number;
  readonly FLAG_WIKILINKS: number;
  readonly FLAG_UNDERLINE: number;
  readonly DIALECT_COMMONMARK: number;
  readonly DIALECT_GITHUB: number;
  /**
   * Run the built-in tree-sitter highlight query for `language` over
   * `source`. Returns `null` for unsupported languages, or when the build
   * was configured without tree-sitter. Aliases: `ts`/`js` → `typescript`,
   * `jsx` → `tsx`, `jsonc` → `json`.
   */
  highlight(source: string, language: string): FXEMarkdown.HighlightResult | null;
  /** Languages with a built-in highlights query. */
  highlightLanguages(): readonly string[];
};
