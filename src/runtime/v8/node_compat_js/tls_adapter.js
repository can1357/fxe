
const native = globalThis.__fxe_native?.tls;
const defer = (fn) => typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0);

class Emitter {
  constructor() { this._events = new Map(); }
  on(name, fn) {
    if (typeof fn !== 'function') return this;
    const list = this._events.get(name) ?? [];
    list.push(fn);
    this._events.set(name, list);
    return this;
  }
  once(name, fn) {
    if (typeof fn !== 'function') return this;
    const wrapped = (...args) => { this.off(name, wrapped); fn(...args); };
    return this.on(name, wrapped);
  }
  off(name, fn) {
    const list = this._events.get(name);
    if (!list) return this;
    const next = list.filter((item) => item !== fn);
    if (next.length === 0) this._events.delete(name);
    else this._events.set(name, next);
    return this;
  }
  removeListener(name, fn) { return this.off(name, fn); }
  emit(name, ...args) {
    const list = this._events.get(name);
    if (!list || list.length === 0) return false;
    for (const fn of [...list]) fn(...args);
    return true;
  }
}

export const rootCertificates = Object.freeze([]);
export const getCACertificates = () => [...rootCertificates];

export class SecureContext {
  constructor(options = {}) {
    this.context = { ...options };
  }
}

export const createSecureContext = (options = {}) => new SecureContext(options);

const normalizeConnectArgs = (args) => {
  const opts = {};
  let callback;
  if (typeof args[0] === 'object' && args[0] !== null) Object.assign(opts, args[0]);
  else {
    opts.port = args[0];
    if (typeof args[1] === 'string') opts.host = args[1];
    if (typeof args[1] === 'function') callback = args[1];
    else if (typeof args[2] === 'function') callback = args[2];
  }
  if (typeof args[1] === 'function' && callback === undefined) callback = args[1];
  return { options: opts, callback };
};

export class TLSSocket extends Emitter {
  constructor(socket = undefined, options = {}) {
    super();
    this.authorized = false;
    this.authorizationError = null;
    this.encrypted = true;
    this.servername = options.servername ?? options.host ?? null;
    this._socket = socket;
    this._closed = false;
  }
  connect(...args) {
    const { options, callback } = normalizeConnectArgs(args);
    if (callback) this.once('secureConnect', callback);
    this.servername = options.servername ?? options.host ?? this.servername;
    if (!native || typeof native.connect !== 'function') {
      defer(() => {
        if (this._closed) return;
        const err = new Error('node:tls.connect requires FXE native TLS socket backing');
        err.code = 'ERR_FXE_TLS_UNAVAILABLE';
        this.authorizationError = err.message;
        this.emit('error', err);
        this.destroy();
      });
      return this;
    }
    defer(() => {
      try {
        const result = native.connect(options);
        if (result?.error) throw new Error(result.error);
        this.authorized = true;
        this.authorizationError = null;
        this.emit('secureConnect');
      } catch (error) {
        this.authorizationError = error?.message ?? String(error);
        this.emit('error', error);
        this.destroy();
      }
    });
    return this;
  }
  write(_data, callback) {
    if (typeof callback === 'function') defer(callback);
    if (!native) throw new Error('node:tls.TLSSocket.write requires FXE native TLS socket backing');
    return true;
  }
  end(_data, callback) {
    if (typeof callback === 'function') defer(callback);
    return this.destroy();
  }
  destroy(error) {
    if (this._closed) return this;
    this._closed = true;
    if (error) this.emit('error', error);
    defer(() => this.emit('close', Boolean(error)));
    return this;
  }
  getPeerCertificate() { return {}; }
  getCipher() { return null; }
  setTimeout() { return this; }
  setNoDelay() { return this; }
  setKeepAlive() { return this; }
}

export const connect = (...args) => new TLSSocket(undefined, normalizeConnectArgs(args).options).connect(...args);
export const createConnection = connect;
export const checkServerIdentity = () => undefined;
export const createServer = () => {
  throw new Error('node:tls.createServer requires a native TLS server implementation, which FXE does not provide');
};

export default {
  TLSSocket,
  SecureContext,
  rootCertificates,
  getCACertificates,
  createSecureContext,
  connect,
  createConnection,
  checkServerIdentity,
  createServer,
};
