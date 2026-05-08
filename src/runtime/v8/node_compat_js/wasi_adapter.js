const ESUCCESS = 0;
const EINVAL = 28;
const ENOSYS = 52;
const CLOCK_REALTIME = 0;
const CLOCK_MONOTONIC = 1;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const toStringArray = (value) => Array.isArray(value) ? value.map((entry) => String(entry)) : [];
const toEnvArray = (value) => {
  if (!value || typeof value !== 'object') {
    return [];
  }
  return Object.entries(value).map(([key, entry]) => `${String(key)}=${String(entry)}`);
};

const writeChunks = (target, text) => {
  if (typeof target?.write === 'function') {
    target.write(text);
    return;
  }
  if (typeof console === 'object' && console !== null) {
    const method = target === globalThis.process?.stderr ? 'error' : 'log';
    console[method]?.(text);
  }
};

const monotonicNowNs = () => {
  if (typeof globalThis.performance?.now === 'function') {
    return BigInt(Math.floor(globalThis.performance.now() * 1_000_000));
  }
  return BigInt(Date.now()) * 1_000_000n;
};

const realtimeNowNs = () => {
  if (
    typeof globalThis.performance?.now === 'function'
    && Number.isFinite(globalThis.performance.timeOrigin)
  ) {
    return BigInt(
      Math.floor((globalThis.performance.timeOrigin + globalThis.performance.now()) * 1_000_000),
    );
  }
  return BigInt(Date.now()) * 1_000_000n;
};

const fillRandom = (bytes) => {
  if (typeof globalThis.crypto?.getRandomValues === 'function') {
    for (let offset = 0; offset < bytes.byteLength; offset += 65536) {
      globalThis.crypto.getRandomValues(bytes.subarray(offset, Math.min(offset + 65536, bytes.byteLength)));
    }
    return;
  }
  for (let i = 0; i < bytes.byteLength; ++i) {
    bytes[i] = (Math.random() * 256) & 0xff;
  }
};

class WASI {
  constructor(opts = {}) {
    this.args = toStringArray(opts.args);
    this.env = toEnvArray(opts.env);
    this.exitCode = 0;
    this._memory = null;
  }

  _setMemory(instance) {
    const memory = instance?.exports?.memory;
    if (!(memory instanceof WebAssembly.Memory)) {
      throw new Error('WASI: instance must export WebAssembly.Memory as "memory"');
    }
    this._memory = memory;
  }

  _view() {
    if (!this._memory) {
      throw new Error('WASI: instance not started; call start(instance) or initialize(instance) first');
    }
    return new DataView(this._memory.buffer);
  }

  start(instance) {
    this._setMemory(instance);
    if (typeof instance?.exports?._start !== 'function') {
      throw new Error('WASI: instance has no _start');
    }
    try {
      instance.exports._start();
    } catch (error) {
      if (error && error.__wasi_exit !== undefined) {
        this.exitCode = Number(error.__wasi_exit);
        return;
      }
      throw error;
    }
  }

  initialize(instance) {
    this._setMemory(instance);
    if (typeof instance?.exports?._initialize === 'function') {
      instance.exports._initialize();
    }
  }

  getImportObject() {
    const wasi = this;
    return {
      wasi_snapshot_preview1: {
        proc_exit(code) {
          const error = new Error(`WASI exit ${code}`);
          error.__wasi_exit = Number(code);
          throw error;
        },
        fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
          if (fd !== 1 && fd !== 2) {
            return ENOSYS;
          }
          const view = wasi._view();
          let total = 0;
          let out = '';
          for (let i = 0; i < iovsLen; ++i) {
            const base = view.getUint32(iovsPtr + i * 8, true);
            const len = view.getUint32(iovsPtr + i * 8 + 4, true);
            out += decoder.decode(new Uint8Array(wasi._memory.buffer, base, len), { stream: i + 1 < iovsLen });
            total += len;
          }
          writeChunks(fd === 2 ? globalThis.process?.stderr : globalThis.process?.stdout, out);
          view.setUint32(nwrittenPtr, total, true);
          return ESUCCESS;
        },
        fd_read(fd, _iovsPtr, _iovsLen, nreadPtr) {
          if (fd !== 0) {
            return ENOSYS;
          }
          wasi._view().setUint32(nreadPtr, 0, true);
          return ESUCCESS;
        },
        fd_close(fd) {
          return fd >= 0 && fd <= 2 ? ESUCCESS : ENOSYS;
        },
        fd_seek() {
          return ENOSYS;
        },
        fd_fdstat_get() {
          return ENOSYS;
        },
        fd_fdstat_set_flags() {
          return ENOSYS;
        },
        random_get(buf, len) {
          fillRandom(new Uint8Array(wasi._memory.buffer, buf, len));
          return ESUCCESS;
        },
        clock_time_get(id, _precision, timeOutPtr) {
          if (id !== CLOCK_REALTIME && id !== CLOCK_MONOTONIC) {
            return EINVAL;
          }
          wasi._view().setBigUint64(
            timeOutPtr,
            id === CLOCK_REALTIME ? realtimeNowNs() : monotonicNowNs(),
            true,
          );
          return ESUCCESS;
        },
        clock_res_get(id, resOutPtr) {
          if (id !== CLOCK_REALTIME && id !== CLOCK_MONOTONIC) {
            return EINVAL;
          }
          wasi._view().setBigUint64(resOutPtr, 1n, true);
          return ESUCCESS;
        },
        environ_sizes_get(countPtr, sizePtr) {
          const view = wasi._view();
          const totalBytes = wasi.env.reduce((total, entry) => total + encoder.encode(`${entry}\0`).length, 0);
          view.setUint32(countPtr, wasi.env.length, true);
          view.setUint32(sizePtr, totalBytes, true);
          return ESUCCESS;
        },
        environ_get(envPtr, envBufPtr) {
          const view = wasi._view();
          let offset = envBufPtr;
          for (let i = 0; i < wasi.env.length; ++i) {
            const bytes = encoder.encode(`${wasi.env[i]}\0`);
            new Uint8Array(wasi._memory.buffer, offset, bytes.length).set(bytes);
            view.setUint32(envPtr + i * 4, offset, true);
            offset += bytes.length;
          }
          return ESUCCESS;
        },
        args_sizes_get(countPtr, sizePtr) {
          const view = wasi._view();
          const totalBytes = wasi.args.reduce((total, entry) => total + encoder.encode(`${entry}\0`).length, 0);
          view.setUint32(countPtr, wasi.args.length, true);
          view.setUint32(sizePtr, totalBytes, true);
          return ESUCCESS;
        },
        args_get(argvPtr, argvBufPtr) {
          const view = wasi._view();
          let offset = argvBufPtr;
          for (let i = 0; i < wasi.args.length; ++i) {
            const bytes = encoder.encode(`${wasi.args[i]}\0`);
            new Uint8Array(wasi._memory.buffer, offset, bytes.length).set(bytes);
            view.setUint32(argvPtr + i * 4, offset, true);
            offset += bytes.length;
          }
          return ESUCCESS;
        },
        path_open() {
          return ENOSYS;
        },
      },
    };
  }
}

const api = { WASI };

export { WASI };
export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
