
const native = globalThis.__fxe_native?.https;
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
  return {
    url: `https://${hostname}${port}${path.startsWith('/') ? path : `/${path}`}`,
    method,
    headers,
    agent: base.agent,
    rejectUnauthorized: base.rejectUnauthorized !== false,
  };
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
    this._opts = { ...options };
    this.options = this._opts;
    this.protocol = 'https:';
    this.defaultPort = 443;
    this.keepAlive = Boolean(options.keepAlive);
  }
  destroy() {}
}

export const globalAgent = new Agent(native?.globalAgent ?? {});

export class IncomingMessage extends Emitter {
  constructor(response) {
    super();
    this.statusCode = response.status ?? response.statusCode ?? 0;
    this.statusMessage = response.statusText ?? response.statusMessage ?? '';
    this.headers = {};
    this.rawHeaders = [];
    if (response.headers && typeof response.headers.forEach === 'function') {
      response.headers.forEach((value, key) => {
        const lower = String(key).toLowerCase();
        this.headers[lower] = String(value);
        this.rawHeaders.push(String(key), String(value));
      });
    } else {
      for (const [key, value] of Object.entries(response.headers ?? {})) {
        const lower = String(key).toLowerCase();
        this.headers[lower] = String(value);
        this.rawHeaders.push(String(key), String(value));
      }
    }
    this.url = response.url ?? '';
    this.complete = false;
    this.readable = true;
    this._response = response;
  }
  setEncoding() { return this; }
  resume() { return this; }
  async _drain() {
    try {
      if (typeof this._response.arrayBuffer === 'function') {
        const bytes = new Uint8Array(await this._response.arrayBuffer());
        if (bytes.byteLength > 0) this.emit('data', Buffer.from(bytes));
      } else if (this._response.body !== undefined && this._response.body !== '') {
        this.emit('data', Buffer.from(String(this._response.body)));
      }
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
  _nativeDispatch(body) {
    if (!native || typeof native.request !== 'function') throw new Error('node:https native request unavailable');
    return native.request(this._options.url, {
      method: this.method,
      headers: this._headers,
      rejectUnauthorized: this._options.rejectUnauthorized,
    }, body ?? '');
  }
  async _dispatch() {
    this.emit('finish');
    const body = concatBody(this._chunks);
    try {
      if (typeof fetch !== 'function') throw new Error('node:https requires host-backed fetch');
      const init = { method: this.method, headers: this._headers };
      if (body !== undefined && this.method !== 'GET' && this.method !== 'HEAD') init.body = body;
      const response = await fetch(this._options.url, init);
      const message = new IncomingMessage(response);
      this.emit('response', message);
      defer(() => { void message._drain(); });
    } catch (fetchError) {
      try {
        const response = this._nativeDispatch(body);
        const message = new IncomingMessage(response);
        this.emit('response', message);
        defer(() => { void message._drain(); });
      } catch (_nativeError) {
        this.emit('error', fetchError);
        this.emit('close');
      }
    }
  }
}

export const request = (input, options, callback) => {
  if (typeof options === 'function') { callback = options; options = {}; }
  return new ClientRequest(input, options, callback);
};

export const get = (input, options, callback) => {
  if (typeof options === 'function') { callback = options; options = {}; }
  const req = request(input, { ...(options ?? {}), method: 'GET' }, callback);
  req.end();
  return req;
};

class ServerRequest extends Emitter {
  constructor(event) {
    super();
    this.method = event.method;
    this.url = event.url;
    this.headers = event.headers ?? {};
    this.complete = false;
    this.readable = true;
    this._body = event.body ?? '';
  }
  setEncoding() { return this; }
  resume() { return this; }
  _drain() {
    if (this._body.length > 0) this.emit('data', Buffer.from(this._body));
    this.complete = true;
    this.readable = false;
    this.emit('end');
  }
}

class ServerResponse extends Emitter {
  constructor(server, requestId) {
    super();
    this.statusCode = 200;
    this.statusMessage = 'OK';
    this.headersSent = false;
    this.writableEnded = false;
    this._server = server;
    this._requestId = requestId;
    this._headers = {};
    this._chunks = [];
  }
  setHeader(name, value) { this._headers[String(name).toLowerCase()] = String(value); return this; }
  getHeader(name) { return this._headers[String(name).toLowerCase()]; }
  removeHeader(name) { delete this._headers[String(name).toLowerCase()]; return this; }
  writeHead(statusCode, statusMessageOrHeaders, headers) {
    this.statusCode = Number(statusCode);
    if (typeof statusMessageOrHeaders === 'string') this.statusMessage = statusMessageOrHeaders;
    else if (statusMessageOrHeaders && typeof statusMessageOrHeaders === 'object') headers = statusMessageOrHeaders;
    if (headers && typeof headers === 'object') {
      for (const [key, value] of Object.entries(headers)) this.setHeader(key, value);
    }
    this.headersSent = true;
    return this;
  }
  write(chunk, encoding, callback) {
    if (this.writableEnded) throw new Error('write after end');
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    this.headersSent = true;
    if (typeof encoding === 'function') defer(encoding);
    else if (typeof callback === 'function') defer(callback);
    return true;
  }
  end(chunk, encoding, callback) {
    if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
    if (this.writableEnded) return this;
    this.writableEnded = true;
    const body = concatBody(this._chunks) ?? '';
    native.respond(this._server._handle, this._requestId, this.statusCode, this._headers, body);
    if (typeof callback === 'function') defer(callback);
    this.emit('finish');
    this.emit('close');
    return this;
  }
}

class Server extends Emitter {
  constructor(options, listener) {
    super();
    if (!native || typeof native.createServer !== 'function') {
      throw new Error('native TLS server implementation unavailable');
    }
    this._handle = native.createServer(options);
    this._listening = false;
    this._polling = false;
    this._address = null;
    if (typeof listener === 'function') this.on('request', listener);
  }
  listen(...args) {
    let port = 0;
    let callback;
    if (typeof args[0] === 'object' && args[0] !== null) {
      port = Number(args[0].port ?? 0);
      if (typeof args[1] === 'function') callback = args[1];
    } else {
      port = Number(args[0] ?? 0);
      if (typeof args[1] === 'function') callback = args[1];
      else if (typeof args[2] === 'function') callback = args[2];
    }
    this._address = native.listen(this._handle, { port });
    this._listening = true;
    this._startPolling();
    if (typeof callback === 'function') defer(callback);
    this.emit('listening');
    return this;
  }
  address() {
    return native.address(this._handle) ?? this._address;
  }
  close(callback) {
    if (typeof callback === 'function') this.once('close', callback);
    if (!this._listening) {
      defer(() => this.emit('close'));
      return this;
    }
    this._listening = false;
    native.close(this._handle);
    defer(() => this.emit('close'));
    return this;
  }
  _startPolling() {
    if (this._polling) return;
    this._polling = true;
    const poll = () => {
      if (!this._listening) { this._polling = false; return; }
      try {
        for (const event of native.drain(this._handle)) this._handleEvent(event);
      } catch (error) {
        this.emit('error', error);
      }
      setTimeout(poll, 1);
    };
    defer(poll);
  }
  _handleEvent(event) {
    if (event.type === 'request') {
      const req = new ServerRequest(event);
      const res = new ServerResponse(this, event.requestId);
      this.emit('request', req, res);
      defer(() => req._drain());
    } else if (event.type === 'error') {
      this.emit('error', new Error(event.message ?? 'native HTTPS server error'));
    }
  }
}

export function createServer(options, listener) {
  if (!options || !options.cert || !options.key) {
    throw new Error('native TLS server implementation requires { cert, key } PEM');
  }
  return new Server(options, listener);
}

export default {
  Agent,
  ClientRequest,
  IncomingMessage,
  Server,
  globalAgent,
  request,
  get,
  createServer,
};
