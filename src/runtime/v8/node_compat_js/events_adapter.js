const kEvents = Symbol('fxe.events');
const kMaxListeners = Symbol('fxe.maxListeners');
const kCapture = Symbol('fxe.captureRejections');

let defaultMax = 10;

const eventKey = (name) => (typeof name === 'symbol' ? name : String(name));
const validateListener = (listener) => {
  if (typeof listener !== 'function') {
    throw new TypeError('listener must be a function');
  }
};
const listenerStore = (target) => target[kEvents] || (target[kEvents] = new Map());
const listenersFor = (target, name, create = false) => {
  const events = create ? listenerStore(target) : target[kEvents];
  if (!events) return undefined;
  const key = eventKey(name);
  let list = events.get(key);
  if (!list && create) {
    list = [];
    events.set(key, list);
  }
  return list;
};

export class EventEmitter {
  constructor(options = undefined) {
    Object.defineProperty(this, kEvents, { configurable: true, value: new Map() });
    this[kMaxListeners] = undefined;
    this[kCapture] = Boolean(options && options.captureRejections);
  }
  static get defaultMaxListeners() { return defaultMax; }
  static set defaultMaxListeners(value) {
    const n = Number(value);
    if (!Number.isFinite(n) || n < 0) throw new RangeError('defaultMaxListeners must be a non-negative number');
    defaultMax = n;
  }
  static listenerCount(emitter, eventName, listener = undefined) {
    return emitter.listenerCount(eventName, listener);
  }
  static on(emitter, eventName, options = undefined) {
    return on(emitter, eventName, options);
  }
  static once(emitter, eventName, options = undefined) {
    return once(emitter, eventName, options);
  }
  static getEventListeners(emitter, eventName) {
    return typeof emitter.listeners === 'function' ? emitter.listeners(eventName) : [];
  }
  static setMaxListeners(n, ...targets) {
    if (targets.length === 0) {
      EventEmitter.defaultMaxListeners = n;
      return;
    }
    for (const target of targets) {
      if (target && typeof target.setMaxListeners === 'function') target.setMaxListeners(n);
    }
  }
  setMaxListeners(n) {
    const value = Number(n);
    if (!Number.isFinite(value) || value < 0) throw new RangeError('maxListeners must be a non-negative number');
    this[kMaxListeners] = value;
    return this;
  }
  getMaxListeners() {
    return this[kMaxListeners] === undefined ? defaultMax : this[kMaxListeners];
  }
  emit(eventName, ...args) {
    const list = listenersFor(this, eventName);
    if (!list || list.length === 0) {
      if (eventKey(eventName) === 'error') {
        const err = args[0];
        if (err instanceof Error) throw err;
        throw new Error(`Unhandled error.${err === undefined ? '' : ` (${String(err)})`}`);
      }
      return false;
    }
    for (const entry of [...list]) {
      const fn = entry.listener;
      if (entry.once) this.removeListener(eventName, entry.wrapper ?? fn);
      try {
        const result = fn.apply(this, args);
        if (this[kCapture] && result && typeof result.then === 'function') {
          result.catch((error) => this.emit('error', error));
        }
      } catch (error) {
        throw error;
      }
    }
    return true;
  }
  addListener(eventName, listener) { return this.on(eventName, listener); }
  on(eventName, listener) {
    validateListener(listener);
    const list = listenersFor(this, eventName, true);
    list.push({ listener, once: false });
    return this;
  }
  prependListener(eventName, listener) {
    validateListener(listener);
    const list = listenersFor(this, eventName, true);
    list.unshift({ listener, once: false });
    return this;
  }
  once(eventName, listener) {
    validateListener(listener);
    const wrapped = (...args) => listener.apply(this, args);
    Object.defineProperty(wrapped, 'listener', { configurable: true, value: listener });
    const list = listenersFor(this, eventName, true);
    list.push({ listener, once: true, wrapper: wrapped });
    return this;
  }
  prependOnceListener(eventName, listener) {
    validateListener(listener);
    const wrapped = (...args) => listener.apply(this, args);
    Object.defineProperty(wrapped, 'listener', { configurable: true, value: listener });
    const list = listenersFor(this, eventName, true);
    list.unshift({ listener, once: true, wrapper: wrapped });
    return this;
  }
  removeListener(eventName, listener) {
    validateListener(listener);
    const events = this[kEvents];
    const list = listenersFor(this, eventName);
    if (!list) return this;
    for (let i = list.length - 1; i >= 0; --i) {
      const entry = list[i];
      if (entry.listener === listener || entry.wrapper === listener || entry.wrapper?.listener === listener) {
        list.splice(i, 1);
        break;
      }
    }
    if (list.length === 0 && events) events.delete(eventKey(eventName));
    return this;
  }
  off(eventName, listener) { return this.removeListener(eventName, listener); }
  removeAllListeners(eventName = undefined) {
    if (eventName === undefined) this[kEvents].clear();
    else this[kEvents].delete(eventKey(eventName));
    return this;
  }
  listeners(eventName) {
    return (listenersFor(this, eventName) ?? []).map((entry) => entry.listener);
  }
  rawListeners(eventName) {
    return (listenersFor(this, eventName) ?? []).map((entry) => entry.wrapper ?? entry.listener);
  }
  listenerCount(eventName, listener = undefined) {
    const list = listenersFor(this, eventName) ?? [];
    if (listener === undefined) return list.length;
    return list.filter((entry) => entry.listener === listener || entry.wrapper === listener).length;
  }
  eventNames() {
    return [...this[kEvents].keys()];
  }
}

