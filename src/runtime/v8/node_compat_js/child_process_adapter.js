
const native = globalThis.__fxe_native?.spawn;

const requireSpawn = () => {
  if (!native || typeof native.spawn !== 'function') {
    throw new Error('host-backed node:child_process function unavailable: spawn');
  }
  return native.spawn.bind(native);
};

class EventEmitterLike {
  constructor() {
    this._events = Object.create(null);
  }
  on(event, listener) {
    if (typeof listener === 'function') {
      (this._events[event] ??= []).push(listener);
    }
    return this;
  }
  once(event, listener) {
    if (typeof listener !== 'function') {
      return this;
    }
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
    if (!list) {
      return false;
    }
    for (const listener of [...list]) {
      listener(...args);
    }
    return true;
  }
}

class ReadableLike extends EventEmitterLike {
  constructor() {
    super();
    this._encoding = null;
  }
  setEncoding(encoding) {
    this._encoding = String(encoding ?? 'utf8').toLowerCase();
    return this;
  }
  _emitData(chunk) {
    if (chunk.length === 0) {
      return;
    }
    this.emit('data', this._encoding === null && typeof Buffer === 'function' ? Buffer.from(chunk) : chunk);
  }
}

class WritableLike {
  constructor(handle) {
    this._handle = handle;
  }
  write(chunk) {
    return this._handle.writeStdin(String(chunk ?? ''));
  }
  end(chunk) {
    if (chunk !== undefined) {
      this.write(chunk);
    }
    return this._handle.endStdin();
  }
}

const normalizeArgs = (args) => {
  if (args === undefined || args === null) {
    return [];
  }
  if (!Array.isArray(args)) {
    throw new TypeError('child_process arguments must be an array');
  }
  return args.map((arg) => String(arg));
};

export const spawn = (file, args = [], options = {}) => {
  const spawnNative = requireSpawn();
  const argv = normalizeArgs(args);
  const handle = spawnNative(String(file), argv, options ?? {});
  const child = new EventEmitterLike();
  child.pid = handle.pid;
  child.stdin = new WritableLike(handle);
  child.stdout = new ReadableLike();
  child.stderr = new ReadableLike();
  child.killed = false;
  child.exitCode = null;
  child.signalCode = null;
  child.kill = (signal = 'SIGTERM') => {
    const ok = handle.kill(signal);
    if (ok) {
      child.killed = true;
    }
    return ok;
  };

  let closed = false;
  const finish = (status) => {
    if (closed) {
      return;
    }
    closed = true;
    clearInterval(timer);
    const code = status && status.exitCode !== undefined ? status.exitCode : null;
    const signal = status && status.signal !== undefined ? status.signal : null;
    child.exitCode = code;
    child.signalCode = signal;
    child.stdout.emit('end');
    child.stderr.emit('end');
    child.emit('exit', code, signal);
    child.emit('close', code, signal);
  };
  const drain = () => {
    try {
      child.stdout._emitData(handle.readStdout());
      child.stderr._emitData(handle.readStderr());
      const status = handle.wait();
      if (status !== null && status !== undefined) {
        child.stdout._emitData(handle.readStdout());
        child.stderr._emitData(handle.readStderr());
        finish(status);
      }
    } catch (error) {
      if (!closed) {
        closed = true;
        clearInterval(timer);
        child.emit('error', error);
        child.emit('close', child.exitCode, child.signalCode);
      }
    }
  };
  const timer = setInterval(drain, Math.max(1, Number(options?.pollInterval ?? 5) || 5));
  Promise.resolve().then(drain);
  return child;
};

export const execFile = (file, args = [], options, callback) => {
  if (typeof args === 'function') {
    callback = args;
    args = [];
    options = {};
  } else if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  const child = spawn(file, args, options ?? {});
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8').on('data', (chunk) => { stdout += String(chunk); });
  child.stderr.setEncoding('utf8').on('data', (chunk) => { stderr += String(chunk); });
  if (typeof callback === 'function') {
    child.on('close', (code, signal) => {
      if (code === 0) {
        callback(null, stdout, stderr);
        return;
      }
      const error = new Error(`Command failed: ${file}`);
      error.code = code;
      error.signal = signal;
      error.stdout = stdout;
      error.stderr = stderr;
      callback(error, stdout, stderr);
    });
  }
  return child;
};

export const spawnSync = (file, args = [], options = {}) => {
  const spawnNative = requireSpawn();
  const argv = normalizeArgs(args);
  const opts = options ?? {};
  const timeout = opts.timeout === undefined ? 0 : Number(opts.timeout);
  if (!Number.isFinite(timeout) || timeout < 0) {
    throw new RangeError('spawnSync timeout must be a non-negative number');
  }
  const handle = spawnNative(String(file), argv, opts);
  const input = opts.input;
  if (input !== undefined) {
    handle.writeStdin(String(input));
  }
  handle.endStdin();

  let stdout = '';
  let stderr = '';
  let status = null;
  let timedOut = false;
  const started = Date.now();
  for (;;) {
    stdout += handle.readStdout();
    stderr += handle.readStderr();
    status = handle.wait();
    if (status !== null && status !== undefined) {
      stdout += handle.readStdout();
      stderr += handle.readStderr();
      break;
    }
    if (timeout > 0 && Date.now() - started >= timeout) {
      handle.kill(opts.killSignal ?? 'SIGTERM');
      timedOut = true;
      continue;
    }
    if (typeof handle.sleep === 'function') {
      handle.sleep(1);
    }
  }

  const signal = status && status.signal !== undefined ? status.signal : null;
  const exitCode = status && status.exitCode !== undefined ? status.exitCode : null;
  const result = {
    pid: handle.pid,
    output: [null, stdout, stderr],
    stdout,
    stderr,
    status: exitCode,
    signal,
    error: undefined,
  };
  if (timedOut) {
    const error = new Error(`spawnSync ${file} timed out`);
    error.code = 'ETIMEDOUT';
    result.error = error;
  }
  if (opts.encoding === 'buffer' && typeof Buffer === 'function') {
    result.stdout = Buffer.from(stdout);
    result.stderr = Buffer.from(stderr);
    result.output = [null, result.stdout, result.stderr];
  }
  return result;
};

export const execFileSync = (file, args = [], options = {}) => {
  const result = spawnSync(file, args, options);
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    const error = new Error(`Command failed: ${file}`);
    error.status = result.status;
    error.signal = result.signal;
    error.stdout = result.stdout;
    error.stderr = result.stderr;
    throw error;
  }
  return result.stdout;
};

export default {
  spawn,
  execFile,
  spawnSync,
  execFileSync,
};
