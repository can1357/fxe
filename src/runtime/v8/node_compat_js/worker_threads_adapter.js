const g = globalThis;

const nativeWorker = g.__fxe_native?.worker;

const cloneMessage = (value) => {
  if (value === null || typeof value !== 'object') {
    return value;
  }
  return typeof g.structuredClone === 'function' ? g.structuredClone(value) : JSON.parse(JSON.stringify(value));
};

const serializeWorkerData = (value) => JSON.stringify(cloneMessage(value));
const deserializeWorkerData = (json) => JSON.parse(json);

const scheduleMessage = (fn) => {
  Promise.resolve().then(fn);
};

const dispatchMessage = (target, data) => {
  if (target._closed) {
    return;
  }
  const event = { type: 'message', data, target, currentTarget: target };
  if (typeof target.onmessage === 'function') {
    target.onmessage.call(target, event);
  }
  for (const listener of Array.from(target._listeners)) {
    if (!target._closed && typeof listener === 'function') {
      listener.call(target, event);
    }
  }
};

const dispatchClose = (target) => {
  if (target._closed) {
    return;
  }
  const event = { type: 'close', data: null, target, currentTarget: target };
  if (typeof target.onclose === 'function') {
    target.onclose.call(target, event);
  }
  for (const listener of Array.from(target._closeListeners ?? [])) {
    if (!target._closed && typeof listener === 'function') {
      listener.call(target, event);
    }
  }
};

const dispatchError = (target, error) => {
  if (target._closed) {
    return;
  }
  const event = { type: 'error', error, message: error?.message ?? String(error), target, currentTarget: target };
  if (typeof target.onerror === 'function') {
    target.onerror.call(target, event);
  }
  for (const listener of Array.from(target._errorListeners)) {
    if (!target._closed && typeof listener === 'function') {
      listener.call(target, event);
    }
  }
};

const dispatchExit = (target, code) => {
  if (target._closed) {
    return;
  }
  const event = { type: 'exit', exitCode: code, target, currentTarget: target };
  for (const listener of Array.from(target._exitListeners)) {
    if (!target._closed && typeof listener === 'function') {
      listener.call(target, code, event);
    }
  }
};

const dispatchOnline = (target) => {
  if (target._closed) {
    return;
  }
  const event = { type: 'online', target, currentTarget: target };
  for (const listener of Array.from(target._onlineListeners)) {
    if (!target._closed && typeof listener === 'function') {
      listener.call(target, event);
    }
  }
};

class FxeMessagePort {
  constructor(nativePort = null) {
    this.onmessage = null;
    this.onclose = null;
    this._closed = false;
    this._listeners = new Set();
    this._closeListeners = new Set();
    this._peer = null;
    this._nativePort = nativePort;
    this._polling = false;
    this._pollTimer = undefined;
  }
  postMessage(value, transferList = undefined) {
    if (this._closed) {
      return;
    }
    if (this._nativePort !== null) {
      return this._nativePort.postMessage(value, transferList);
    }
    if (this._peer === null || this._peer._closed) {
      return;
    }
    const peer = this._peer;
    const data = cloneMessage(value);
    scheduleMessage(() => {
      if (!this._closed && !peer._closed) {
        dispatchMessage(peer, data);
      }
    });
  }
  start() {
    if (this._nativePort === null || this._polling || this._closed) {
      return;
    }
    this._polling = true;
    this._poll();
  }
  close() {
    if (this._closed) {
      return;
    }
    this._closed = true;
    this._polling = false;
    if (this._pollTimer !== undefined) {
      clearTimeout(this._pollTimer);
      this._pollTimer = undefined;
    }
    this._listeners.clear();
    this._closeListeners.clear();
    this._nativePort?.close?.();
  }
  addEventListener(type, listener) {
    if (typeof listener !== 'function') {
      return;
    }
    if (type === 'message') {
      this._listeners.add(listener);
      this.start();
    } else if (type === 'close') {
      this._closeListeners.add(listener);
      this.start();
    }
  }
  removeEventListener(type, listener) {
    if (type === 'message') {
      this._listeners.delete(listener);
    } else if (type === 'close') {
      this._closeListeners.delete(listener);
    }
  }
  on(type, listener) {
    this.addEventListener(type, listener);
    return this;
  }
  off(type, listener) {
    this.removeEventListener(type, listener);
    return this;
  }
  _poll() {
    if (!this._polling || this._closed || this._nativePort === null) {
      return;
    }
    for (const event of this._nativePort.drainMessages()) {
      if (event.type === 'message') {
        dispatchMessage(this, event.data);
      } else if (event.type === 'close') {
        dispatchClose(this);
        this.close();
        return;
      }
    }
    this._pollTimer = setTimeout(() => {
      this._pollTimer = undefined;
      this._poll();
    }, 1);
  }
  get [Symbol.toStringTag]() {
    return 'MessagePort';
  }
}

const nativeCreateMessageChannel =
  nativeWorker != null && typeof nativeWorker.createMessageChannel === 'function'
    ? nativeWorker.createMessageChannel.bind(nativeWorker)
    : undefined;

