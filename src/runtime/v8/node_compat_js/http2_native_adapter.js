const native = globalThis.__fxe_native?.http2;
const defer = (fn) => typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0);
const textEncoder = new TextEncoder();
const schedulePoll = (fn) => typeof setImmediate === 'function' ? setImmediate(fn) : setTimeout(fn, 0);

const requireNative = () => {
  if (!native || native.notImplemented || typeof native.connect !== 'function') {
    throw new Error('host-backed node:http2 native implementation unavailable');
  }
  return native;
};

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

export const constants = Object.freeze({
  HTTP2_HEADER_METHOD: ':method',
  HTTP2_HEADER_PATH: ':path',
  HTTP2_HEADER_STATUS: ':status',
  HTTP2_HEADER_AUTHORITY: ':authority',
  HTTP2_HEADER_SCHEME: ':scheme',
  HTTP2_HEADER_CONTENT_TYPE: 'content-type',
  HTTP2_HEADER_CONTENT_LENGTH: 'content-length',
  HTTP_STATUS_OK: 200,
  HTTP_STATUS_CREATED: 201,
  HTTP_STATUS_ACCEPTED: 202,
  HTTP_STATUS_NO_CONTENT: 204,
  HTTP_STATUS_MOVED_PERMANENTLY: 301,
  HTTP_STATUS_FOUND: 302,
  HTTP_STATUS_BAD_REQUEST: 400,
  HTTP_STATUS_UNAUTHORIZED: 401,
  HTTP_STATUS_FORBIDDEN: 403,
  HTTP_STATUS_NOT_FOUND: 404,
  HTTP_STATUS_INTERNAL_SERVER_ERROR: 500,
});

const concatBody = (chunks) => {
  if (chunks.length === 0) return new Uint8Array(0);
  const arrays = chunks.map((chunk) => {
    if (typeof chunk === 'string') return textEncoder.encode(chunk);
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    return textEncoder.encode(String(chunk));
  });
  const total = arrays.reduce((sum, chunk) => sum + chunk.byteLength, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of arrays) { out.set(chunk, offset); offset += chunk.byteLength; }
  return out;
};

const responseHeaders = (result) => ({
  [constants.HTTP2_HEADER_STATUS]: result.status,
  ...(result.headers ?? {}),
});

const abortError = (signal) => {
  const reason = signal?.reason;
  if (reason && typeof reason === 'object' && (reason.name === 'AbortError' || reason.code === 'ABORT_ERR')) return reason;
  const error = new Error(typeof reason === 'string' ? reason : 'The operation was aborted');
  error.name = 'AbortError';
  error.code = 'ABORT_ERR';
  return error;
};

export class ClientHttp2Stream extends Emitter {
  constructor(session, headers = {}, options = {}) {
    super();
    this.session = session;
    this.closed = false;
    this.destroyed = false;
    this._headers = { ...headers };
    this._options = { ...options };
    this._chunks = [];
    this._streamId = 0;
    this._readHandle = 0;
    this._aborted = false;
    this._abortCleanup = undefined;
    this._closeEmitted = false;
  }
  write(chunk, encoding, callback) {
    if (this.closed) throw new Error('write after end');
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    if (typeof encoding === 'function') defer(encoding);
    else if (typeof callback === 'function') defer(callback);
    return true;
  }
  end(chunk, encoding, callback) {
    if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
    if (typeof callback === 'function') this.once('finish', callback);
    defer(() => this._dispatch());
    return this;
  }
  close() { return this.destroy(); }

  _emitCloseOnce() {
    if (this._closeEmitted) return;
    this._closeEmitted = true;
    defer(() => this.emit('close'));
  }
  destroy(error) {
    if (this.destroyed) return this;
    this._abortCleanup?.();
    this._abortCleanup = undefined;
    this.closed = true;
    this.destroyed = true;
    if (error) this.emit('error', error);
    this._emitCloseOnce();
    return this;
  }
  respond() { throw new Error('ClientHttp2Stream.respond is only available on server streams'); }
  _finishWith(result) {
    if (this.destroyed) return;
    this._abortCleanup?.();
    this._abortCleanup = undefined;
    if (this._aborted) {
      this.destroyed = true;
      this._emitCloseOnce();
      return;
    }
    if (!result?.ok) {
      if (result?.code === 'ABORT_ERR') {
        this.destroy(abortError(this._options.signal));
        return;
      }
      this.destroy(new Error(result?.error ?? 'HTTP/2 native read failed'));
      return;
    }
    this.emit('response', responseHeaders(result), 0);
    const body = result.body instanceof Uint8Array ? result.body : new Uint8Array(0);
    if (body.byteLength > 0) this.emit('data', Buffer.from(body));
    this.closed = true;
    this.destroyed = true;
    this.emit('end');
    this._emitCloseOnce();
  }
  _dispatch() {
    if (this.destroyed) return;
    if (this._options.signal?.aborted) {
      this.destroy(abortError(this._options.signal));
      return;
    }
    this.emit('finish');
    try {
      const n = requireNative();
      const body = concatBody(this._chunks);
      const timeout = Number(this._options.timeout ?? this._options.timeoutMs ?? 0);
      this._streamId = n.submit(this.session._handle, {
        ...this._headers,
        __body: body,
        __timeoutMs: Number.isFinite(timeout) && timeout > 0 ? Math.trunc(timeout) : 0,
      });
      const onAbort = () => {
        if (this.destroyed || this._aborted) return;
        this._aborted = true;
        this.closed = true;
        this.emit('error', abortError(this._options.signal));
        this._emitCloseOnce();
        // Keep polling after abort so readResult can observe native cancel completion,
        // join the worker thread, and erase the native read handle without delivering a response.
        defer(() => { try { n.cancel(this.session._handle, this._streamId); } catch {} });
      };
      if (this._options.signal?.addEventListener) {
        this._abortCleanup = () => this._options.signal.removeEventListener('abort', onAbort);
        this._options.signal.addEventListener('abort', onAbort, { once: true });
      }
      this._readHandle = n.read(this.session._handle, this._streamId, () => {});
      const poll = () => {
        if (this.destroyed) return;
        const result = n.readResult(this._readHandle);
        if (result === null) {
          schedulePoll(poll);
          return;
        }
        this._finishWith(result);
      };
      poll();
    } catch (error) {
      this.destroy(error);
    }
  }
}

