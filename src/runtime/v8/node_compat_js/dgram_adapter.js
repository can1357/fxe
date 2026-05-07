
const native = globalThis.__fxe_native?.dgram;
const defer = (fn) => typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0);

const requireNative = () => {
  if (!native || typeof native.bind !== 'function') {
    throw new Error('node:dgram UDP requires FXE native socket backing');
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
    if (!list) return false;
    for (const fn of [...list]) fn(...args);
    return true;
  }
}

const toChunk = (data) => typeof Buffer === 'function' ? Buffer.from(data) : data;
const errorFrom = (result, prefix) => {
  const err = new Error(`${prefix}: ${result?.error ?? 'socket error'}`);
  if (result?.errno !== undefined) err.errno = result.errno;
  return err;
};

const emitOrThrow = (socket, err) => {
  if (!socket.emit('error', err)) throw err;
};

class Socket extends Emitter {
  constructor(type = 'udp4') {
    super();
    if (typeof type === 'object' && type !== null) type = type.type ?? 'udp4';
    if (type !== 'udp4' && type !== 'udp6') throw new Error('node:dgram supports udp4 and udp6 sockets in FXE');
    this.type = type;
    this._bindHost = type === 'udp6' ? '::' : '0.0.0.0';
    this._sendHost = type === 'udp6' ? '::1' : '127.0.0.1';
    this._fd = undefined;
    this._address = null;
    this._recvTimer = undefined;
    this._closed = false;
  }
  bind(port = 0, host = this._bindHost, callback = undefined) {
    if (typeof port === 'object' && port !== null) {
      const options = port;
      callback = typeof host === 'function' ? host : callback;
      host = options.address ?? options.host ?? this._bindHost;
      port = options.port ?? 0;
    }
    if (typeof host === 'function') {
      callback = host;
      host = this._bindHost;
    }
    let result;
    try {
      result = requireNative().bind(String(host), Number(port ?? 0), this.type);
    } catch (caught) {
      const err = caught instanceof Error ? caught : new Error(String(caught));
      defer(() => emitOrThrow(this, err));
      return this;
    }
    this._fd = result.fd;
    this._address = { address: result.address, family: result.family, port: result.port };
    this._recvTimer = setInterval(() => {
      if (this._closed || this._fd === undefined) return;
      const packet = requireNative().recv(this._fd);
      if (packet === null || packet === undefined) return;
      if (packet.error) {
        this.emit('error', errorFrom(packet, 'recv'));
        return;
      }
      this.emit('message', toChunk(packet.data), {
        address: packet.address,
        family: packet.family,
        port: packet.port,
        size: packet.data?.byteLength ?? packet.data?.length ?? 0,
      });
    }, 1);
    if (typeof callback === 'function') defer(callback);
    defer(() => this.emit('listening'));
    return this;
  }
  address() {
    return this._address;
  }
  send(buffer, port, host = this._sendHost, callback = undefined) {
    if (typeof host === 'function') {
      callback = host;
      host = this._sendHost;
    }
    if (this._fd === undefined || this._closed) {
      const err = new Error('node:dgram.send on closed socket');
      if (typeof callback === 'function') {
        defer(() => callback(err));
        return undefined;
      }
      throw err;
    }
    try {
      requireNative().send(this._fd, buffer, String(host), Number(port), this.type);
    } catch (caught) {
      const err = caught instanceof Error ? caught : new Error(String(caught));
      if (typeof callback === 'function') defer(() => callback(err));
      else defer(() => emitOrThrow(this, err));
      return undefined;
    }
    if (typeof callback === 'function') defer(() => callback(null));
    return undefined;
  }
  close(callback) {
    if (this._closed) {
      if (typeof callback === 'function') defer(callback);
      return this;
    }
    this._closed = true;
    if (this._recvTimer !== undefined) clearInterval(this._recvTimer);
    if (this._fd !== undefined) {
      requireNative().close(this._fd);
      this._fd = undefined;
    }
    if (typeof callback === 'function') this.once('close', callback);
    defer(() => this.emit('close'));
    return this;
  }
}

export const createSocket = (type) => new Socket(type);

export default {
  createSocket,
};
