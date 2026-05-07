
const native = globalThis.fs;
const fdNative = globalThis.__fxe_native?.fs_fd;
const textEncoder = new TextEncoder();

const requireNative = (name) => {
  if (!native || typeof native[name] !== 'function') {
    throw new Error(`host-backed node:fs function unavailable: ${name}`);
  }
  return native[name].bind(native);
};

const requireFdNative = (name) => {
  if (!fdNative || typeof fdNative[name] !== 'function') {
    throw new Error(`host-backed node:fs fd function unavailable: ${name}`);
  }
  return fdNative[name].bind(fdNative);
};

const readFileNative = requireNative('readFile');
const writeFileNative = requireNative('writeFile');
const appendFileNative = requireNative('appendFile');
const statNative = requireNative('stat');
const readdirNative = requireNative('readdir');
const mkdirNative = requireNative('mkdir');
const rmNative = requireNative('rm');
const renameNative = requireNative('rename');
const realpathNative = requireNative('realpath');
const existsNative = requireNative('exists');

const splitCallback = (args) => {
  const actual = Array.prototype.slice.call(args);
  const last = actual[actual.length - 1];
  return typeof last === 'function' ? { args: actual.slice(0, -1), callback: last } : { args: actual, callback: undefined };
};

const withNodeCallback = (promise, callback) => {
  if (typeof callback !== 'function') {
    return promise;
  }
  return Promise.resolve(promise).then(
    (value) => {
      callback(null, value);
      return value;
    },
    (error) => {
      callback(error);
      return undefined;
    },
  );
};

const defer = (fn) => (typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0));

const byteLengthOf = (value) => value?.byteLength ?? value?.length ?? 0;

const toUint8Array = (value, encoding = 'utf8') => {
  if (value instanceof Uint8Array) return value;
  if (typeof Buffer === 'function' && Buffer.isBuffer?.(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  if (typeof value === 'string') return textEncoder.encode(value);
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  if (value === undefined || value === null) return new Uint8Array(0);
  return textEncoder.encode(String(value));
};

const toNodeChunk = (value, length = value?.byteLength ?? 0) => {
  const chunk = length === value.byteLength ? value : value.subarray(0, length);
  return typeof Buffer === 'function' ? Buffer.from(chunk) : chunk;
};

export const readFileSync = (...args) => requireNative('readFileSync')(...args);
export const writeFileSync = (...args) => requireNative('writeFileSync')(...args);
export const appendFileSync = (...args) => requireNative('appendFileSync')(...args);
export const existsSync = (...args) => requireNative('existsSync')(...args);
export const statSync = (...args) => requireNative('statSync')(...args);
export const readdirSync = (...args) => requireNative('readdirSync')(...args);
export const mkdirSync = (...args) => requireNative('mkdirSync')(...args);
export const rmSync = (...args) => requireNative('rmSync')(...args);
export const renameSync = (...args) => requireNative('renameSync')(...args);
export const realpathSync = (...args) => requireNative('realpathSync')(...args);

export const openSync = (path, flags = 'r', mode = 0o666) => requireFdNative('openSync')(path, flags, mode).fd;
export const readSync = (fd, buffer, offset = 0, length = byteLengthOf(buffer) - offset, position = null) =>
  requireFdNative('readSync')(fd, buffer, offset, length, position).bytesRead;
export const writeSync = (fd, buffer, offset = 0, length = byteLengthOf(buffer) - offset, position = null) => {
  const bytes = toUint8Array(buffer);
  return requireFdNative('writeSync')(fd, bytes, offset, length, position).bytesWritten;
};
export const closeSync = (fd) => requireFdNative('closeSync')(fd);
export const fstatSync = (fd) => requireFdNative('fstatSync')(fd);
export const ftruncateSync = (fd, len = 0) => requireFdNative('ftruncateSync')(fd, len);
export const fdatasyncSync = (fd) => requireFdNative('fdatasyncSync')(fd);

export const readFile = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(readFileNative(...args), callback);
};
export const writeFile = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(writeFileNative(...args), callback);
};
export const appendFile = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(appendFileNative(...args), callback);
};
export const stat = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(statNative(...args), callback);
};
export const readdir = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(readdirNative(...args), callback);
};
export const mkdir = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(mkdirNative(...args), callback);
};
export const rm = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(rmNative(...args), callback);
};
export const rename = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(renameNative(...args), callback);
};
export const realpath = (...callArgs) => {
  const { args, callback } = splitCallback(callArgs);
  return withNodeCallback(realpathNative(...args), callback);
};
export const exists = (path, callback) => {
  const promise = existsNative(path);
  if (typeof callback !== 'function') {
    return promise;
  }
  return Promise.resolve(promise).then((ok) => {
    callback(Boolean(ok));
    return Boolean(ok);
  });
};