export const addAbortListener = (signal, listener) => {
  validateListener(listener);
  if (!signal || typeof signal.addEventListener !== 'function') throw new TypeError('signal must be an AbortSignal');
  if (signal.aborted) {
    queueMicrotask(listener);
    return { [Symbol.dispose]: () => {} };
  }
  signal.addEventListener('abort', listener, { once: true });
  return { [Symbol.dispose]: () => signal.removeEventListener('abort', listener) };
};

export const once = (emitter, eventName, options = undefined) => new Promise((resolve, reject) => {
  const signal = options && options.signal;
  if (signal?.aborted) {
    reject(signal.reason ?? new Error('aborted'));
    return;
  }
  const cleanup = () => {
    emitter.removeListener(eventName, onEvent);
    if (eventName !== 'error') emitter.removeListener('error', onError);
    signal?.removeEventListener?.('abort', onAbort);
  };
  const onEvent = (...args) => { cleanup(); resolve(args); };
  const onError = (err) => { cleanup(); reject(err); };
  const onAbort = () => { cleanup(); reject(signal.reason ?? new Error('aborted')); };
  emitter.once(eventName, onEvent);
  if (eventName !== 'error') emitter.once('error', onError);
  signal?.addEventListener?.('abort', onAbort, { once: true });
});

export const on = async function* (emitter, eventName, options = undefined) {
  const queue = [];
  const waiters = [];
  let done = false;
  const signal = options && options.signal;
  const push = (args) => {
    const waiter = waiters.shift();
    if (waiter) waiter({ value: args, done: false });
    else queue.push(args);
  };
  const finish = () => {
    done = true;
    for (const waiter of waiters.splice(0)) waiter({ value: undefined, done: true });
  };
  emitter.on(eventName, (...args) => push(args));
  signal?.addEventListener?.('abort', finish, { once: true });
  try {
    while (!done) {
      if (queue.length !== 0) yield queue.shift();
      else {
        const next = await new Promise((resolve) => waiters.push(resolve));
        if (next.done) break;
        yield next.value;
      }
    }
  } finally {
    emitter.removeAllListeners(eventName);
  }
};

export const listenerCount = EventEmitter.listenerCount;
export const getEventListeners = EventEmitter.getEventListeners;
export const setMaxListeners = EventEmitter.setMaxListeners;
export const defaultMaxListeners = EventEmitter.defaultMaxListeners;
export default EventEmitter;