export class ClientHttp2Session extends Emitter {
  constructor(authority, options = {}, listener) {
    super();
    this.authority = String(authority);
    this.options = { ...options };
    this.closed = false;
    this.destroyed = false;
    this.encrypted = this.authority.startsWith('https:');
    this._handle = requireNative().connect(this.authority, this.options);
    if (typeof listener === 'function') this.once('connect', listener);
    defer(() => { if (!this.destroyed) this.emit('connect', this, undefined); });
  }
  request(headers = {}, options = {}) {
    if (this.closed || this.destroyed) throw new Error('node:http2 session is closed');
    return new ClientHttp2Stream(this, headers, options);
  }
  close(callback) {
    if (typeof callback === 'function') this.once('close', callback);
    if (!this.closed) {
      this.closed = true;
      try { requireNative().close(this._handle); } catch {}
    }
    defer(() => this.emit('close'));
    return this;
  }
  destroy(error) {
    if (this.destroyed) return this;
    this.closed = true;
    this.destroyed = true;
    try { requireNative().close(this._handle); } catch {}
    if (error) this.emit('error', error);
    defer(() => this.emit('close'));
    return this;
  }
  setTimeout() { return this; }
}

export class ServerHttp2Stream extends Emitter {
  constructor(server, request) {
    super();
    this.server = server;
    this.id = request.id;
    this.closed = false;
    this.destroyed = false;
    this.headers = request.headers ?? {};
    this.method = request.method;
    this.path = request.path;
    this._responseHeaders = { [constants.HTTP2_HEADER_STATUS]: constants.HTTP_STATUS_OK };
    this._chunks = [];
  }
  respond(headers = {}) {
    this._responseHeaders = { ...this._responseHeaders, ...headers };
    return this;
  }
  write(chunk, encoding, callback) {
    if (this.closed) throw new Error('write after end');
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    if (typeof encoding === 'function') defer(encoding);
    else if (typeof callback === 'function') defer(callback);
    return true;
  }
  end(chunk, encoding, callback) {
    if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
    if (typeof callback === 'function') this.once('finish', callback);
    try {
      requireNative().serverRespond(this.server._handle, this.id, this._responseHeaders, concatBody(this._chunks));
      this.closed = true;
      this.destroyed = true;
      this.emit('finish');
      this.emit('close');
    } catch (error) {
      this.destroy(error);
    }
    return this;
  }
  close() { return this.destroy(); }
  destroy(error) {
    if (this.destroyed) return this;
    this.closed = true;
    this.destroyed = true;
    if (error) this.emit('error', error);
    defer(() => this.emit('close'));
    return this;
  }
}

export class Http2SecureServer extends Emitter {
  constructor(options = {}, listener) {
    super();
    this.options = { ...options };
    this._handle = 0;
    this._address = null;
    this.listening = false;
    this.closed = false;
    if (typeof listener === 'function') this.on('stream', listener);
  }
  listen(port = 0, host, callback) {
    if (typeof port === 'object' && port !== null) {
      callback = typeof host === 'function' ? host : port.callback;
      port = Number(port.port ?? 0);
    } else if (typeof host === 'function') {
      callback = host;
    }
    if (typeof callback === 'function') this.once('listening', callback);
    const n = requireNative();
    this._handle = n.createServer({ ...this.options, port: Number(port ?? 0) });
    this._address = n.serverAddress(this._handle);
    this.listening = true;
    this.closed = false;
    this._poll();
    defer(() => this.emit('listening'));
    return this;
  }
  address() { return this._address; }
  close(callback) {
    if (typeof callback === 'function') this.once('close', callback);
    if (!this.closed) {
      this.closed = true;
      this.listening = false;
      try { if (this._handle) requireNative().serverClose(this._handle); } catch {}
    }
    defer(() => this.emit('close'));
    return this;
  }
  _poll() {
    if (this.closed || !this._handle) return;
    try {
      let request;
      while ((request = requireNative().serverPoll(this._handle)) !== null) {
        const stream = new ServerHttp2Stream(this, request);
        const headers = {
          [constants.HTTP2_HEADER_METHOD]: request.method,
          [constants.HTTP2_HEADER_PATH]: request.path,
          ...(request.headers ?? {}),
        };
        this.emit('stream', stream, headers, 0);
      }
    } catch (error) {
      this.emit('error', error);
      this.close();
      return;
    }
    setTimeout(() => this._poll(), 1);
  }
}

export const connect = (authority, options, listener) => {
  if (typeof options === 'function') { listener = options; options = {}; }
  return new ClientHttp2Session(authority, options ?? {}, listener);
};

export const createServer = () => {
  throw new Error('node:http2.createServer requires cleartext HTTP/2, which FXE does not provide');
};

export const createSecureServer = (options = {}, listener) => new Http2SecureServer(options, listener);

export default {
  constants,
  connect,
  createServer,
  createSecureServer,
  ClientHttp2Session,
  ClientHttp2Stream,
  Http2SecureServer,
};