class FxeMessageChannel {
  constructor() {
    if (typeof nativeCreateMessageChannel === 'function') {
      const nativeChannel = nativeCreateMessageChannel();
      this.port1 = new FxeMessagePort(nativeChannel.port1);
      this.port2 = new FxeMessagePort(nativeChannel.port2);
      return;
    }
    this.port1 = new FxeMessagePort();
    this.port2 = new FxeMessagePort();
    this.port1._peer = this.port2;
    this.port2._peer = this.port1;
  }
  get [Symbol.toStringTag]() {
    return 'MessageChannel';
  }
}

const GlobalMessageChannel = typeof g.MessageChannel === 'function' ? g.MessageChannel : undefined;
const GlobalMessagePort =
  typeof g.MessagePort === 'function'
    ? g.MessagePort
    : GlobalMessageChannel !== undefined
      ? new GlobalMessageChannel().port1.constructor
      : undefined;

const nativeWorkerStart =
  nativeWorker != null && typeof nativeWorker.spawn === 'function'
    ? nativeWorker.spawn.bind(nativeWorker)
    : nativeWorker != null && typeof nativeWorker.start === 'function'
      ? nativeWorker.start.bind(nativeWorker)
      : nativeWorker != null && typeof nativeWorker.createWorker === 'function'
        ? nativeWorker.createWorker.bind(nativeWorker)
        : undefined;
const nativeWorkerIsAvailable =
  nativeWorker != null &&
  nativeWorker.notImplemented !== true &&
  nativeWorker.available === true &&
  typeof nativeWorkerStart === 'function';

const fallbackNativeDetail = Object.freeze({
  available: false,
  code: 'FXE_WORKER_NATIVE_MISSING',
  reason: 'native worker namespace is missing',
  message: 'worker_threads.Worker is not available: native worker namespace is missing',
});

const nativeDetails = nativeWorker ?? fallbackNativeDetail;
const exportedMessageChannel = typeof nativeCreateMessageChannel === 'function' ? FxeMessageChannel : GlobalMessageChannel ?? FxeMessageChannel;

export const capabilities = Object.freeze({
  worker: nativeWorkerIsAvailable,
  sameIsolateMessageChannel: typeof (GlobalMessageChannel ?? FxeMessageChannel) === 'function',
  crossIsolateMessageChannel: typeof nativeCreateMessageChannel === 'function',
  sameIsolateBroadcastChannel: typeof g.BroadcastChannel === 'function',
  native: nativeDetails,
});

export const isMainThread = nativeWorker?.isMainThread !== false;
export const threadId = Number(nativeWorker?.threadId ?? 0);
export const workerData = isMainThread
  ? undefined
  : deserializeWorkerData(typeof nativeWorker?.workerDataJson === 'string' ? nativeWorker.workerDataJson : 'null');
export const MessageChannel = exportedMessageChannel;
export const MessagePort = typeof nativeCreateMessageChannel === 'function' ? FxeMessagePort : GlobalMessagePort ?? FxeMessagePort;
// FXE does not yet have a native cross-isolate broadcast registry. Keep the host
// same-isolate BroadcastChannel when present and otherwise report the feature as absent.
export const BroadcastChannel = typeof g.BroadcastChannel === 'function' ? g.BroadcastChannel : undefined;

class ParentPort {
  constructor() {
    this.onmessage = null;
    this._closed = false;
    this._listeners = new Set();
    this._polling = false;
    this._pollTimer = undefined;
  }
  postMessage(value, transferList = undefined) {
    if (this._closed) {
      return;
    }
    nativeWorker.postMessage(value, transferList);
  }
  start() {
    if (this._polling || this._closed) {
      return;
    }
    this._polling = true;
    this._poll();
  }
  close() {
    this._closed = true;
    this._polling = false;
    if (this._pollTimer !== undefined) {
      clearTimeout(this._pollTimer);
      this._pollTimer = undefined;
    }
    this._listeners.clear();
  }
  addEventListener(type, listener) {
    if (type === 'message' && typeof listener === 'function') {
      this._listeners.add(listener);
      this.start();
    }
  }
  removeEventListener(type, listener) {
    if (type === 'message') {
      this._listeners.delete(listener);
    }
  }
  on(type, listener) {
    this.addEventListener(type, listener);
    return this;
  }
  off(type, listener) {
    this.removeEventListener(type, listener);
    return this;
  }
  _poll() {
    if (!this._polling || this._closed || typeof nativeWorker?.drainMessages !== 'function') {
      return;
    }
    for (const event of nativeWorker.drainMessages()) {
      if (event.type === 'message') {
        dispatchMessage(this, event.data);
      }
    }
    this._pollTimer = setTimeout(() => {
      this._pollTimer = undefined;
      this._poll();
    }, 1);
  }
}

export const parentPort = isMainThread || !nativeWorkerIsAvailable ? null : new ParentPort();

