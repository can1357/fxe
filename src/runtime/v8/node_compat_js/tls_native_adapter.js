
const native = globalThis.__fxe_native?.tls;
const defer = (fn) => typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0);
const textEncoder = new TextEncoder();

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

const nativeRoots = (() => {
  try {
    return typeof native?.rootCertificates === 'function' ? native.rootCertificates() : [];
  } catch {
    return [];
  }
})();

export const rootCertificates = Object.freeze(Array.isArray(nativeRoots) ? [...nativeRoots] : []);
export const getCACertificates = () => [...rootCertificates];

export class SecureContext {
  constructor(opts = {}) {
    this._opts = opts && typeof opts === 'object' ? { ...opts } : {};
    this.context = this._opts;
    this._native = typeof native?.createSecureContext === 'function'
      ? native.createSecureContext(this._opts)
      : undefined;
  }
}

export function createSecureContext(opts = {}) {
  return new SecureContext(opts);
}

const normalizeConnectArgs = (args) => {
  const opts = {};
  let callback;
  if (typeof args[0] === 'object' && args[0] !== null) {
    Object.assign(opts, args[0]);
    if (typeof args[1] === 'function') callback = args[1];
  } else {
    opts.port = args[0];
    if (typeof args[1] === 'string') opts.host = args[1];
    else if (typeof args[1] === 'object' && args[1] !== null) Object.assign(opts, args[1]);
    if (typeof args[2] === 'object' && args[2] !== null) Object.assign(opts, args[2]);
    callback = [args[1], args[2], args[3]].find((value) => typeof value === 'function');
  }
  opts.host = String(opts.host ?? opts.hostname ?? 'localhost');
  opts.hostname = String(opts.hostname ?? opts.host);
  opts.port = Number(opts.port ?? 443);
  opts.servername = opts.servername ?? opts.host;
  if (opts.secureContext instanceof SecureContext) {
    Object.assign(opts, opts.secureContext._opts);
  }
  return { options: opts, callback };
};

const toUint8Array = (data, encoding) => {
  if (data === undefined || data === null) return new Uint8Array();
  if (typeof data === 'string') return textEncoder.encode(data);
  if (typeof Buffer === 'function' && Buffer.isBuffer?.(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  return textEncoder.encode(String(data), encoding);
};

const toChunk = (data) => {
  if (typeof Buffer === 'function') return Buffer.from(data);
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  return data;
};

const nativeTlsError = (message) => {
  const suffix = message ? `: ${message}` : '';
  const err = new Error(`FXE native TLS socket backing failed${suffix}`);
  err.code = 'ERR_FXE_TLS_NATIVE';
  return err;
};

export class TLSSocket extends Emitter {
  constructor(socket = undefined, options = {}) {
    super();
    this.authorized = false;
    this.authorizationError = null;
    this.encrypted = true;
    this.connecting = false;
    this.destroyed = false;
    this.servername = options.servername ?? options.host ?? options.hostname ?? null;
    this.alpnProtocol = null;
    this._socket = socket;
    this._options = options && typeof options === 'object' ? { ...options } : {};
    this._handle = undefined;
    this._closed = false;
    this._connected = false;
    this._pendingWrites = [];
    this._hadError = false;
  }
  connect(...args) {
    const { options, callback } = normalizeConnectArgs(args.length > 0 ? args : [this._options]);
    this._options = { ...this._options, ...options };
    this.servername = this._options.servername ?? this._options.host ?? this.servername;
    if (callback) this.once('secureConnect', callback);
    if (!native || typeof native.connect !== 'function') {
      defer(() => {
        if (this._closed) return;
        this._onNativeError('node:tls.connect requires FXE native TLS socket backing');
        this._finishClose(true);
      });
      return this;
    }
    this.connecting = true;
    try {
      this._handle = native.connect(
        this._options,
        (info = {}) => this._onNativeConnect(info),
        (message) => this._onNativeError(message),
        (data) => this._onNativeData(data),
        () => this._finishClose(false),
      );
    } catch (error) {
      defer(() => {
        this._onNativeError(error?.message ?? String(error));
        this._finishClose(true);
      });
    }
    return this;
  }
  _onNativeConnect(info = {}) {
    if (this._closed) return;
    this.connecting = false;
    this._connected = true;
    this.authorized = true;
    this.authorizationError = null;
    this.alpnProtocol = info.alpnProtocol || false;
    this.emit('secureConnect');
    for (const { data, callback } of this._pendingWrites.splice(0)) {
      this._writeNow(data, callback);
    }
  }
  _onNativeError(message) {
    if (this._closed) return;
    const err = message instanceof Error ? message : nativeTlsError(String(message ?? 'TLS socket error'));
    this.authorizationError = err.message;
    this._hadError = true;
    this.emit('error', err);
  }
  _onNativeData(data) {
    if (this._closed) return;
    this.emit('data', toChunk(data));
  }
  _writeNow(data, callback) {
    try {
      if (this._handle === undefined || !native?.write?.(this._handle, data)) {
        throw nativeTlsError('TLS socket is not connected');
      }
      if (typeof callback === 'function') defer(callback);
    } catch (error) {
      if (typeof callback === 'function') defer(() => callback(error));
      this.destroy(error);
    }
  }
  write(data, encoding, callback) {
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    if (this._closed) throw new Error('node:tls.TLSSocket.write on closed socket');
    const bytes = toUint8Array(data, encoding);
    if (!this._connected) this._pendingWrites.push({ data: bytes, callback });
    else this._writeNow(bytes, callback);
    return true;
  }
  end(data, encoding, callback) {
    if (typeof data === 'function') { callback = data; data = undefined; }
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    if (data !== undefined && data !== null) this.write(data, encoding);
    if (typeof callback === 'function') this.once('close', callback);
    return this.destroy();
  }
  destroy(error) {
    if (this._closed) return this;
    if (error) {
      this._hadError = true;
      this.emit('error', error);
    }
    if (this._handle !== undefined && typeof native?.close === 'function') {
      try { native.close(this._handle); } catch {}
    }
    this._finishClose(Boolean(error));
    return this;
  }
  _finishClose(hadError) {
    if (this._closed) return;
    this._closed = true;
    this.destroyed = true;
    this.connecting = false;
    this._connected = false;
    this._handle = undefined;
    this._pendingWrites.length = 0;
    this.emit('close', Boolean(hadError || this._hadError));
  }
  getPeerCertificate() { return {}; }
  getCipher() { return null; }
  setTimeout() { return this; }
  setNoDelay() { return this; }
  setKeepAlive() { return this; }
  pause() { return this; }
  resume() { return this; }
}

export function connect(...args) {
  const { options, callback } = normalizeConnectArgs(args);
  return new TLSSocket(undefined, options).connect(options, callback);
}

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
