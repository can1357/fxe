
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

const isUrlLike = (value) => value && typeof value === 'object' && typeof value.href === 'string';
const encodeAuth = (auth) => auth ? `Basic ${btoa(String(auth))}` : undefined;

const normalizeHeaders = (headers) => {
  const out = {};
  if (!headers) return out;
  if (typeof headers.forEach === 'function') {
    headers.forEach((value, key) => { out[String(key).toLowerCase()] = String(value); });
    return out;
  }
  if (Array.isArray(headers)) {
    for (const [key, value] of headers) out[String(key).toLowerCase()] = String(value);
    return out;
  }
  for (const key of Object.keys(headers)) {
    const value = headers[key];
    if (Array.isArray(value)) out[key.toLowerCase()] = value.map(String).join(', ');
    else if (value !== undefined) out[key.toLowerCase()] = String(value);
  }
  return out;
};

const optionsFrom = (input, options = {}) => {
  const base = {};
  if (typeof input === 'string' || isUrlLike(input)) {
    const url = new URL(String(input));
    base.protocol = url.protocol;
    base.hostname = url.hostname;
    base.port = url.port;
    base.path = `${url.pathname || '/'}${url.search || ''}`;
    base.auth = url.username ? `${decodeURIComponent(url.username)}:${decodeURIComponent(url.password)}` : undefined;
  } else if (input && typeof input === 'object') {
    Object.assign(base, input);
  }
  if (options && typeof options === 'object') Object.assign(base, options);
  const protocol = String(base.protocol ?? 'https:');
  if (protocol !== 'https:') throw new Error(`node:https.request requires https: protocol, got ${protocol}`);
  const hostname = String(base.hostname ?? base.host ?? 'localhost').replace(/^\[|\]$/g, '');
  const port = base.port ? `:${base.port}` : '';
  const path = String(base.path ?? `${base.pathname ?? '/'}${base.search ?? ''}`);
  const method = String(base.method ?? 'GET').toUpperCase();
  const headers = normalizeHeaders(base.headers);
  const auth = encodeAuth(base.auth);
  if (auth && headers.authorization === undefined) headers.authorization = auth;
  return { url: `https://${hostname}${port}${path.startsWith('/') ? path : `/${path}`}`, method, headers, agent: base.agent };
};

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

export class Agent {
  constructor(options = {}) {
    this.options = { ...options };
    this.protocol = 'https:';
    this.defaultPort = 443;
    this.keepAlive = Boolean(options.keepAlive);
  }
  destroy() {}
}

export const globalAgent = new Agent();

export class IncomingMessage extends Emitter {
  constructor(response) {
    super();
    this.statusCode = response.status;
    this.statusMessage = response.statusText ?? '';
    this.headers = {};
    this.rawHeaders = [];
    response.headers?.forEach?.((value, key) => {
      const lower = String(key).toLowerCase();
      this.headers[lower] = String(value);
      this.rawHeaders.push(String(key), String(value));
    });
    this.url = response.url ?? '';
    this.complete = false;
    this.readable = true;
    this._response = response;
  }
  setEncoding() { return this; }
  resume() { return this; }
  async _drain() {
    try {
      const bytes = new Uint8Array(await this._response.arrayBuffer());
      if (bytes.byteLength > 0) this.emit('data', Buffer.from(bytes));
      this.complete = true;
      this.readable = false;
      this.emit('end');
      this.emit('close');
    } catch (error) {
      this.emit('error', error);
      this.emit('close');
    }
  }
}

export class ClientRequest extends Emitter {
  constructor(input, options, callback) {
    super();
    this._options = optionsFrom(input, options);
    this._chunks = [];
    this._ended = false;
    this._headers = { ...this._options.headers };
    this.method = this._options.method;
    this.path = new URL(this._options.url).pathname;
    if (typeof callback === 'function') this.once('response', callback);
  }
  setHeader(name, value) { this._headers[String(name).toLowerCase()] = String(value); return this; }
  getHeader(name) { return this._headers[String(name).toLowerCase()]; }
  removeHeader(name) { delete this._headers[String(name).toLowerCase()]; return this; }
  write(chunk, encoding, callback) {
    if (this._ended) throw new Error('write after end');
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    if (typeof encoding === 'function') defer(encoding);
    else if (typeof callback === 'function') defer(callback);
    return true;
  }
  end(chunk, encoding, callback) {
    if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
    if (typeof callback === 'function') this.once('finish', callback);
    if (this._ended) return this;
    this._ended = true;
    defer(() => this._dispatch());
    return this;
  }
  abort() { return this.destroy(new Error('request aborted')); }
  destroy(error) {
    if (error) defer(() => this.emit('error', error));
    defer(() => this.emit('close'));
    return this;
  }
  async _dispatch() {
    this.emit('finish');
    try {
      if (typeof fetch !== 'function') throw new Error('node:https requires host-backed fetch');
      const body = concatBody(this._chunks);
      const init = { method: this.method, headers: this._headers, credentials: 'include' };
      if (body !== undefined && this.method !== 'GET' && this.method !== 'HEAD') init.body = body;
      const response = await fetch(this._options.url, init);
      const message = new IncomingMessage(response);
      this.emit('response', message);
      defer(() => { void message._drain(); });
    } catch (error) {
      this.emit('error', error);
      this.emit('close');
    }
  }
}

export const request = (input, options, callback) => {
  if (typeof options === 'function') { callback = options; options = {}; }
  return new ClientRequest(input, options, callback);
};

export const get = (input, options, callback) => {
  const req = request(input, options, callback);
  req.end();
  return req;
};

export const createServer = () => {
  throw new Error('node:https.createServer requires a native TLS server implementation, which FXE does not provide');
};

export default {
  Agent,
  ClientRequest,
  IncomingMessage,
  globalAgent,
  request,
  get,
  createServer,
};