export const open = (path, flags = 'r', mode = 0o666, callback = undefined) => {
  if (typeof mode === 'function') {
    callback = mode;
    mode = 0o666;
  }
  const promise = requireFdNative('open')(path, flags, mode).then((result) => result.fd);
  return withNodeCallback(promise, callback);
};
export const read = (fd, buffer, offset = 0, length = byteLengthOf(buffer) - offset, position = null, callback = undefined) => {
  const promise = requireFdNative('read')(fd, buffer, offset, length, position).then((result) => ({ bytesRead: result.bytesRead, buffer }));
  if (typeof callback !== 'function') return promise;
  return promise.then(
    (result) => {
      callback(null, result.bytesRead, result.buffer);
      return result;
    },
    (error) => {
      callback(error);
      return undefined;
    },
  );
};
export const write = (fd, buffer, offset = 0, length = byteLengthOf(buffer) - offset, position = null, callback = undefined) => {
  const bytes = toUint8Array(buffer);
  const promise = requireFdNative('write')(fd, bytes, offset, length, position).then((result) => ({ bytesWritten: result.bytesWritten, buffer }));
  if (typeof callback !== 'function') return promise;
  return promise.then(
    (result) => {
      callback(null, result.bytesWritten, result.buffer);
      return result;
    },
    (error) => {
      callback(error);
      return undefined;
    },
  );
};
export const close = (fd, callback) => withNodeCallback(requireFdNative('close')(fd), callback);
export const fstat = (fd, callback) => withNodeCallback(requireFdNative('fstat')(fd), callback);
export const ftruncate = (fd, len = 0, callback = undefined) => {
  if (typeof len === 'function') {
    callback = len;
    len = 0;
  }
  return withNodeCallback(requireFdNative('ftruncate')(fd, len), callback);
};
export const fdatasync = (fd, callback) => withNodeCallback(requireFdNative('fdatasync')(fd), callback);

export const promises = {
  readFile: (...args) => readFileNative(...args),
  writeFile: (...args) => writeFileNative(...args),
  appendFile: (...args) => appendFileNative(...args),
  stat: (...args) => statNative(...args),
  readdir: (...args) => readdirNative(...args),
  mkdir: (...args) => mkdirNative(...args),
  rm: (...args) => rmNative(...args),
  rename: (...args) => renameNative(...args),
  realpath: (...args) => realpathNative(...args),
  exists: (...args) => existsNative(...args),
  open: (...args) => open(...args),
  read: (...args) => read(...args),
  write: (...args) => write(...args),
  close: (...args) => requireFdNative('close')(...args),
  fstat: (...args) => requireFdNative('fstat')(...args),
  ftruncate: (...args) => requireFdNative('ftruncate')(...args),
  fdatasync: (...args) => requireFdNative('fdatasync')(...args),
};

