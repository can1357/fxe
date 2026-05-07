
const tcpNative = globalThis.__fxe_native?.net;
const ipcNative = globalThis.__fxe_native?.ipcsock;
const platform = globalThis.__fxe_native?.os?.platform?.() ?? '';
const windowsPipePrefix = '\\\\.\\pipe\\';
const defer = (fn) => typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0);

const isIpcPath = (value) => typeof value === 'string' &&
  ((platform !== 'win32' && value.startsWith('/')) || value.startsWith(windowsPipePrefix));

const requireNative = (kind = 'tcp') => {
  const n = kind === 'ipc' ? ipcNative : tcpNative;
  if (!n || typeof n.listen !== 'function') {
    throw new Error(`node:net ${kind === 'ipc' ? 'IPC socket' : 'TCP'} requires FXE native socket backing`);
  }
  return n;
};

class Emitter {
  constructor() {
    this._events = new Map();
  }
  on(name, fn) {
    if (typeof fn !== 'function') return this;
    const list = this._events.get(name) ?? [];
    list.push(fn);
    this._events.set(name, list);
    return this;
  }
  once(name, fn) {
    if (typeof fn !== 'function') return this;
    const wrapped = (...args) => {
      this.off(name, wrapped);
      fn(...args);
    };
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

const normalizeListenArgs = (args) => {
  let port = 0;
  let host = '127.0.0.1';
  let path;
  let callback;
  if (typeof args[0] === 'object' && args[0] !== null) {
    path = isIpcPath(args[0].path) ? String(args[0].path) : undefined;
    port = Number(args[0].port ?? 0);
    host = String(args[0].host ?? args[0].hostname ?? host);
    callback = typeof args[1] === 'function' ? args[1] : undefined;
  } else if (isIpcPath(args[0])) {
    path = String(args[0]);
    callback = typeof args[1] === 'function' ? args[1] : undefined;
  } else {
    port = Number(args[0] ?? 0);
    if (typeof args[1] === 'string') host = args[1];
    callback = typeof args[1] === 'function' ? args[1] : (typeof args[2] === 'function' ? args[2] : undefined);
  }
  return { port, host, path, callback };
};

const normalizeConnectArgs = (args) => {
  let port = 0;
  let host = '127.0.0.1';
  let path;
  let callback;
  if (typeof args[0] === 'object' && args[0] !== null) {
    path = isIpcPath(args[0].path) ? String(args[0].path) : undefined;
    port = Number(args[0].port ?? 0);
    host = String(args[0].host ?? args[0].hostname ?? host);
    callback = typeof args[1] === 'function' ? args[1] : undefined;
  } else if (isIpcPath(args[0])) {
    path = String(args[0]);
    callback = typeof args[1] === 'function' ? args[1] : undefined;
  } else {
    port = Number(args[0] ?? 0);
    if (typeof args[1] === 'string') host = args[1];
    callback = typeof args[1] === 'function' ? args[1] : (typeof args[2] === 'function' ? args[2] : undefined);
  }
  return { port, host, path, callback };
};

const toChunk = (data) => typeof Buffer === 'function' ? Buffer.from(data) : data;
const errorFrom = (result, prefix) => {
  const err = new Error(`${prefix}: ${result?.error ?? 'socket error'}`);
  if (result?.errno !== undefined) err.errno = result.errno;
  return err;
};

class Socket extends Emitter {
  constructor(fd = undefined, nativeBacking = undefined) {
    super();
    this._fd = fd;
    this._native = nativeBacking;
    this._closed = false;
    this._readTimer = undefined;
  }
  connect(...args) {
    const { port, host, path, callback } = normalizeConnectArgs(args);
    if (callback) this.once('connect', callback);
    const n = requireNative(path ? 'ipc' : 'tcp');
    const result = path ? n.connect(path) : n.connect(host, port);
    this._native = n;
    this._fd = result.fd;
    const finish = () => {
      if (this._closed) return;
      const state = n.finishConnect(this._fd);
      if (state?.error) {
        this.destroy(errorFrom(state, 'connect'));
        return;
      }
      clearInterval(timer);
      this.emit('connect');
      this._startRead();
    };
    const timer = setInterval(finish, 1);
    if (result.connected) defer(finish);
    return this;
  }
  _startRead() {
    if (this._readTimer !== undefined || this._fd === undefined || this._closed) return;
    const n = this._native ?? requireNative();
    this._readTimer = setInterval(() => {
      if (this._closed) return;
      const result = n.read(this._fd);
      if (result === null || result === undefined) return;
      if (result.error) {
        this.destroy(errorFrom(result, 'read'));
        return;
      }
      if (result.eof) {
        this.emit('end');
        this.destroy();
        return;
      }
      if (result.data !== undefined) this.emit('data', toChunk(result.data));
    }, 1);
  }
  write(data, callback) {
    if (this._closed || this._fd === undefined) throw new Error('node:net.Socket.write on closed socket');
    (this._native ?? requireNative()).write(this._fd, data);
    if (typeof callback === 'function') defer(callback);
    return true;
  }
  end(data, callback) {
    if (data !== undefined && data !== null) this.write(data);
    if (!this._closed && this._fd !== undefined) (this._native ?? requireNative()).shutdown(this._fd);
    if (typeof callback === 'function') defer(callback);
    return this;
  }
  destroy(error) {
    if (this._closed) return this;
    this._closed = true;
    if (this._readTimer !== undefined) clearInterval(this._readTimer);
    if (error) this.emit('error', error);
    if (this._fd !== undefined) {
      (this._native ?? requireNative()).close(this._fd);
      this._fd = undefined;
    }
    this.emit('close', Boolean(error));
    return this;
  }
  address() {
    if (this._fd === undefined) return null;
    return (this._native ?? requireNative()).address(this._fd);
  }
  setTimeout() { return this; }
  setNoDelay() { return this; }
  setKeepAlive() { return this; }
}

class Server extends Emitter {
  constructor(connectionListener) {
    super();
    this._fd = undefined;
    this._address = null;
    this._native = undefined;
    this._acceptTimer = undefined;
    this._connections = new Set();
    if (typeof connectionListener === 'function') this.on('connection', connectionListener);
  }
  listen(...args) {
    const { port, host, path, callback } = normalizeListenArgs(args);
    if (callback) this.once('listening', callback);
    const n = requireNative(path ? 'ipc' : 'tcp');
    const result = path ? n.listen(path) : n.listen(host, port);
    this._native = n;
    this._fd = result.fd;
    this._address = path ? result.path : { address: result.address, family: result.family, port: result.port };
    this._acceptTimer = setInterval(() => {
      if (this._fd === undefined) return;
      const accepted = n.accept(this._fd);
      if (accepted === null || accepted === undefined) return;
      if (accepted.error) {
        this.emit('error', errorFrom(accepted, 'accept'));
        return;
      }
      const socket = new Socket(accepted.fd, n);
      this._connections.add(socket);
      socket.once('close', () => this._connections.delete(socket));
      socket._startRead();
      this.emit('connection', socket);
    }, 1);
    defer(() => this.emit('listening'));
    return this;
  }
  address() {
    return this._address;
  }
  close(callback) {
    if (typeof callback === 'function') this.once('close', callback);
    if (this._acceptTimer !== undefined) {
      clearInterval(this._acceptTimer);
      this._acceptTimer = undefined;
    }
    if (this._fd !== undefined) {
      (this._native ?? requireNative()).close(this._fd);
      this._fd = undefined;
    }
    for (const socket of [...this._connections]) socket.destroy();
    defer(() => this.emit('close'));
    return this;
  }
}
export { Socket, Server };

export const createConnection = (...args) => new Socket().connect(...args);
export const connect = createConnection;
export const createServer = (...args) => new Server(...args);

export const isIPv4 = (input) => {
  const parts = String(input).split('.');
  return parts.length === 4 && parts.every((part) => {
    if (!/^\d+$/.test(part)) return false;
    if (part.length > 1 && part.startsWith('0')) return false;
    const value = Number(part);
    return value >= 0 && value <= 255;
  });
};

export const isIPv6 = (input) => String(input).includes(':') ? 6 : 0;
export const isIP = (input) => isIPv4(input) ? 4 : (isIPv6(input) ? 6 : 0);

export default {
  Socket,
  Server,
  createConnection,
  connect,
  createServer,
  isIP,
  isIPv4,
  isIPv6,
};
