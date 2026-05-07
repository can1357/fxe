
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
  if (chunks.length === 0) return undefined;
  if (chunks.every((chunk) => typeof chunk === 'string')) return chunks.join('');
  let total = 0;
  const arrays = chunks.map((chunk) => {
    if (typeof chunk === 'string') return textEncoder.encode(chunk);
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    return textEncoder.encode(String(chunk));
  });
  for (const chunk of arrays) total += chunk.byteLength;
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of arrays) { out.set(chunk, offset); offset += chunk.byteLength; }
  return out;
};

const normalizeAuthority = (authority) => {
  const url = authority && typeof authority === 'object' && typeof authority.href === 'string'
    ? new URL(authority.href)
    : new URL(String(authority));
  if (url.protocol !== 'https:' && url.protocol !== 'http:') {
    throw new Error(`node:http2.connect requires http(s) authority, got ${url.protocol}`);
  }
  return url;
};

export class ClientHttp2Stream extends Emitter {
  constructor(session, headers = {}) {
    super();
    this.session = session;
    this.closed = false;
    this.destroyed = false;
    this._headers = { ...headers };
    this._chunks = [];
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
  destroy(error) {
    if (this.destroyed) return this;
    this.closed = true;
    this.destroyed = true;
    if (error) this.emit('error', error);
    defer(() => this.emit('close'));
    return this;
  }
  respond() { throw new Error('node:http2 server stream APIs are not implemented'); }
  async _dispatch() {
    if (this.destroyed) return;
    this.emit('finish');
    try {
      if (typeof fetch !== 'function') throw new Error('node:http2 client requests require host-backed fetch');
      const method = String(this._headers[constants.HTTP2_HEADER_METHOD] ?? 'GET').toUpperCase();
      const path = String(this._headers[constants.HTTP2_HEADER_PATH] ?? '/');
      const url = new URL(path, this.session.origin).href;
      const headers = {};
      for (const [key, value] of Object.entries(this._headers)) {
        if (!String(key).startsWith(':')) headers[key] = String(value);
      }
      const init = { method, headers };
      const body = concatBody(this._chunks);
      if (body !== undefined && method !== 'GET' && method !== 'HEAD') init.body = body;
      const response = await fetch(url, init);
      const responseHeaders = { [constants.HTTP2_HEADER_STATUS]: response.status };
      response.headers?.forEach?.((value, key) => { responseHeaders[String(key).toLowerCase()] = String(value); });
      this.emit('response', responseHeaders, 0);
      const bytes = new Uint8Array(await response.arrayBuffer());
      if (bytes.byteLength > 0) this.emit('data', Buffer.from(bytes));
      this.closed = true;
      this.emit('end');
      this.emit('close');
    } catch (error) {
      this.destroy(error);
    }
  }
}

export class ClientHttp2Session extends Emitter {
  constructor(authority, options = {}, listener) {
    super();
    this._url = normalizeAuthority(authority);
    this.origin = this._url.origin;
    this.authority = this._url.host;
    this.closed = false;
    this.destroyed = false;
    this.encrypted = this._url.protocol === 'https:';
    this.options = { ...options };
    if (typeof listener === 'function') this.once('connect', listener);
    defer(() => { if (!this.destroyed) this.emit('connect', this, undefined); });
  }
  request(headers = {}, options = {}) {
    if (this.closed || this.destroyed) throw new Error('node:http2 session is closed');
    const stream = new ClientHttp2Stream(this, headers, options);
    return stream;
  }
  close(callback) {
    if (typeof callback === 'function') this.once('close', callback);
    this.closed = true;
    defer(() => this.emit('close'));
    return this;
  }
  destroy(error) {
    if (this.destroyed) return this;
    this.closed = true;
    this.destroyed = true;
    if (error) this.emit('error', error);
    defer(() => this.emit('close'));
    return this;
  }
  setTimeout() { return this; }
}

export const connect = (authority, options, listener) => {
  if (typeof options === 'function') { listener = options; options = {}; }
  return new ClientHttp2Session(authority, options, listener);
};

export const createServer = () => {
  throw new Error('node:http2.createServer requires a native HTTP/2 server implementation, which FXE does not provide');
};
export const createSecureServer = () => {
  throw new Error('node:http2.createSecureServer requires a native HTTP/2 server implementation, which FXE does not provide');
};

export default {
  constants,
  connect,
  createServer,
  createSecureServer,
  ClientHttp2Session,
  ClientHttp2Stream,
};