const normalizeWorkerFilename = (filename) => {
  if (filename instanceof URL) {
    if (filename.protocol !== 'file:') {
      throw new Error(`worker_threads.Worker only supports file: URLs, got ${filename.protocol}`);
    }
    return decodeURIComponent(filename.pathname);
  }
  if (typeof filename === 'string') {
    return filename;
  }
  throw new TypeError('Worker filename must be a string or file: URL');
};

export class Worker {
  constructor(filename, options = {}) {
    if (!nativeWorkerIsAvailable || !isMainThread) {
      throw new Error('worker_threads.Worker is not available in this runtime context');
    }
    this._closed = false;
    this._terminated = false;
    this._listeners = new Set();
    this._errorListeners = new Set();
    this._exitListeners = new Set();
    this._onlineListeners = new Set();
    this._nodeListenerMap = new Map();
    this.onmessage = null;
    this.onerror = null;
    this._pollTimer = undefined;
    const workerDataJson = serializeWorkerData(options?.workerData ?? null);
    this._handle = nativeWorkerStart(normalizeWorkerFilename(filename), workerDataJson);
    if (this._handle === undefined || this._handle === null) {
      throw new Error('worker_threads.Worker native start returned no handle');
    }
    this.threadId = Number(this._handle);
    scheduleMessage(() => dispatchOnline(this));
    this._poll();
  }
  postMessage(value, transferList = undefined) {
    if (this._closed) {
      return;
    }
    return nativeWorker.postMessage(this._handle, value, transferList);
  }
  terminate() {
    if (this._terminated) {
      return Promise.resolve(0);
    }
    this._terminated = true;
    this._closed = true;
    if (this._pollTimer !== undefined) {
      clearTimeout(this._pollTimer);
      this._pollTimer = undefined;
    }
    const ok = nativeWorker.terminate(this._handle);
    return Promise.resolve(ok ? 0 : 1);
  }
  ref() {
    nativeWorker.ref?.(this._handle);
    return this;
  }
  unref() {
    nativeWorker.unref?.(this._handle);
    return this;
  }
  addEventListener(type, listener) {
    if (typeof listener !== 'function') {
      return;
    }
    if (type === 'message') {
      this._listeners.add(listener);
    } else if (type === 'error') {
      this._errorListeners.add(listener);
    } else if (type === 'exit') {
      this._exitListeners.add(listener);
    } else if (type === 'online') {
      this._onlineListeners.add(listener);
    }
  }
  removeEventListener(type, listener) {
    if (type === 'message') {
      this._listeners.delete(listener);
    } else if (type === 'error') {
      this._errorListeners.delete(listener);
    } else if (type === 'exit') {
      this._exitListeners.delete(listener);
    } else if (type === 'online') {
      this._onlineListeners.delete(listener);
    }
  }
  on(type, listener) {
    if (typeof listener !== 'function') {
      return this;
    }
    if (type === 'message') {
      const wrapped = (event) => listener(event.data);
      this._nodeListenerMap.set(listener, wrapped);
      this.addEventListener(type, wrapped);
      return this;
    }
    if (type === 'error') {
      const wrapped = (event) => listener(event.error ?? new Error(event.message));
      this._nodeListenerMap.set(listener, wrapped);
      this.addEventListener(type, wrapped);
      return this;
    }
    if (type === 'online') {
      const wrapped = () => listener();
      this._nodeListenerMap.set(listener, wrapped);
      this.addEventListener(type, wrapped);
      return this;
    }
    this.addEventListener(type, listener);
    return this;
  }
  off(type, listener) {
    const wrapped = this._nodeListenerMap.get(listener);
    if (wrapped !== undefined) {
      this._nodeListenerMap.delete(listener);
      this.removeEventListener(type, wrapped);
      return this;
    }
    this.removeEventListener(type, listener);
    return this;
  }
  once(type, listener) {
    const wrapped = (...args) => {
      this.off(type, wrapped);
      listener(...args);
    };
    return this.on(type, wrapped);
  }
  _poll() {
    if (this._closed || typeof nativeWorker?.drainMessages !== 'function') {
      return;
    }
    let sawExit = false;
    for (const event of nativeWorker.drainMessages(this._handle)) {
      if (event.type === 'message') {
        dispatchMessage(this, event.data);
      } else if (event.type === 'error') {
        dispatchError(this, new Error(event.message));
      } else if (event.type === 'exit') {
        sawExit = true;
        dispatchExit(this, Number(event.exitCode ?? 0));
      }
    }
    if (sawExit) {
      this._closed = true;
      if (this._pollTimer !== undefined) {
        clearTimeout(this._pollTimer);
        this._pollTimer = undefined;
      }
      if (!this._terminated) {
        this._terminated = true;
        nativeWorker.terminate(this._handle);
      }
      return;
    }
    this._pollTimer = setTimeout(() => {
      this._pollTimer = undefined;
      this._poll();
    }, 1);
  }
}

export default {
  isMainThread,
  threadId,
  parentPort,
  workerData,
  MessageChannel,
  MessagePort,
  BroadcastChannel,
  capabilities,
  Worker,
};