class EventEmitterLike {
  constructor() {
    this._events = Object.create(null);
  }
  on(event, listener) {
    if (typeof listener === 'function') (this._events[event] ??= []).push(listener);
    return this;
  }
  once(event, listener) {
    if (typeof listener !== 'function') return this;
    const wrapped = (...args) => {
      this.off(event, wrapped);
      listener(...args);
    };
    return this.on(event, wrapped);
  }
  off(event, listener) {
    const list = this._events[event];
    if (list) {
      const idx = list.indexOf(listener);
      if (idx >= 0) list.splice(idx, 1);
    }
    return this;
  }
  removeListener(event, listener) {
    return this.off(event, listener);
  }
  emit(event, ...args) {
    const list = this._events[event];
    if (!list) return false;
    for (const listener of [...list]) listener(...args);
    return true;
  }
}

class ReadStreamLike extends EventEmitterLike {
  constructor(path, options = {}) {
    super();
    this.path = path;
    this.bytesRead = 0;
    this.readableEnded = false;
    this.destroyed = false;
    this._fd = typeof options.fd === 'number' ? options.fd : undefined;
    this._autoClose = options.autoClose !== false;
    this._highWaterMark = Math.max(1, Number(options.highWaterMark ?? 64 * 1024) | 0);
    this._start = options.start === undefined ? 0 : Number(options.start);
    this._end = options.end === undefined ? undefined : Number(options.end);
    this._encoding = options.encoding ? String(options.encoding).toLowerCase() : null;
    this._flags = options.flags ?? 'r';
    this._mode = options.mode ?? 0o666;
    this._paused = false;
    defer(() => this._pump());
  }
  setEncoding(encoding) {
    this._encoding = String(encoding ?? 'utf8').toLowerCase();
    return this;
  }
  pause() {
    this._paused = true;
    return this;
  }
  resume() {
    this._paused = false;
    this.emit('_resume');
    return this;
  }
  pipe(dest) {
    this.on('data', (chunk) => dest.write(chunk));
    this.on('end', () => dest.end?.());
    this.on('error', (error) => dest.emit?.('error', error));
    return dest;
  }
  close(callback) {
    if (callback) this.once('close', callback);
    this.destroy();
  }
  destroy(error = undefined) {
    this.destroyed = true;
    if (error) this.emit('error', error);
    return this;
  }
  async _waitIfPaused() {
    while (this._paused && !this.destroyed) {
      await new Promise((resolve) => this.once('_resume', resolve));
    }
  }
  async _pump() {
    let fd = this._fd;
    let closeFd = false;
    try {
      if (fd === undefined) {
        fd = await open(this.path, this._flags, this._mode);
        closeFd = this._autoClose;
      }
      this.fd = fd;
      this.emit('open', fd);
      let position = this._start;
      while (!this.destroyed) {
        await this._waitIfPaused();
        const remaining = this._end === undefined ? this._highWaterMark : this._end - position + 1;
        if (remaining <= 0) break;
        const size = Math.min(this._highWaterMark, remaining);
        const buffer = typeof Buffer === 'function' ? Buffer.alloc(size) : new Uint8Array(size);
        const { bytesRead } = await read(fd, buffer, 0, size, position);
        if (bytesRead <= 0) break;
        this.bytesRead += bytesRead;
        position += bytesRead;
        let chunk = toNodeChunk(buffer, bytesRead);
        if (this._encoding) chunk = typeof Buffer === 'function' ? Buffer.from(chunk).toString(this._encoding) : new TextDecoder(this._encoding).decode(chunk);
        this.emit('data', chunk);
      }
      this.readableEnded = true;
      if (!this.destroyed) this.emit('end');
    } catch (error) {
      this.destroyed = true;
      this.emit('error', error);
    } finally {
      if (closeFd && fd !== undefined) {
        try {
          await close(fd);
        } catch (error) {
          this.emit('error', error);
        }
      }
      this.emit('close');
    }
  }
}

