const customPromisify = Symbol.for('nodejs.util.promisify.custom');

const seenStringify = (value, seen) => {
  if (value === null) return 'null';
  if (typeof value === 'string') return `'${value}'`;
  if (typeof value === 'number' || typeof value === 'boolean' || typeof value === 'bigint' || typeof value === 'undefined') return String(value);
  if (typeof value === 'function') return `[Function${value.name ? `: ${value.name}` : ''}]`;
  if (typeof value !== 'object') return String(value);
  if (seen.has(value)) return '[Circular]';
  seen.add(value);
  if (Array.isArray(value)) return `[ ${value.map((item) => seenStringify(item, seen)).join(', ')} ]`;
  if (value instanceof Error) return value.stack || `${value.name}: ${value.message}`;
  if (value instanceof Date) return value.toISOString();
  const tag = value.constructor && value.constructor !== Object ? `${value.constructor.name} ` : '';
  const body = Object.keys(value).map((key) => `${key}: ${seenStringify(value[key], seen)}`).join(', ');
  return `${tag}{ ${body} }`;
};

export const inspect = (value, options = undefined) => {
  if (options && options.colors) return seenStringify(value, new Set());
  return seenStringify(value, new Set());
};
inspect.custom = Symbol.for('nodejs.util.inspect.custom');

export const format = (fmt, ...args) => {
  if (typeof fmt !== 'string') return [fmt, ...args].map((arg) => inspect(arg)).join(' ');
  let index = 0;
  const out = fmt.replace(/%[sdifjoO%]/g, (token) => {
    if (token === '%%') return '%';
    if (index >= args.length) return token;
    const value = args[index++];
    switch (token) {
      case '%s': return String(value);
      case '%d': return String(Number(value));
      case '%i': return String(parseInt(value, 10));
      case '%f': return String(parseFloat(value));
      case '%j': try { return JSON.stringify(value); } catch { return '[Circular]'; }
      case '%o':
      case '%O': return inspect(value);
      default: return token;
    }
  });
  return index < args.length ? `${out} ${args.slice(index).map((arg) => inspect(arg)).join(' ')}` : out;
};
export const formatWithOptions = (_options, fmt, ...args) => format(fmt, ...args);

export const inherits = (ctor, superCtor) => {
  if (typeof ctor !== 'function' || typeof superCtor !== 'function') throw new TypeError('ctor and superCtor must be functions');
  Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
  Object.defineProperty(ctor.prototype, 'constructor', { configurable: true, writable: true, value: ctor });
};

export const promisify = (fn) => {
  if (fn && fn[customPromisify]) return fn[customPromisify];
  if (typeof fn !== 'function') throw new TypeError('fn must be a function');
  const wrapped = function promisified(...args) {
    return new Promise((resolve, reject) => {
      fn.call(this, ...args, (err, value) => err ? reject(err) : resolve(value));
    });
  };
  Object.defineProperty(wrapped, customPromisify, { configurable: true, value: wrapped });
  return wrapped;
};
promisify.custom = customPromisify;

export const callbackify = (fn) => {
  if (typeof fn !== 'function') throw new TypeError('fn must be a function');
  return function callbackified(...args) {
    const cb = args.pop();
    if (typeof cb !== 'function') throw new TypeError('last argument must be a function');
    Promise.resolve(fn.apply(this, args)).then(
      (value) => queueMicrotask(() => cb(null, value)),
      (reason) => queueMicrotask(() => cb(reason)),
    );
  };
};

export const types = {
  isArrayBuffer: (value) => value instanceof ArrayBuffer,
  isAnyArrayBuffer: (value) => value instanceof ArrayBuffer,
  isArrayBufferView: ArrayBuffer.isView,
  isTypedArray: (value) => ArrayBuffer.isView(value) && !(value instanceof DataView),
  isUint8Array: (value) => value instanceof Uint8Array,
  isDate: (value) => value instanceof Date,
  isRegExp: (value) => value instanceof RegExp,
  isMap: (value) => value instanceof Map,
  isSet: (value) => value instanceof Set,
  isPromise: (value) => value instanceof Promise || Boolean(value && typeof value.then === 'function'),
  isNativeError: (value) => value instanceof Error,
};

export const debuglog = () => () => {};
export const deprecate = (fn) => fn;
export const TextEncoder = globalThis.TextEncoder;
export const TextDecoder = globalThis.TextDecoder;
export default { format, formatWithOptions, inspect, inherits, promisify, callbackify, types, debuglog, deprecate, TextEncoder, TextDecoder };