class WriteStreamLike extends EventEmitterLike {
  constructor(path, options = {}) {
    super();
    this.path = path;
    this.bytesWritten = 0;
    this.writableEnded = false;
    this.destroyed = false;
    this._fd = typeof options.fd === 'number' ? options.fd : undefined;
    this._autoClose = options.autoClose !== false;
    this._flags = options.flags ?? 'w';
    this._mode = options.mode ?? 0o666;
    this._position = options.start === undefined ? null : Number(options.start);
    this._ready = this._fd === undefined ? open(path, this._flags, this._mode) : Promise.resolve(this._fd);
    this._ready.then((fd) => {
      this.fd = fd;
      this.emit('open', fd);
    }, (error) => this.destroy(error));
    this._chain = this._ready;
  }
  write(chunk, encoding = undefined, callback = undefined) {
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }
    if (this.writableEnded) throw new Error('write after end');
    const data = toUint8Array(chunk, encoding);
    this._chain = this._chain.then((fd) => write(fd, data, 0, data.byteLength, this._position).then((result) => {
      this.bytesWritten += result.bytesWritten;
      if (this._position !== null) this._position += result.bytesWritten;
      callback?.(null);
      this.emit('drain');
      return fd;
    }), (error) => {
      callback?.(error);
      this.destroy(error);
      throw error;
    });
    return true;
  }
  end(chunk = undefined, encoding = undefined, callback = undefined) {
    if (typeof chunk === 'function') {
      callback = chunk;
      chunk = undefined;
      encoding = undefined;
    }
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }
    if (chunk !== undefined) this.write(chunk, encoding);
    this.writableEnded = true;
    this._chain = this._chain.then(async (fd) => {
      if (this._autoClose) await close(fd);
      this.emit('finish');
      this.emit('close');
      callback?.();
      return fd;
    }, (error) => {
      callback?.(error);
      this.destroy(error);
      throw error;
    });
    return this;
  }
  close(callback) {
    if (callback) this.once('close', callback);
    return this.end();
  }
  destroy(error = undefined) {
    this.destroyed = true;
    if (error) this.emit('error', error);
    this.emit('close');
    return this;
  }
}

export const createReadStream = (path, options = {}) => new ReadStreamLike(path, options ?? {});
export const createWriteStream = (path, options = {}) => new WriteStreamLike(path, options ?? {});

class FSWatcherLike extends EventEmitterLike {
  constructor(nativeWatcher) {
    super();
    this._nativeWatcher = nativeWatcher;
    this._closed = false;
  }
  close() {
    if (this._closed) {
      return;
    }
    this._closed = true;
    this._nativeWatcher?.close?.();
    this.emit('close');
  }
}

export const watch = (path, optsOrListener, listener) => {
  const callback = typeof optsOrListener === 'function' ? optsOrListener : listener;
  const options = optsOrListener && typeof optsOrListener === 'object' ? optsOrListener : {};
  let watcher;
  const nativeWatcher = requireNative('watch')(path, options, (eventType, filename) => {
    if (watcher?._closed) {
      return;
    }
    if (typeof callback === 'function') {
      callback(eventType, filename);
    }
    watcher?.emit('change', eventType, filename);
  });
  watcher = new FSWatcherLike(nativeWatcher);
  return watcher;
};

const fsDefault = {
  readFileSync,
  writeFileSync,
  appendFileSync,
  existsSync,
  statSync,
  readdirSync,
  mkdirSync,
  rmSync,
  renameSync,
  realpathSync,
  openSync,
  readSync,
  writeSync,
  closeSync,
  fstatSync,
  ftruncateSync,
  fdatasyncSync,
  readFile,
  writeFile,
  appendFile,
  stat,
  readdir,
  mkdir,
  rm,
  rename,
  realpath,
  exists,
  open,
  read,
  write,
  close,
  fstat,
  ftruncate,
  fdatasync,
  createReadStream,
  createWriteStream,
  watch,
  promises,
};
export default fsDefault;
