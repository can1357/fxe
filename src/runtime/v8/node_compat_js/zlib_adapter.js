const native = globalThis.__fxe_native?.zlib;
const defer = (fn) => typeof queueMicrotask === 'function' ? queueMicrotask(fn) : setTimeout(fn, 0);

const requireNative = (name) => {
  if (!native || typeof native[name] !== 'function') {
    const err = new Error(`fxe: node:zlib native binding missing for ${name}`);
    err.code = 'ERR_FXE_ZLIB_NATIVE_MISSING';
    throw err;
  }
  return native[name].bind(native);
};

const unsupported = (name, why) => {
  const err = new Error(`fxe: node:zlib.${name} is not implemented: ${why}`);
  err.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  return err;
};

const sync = (name) => (...args) => requireNative(name)(...args);
const wrapAsync = (name, fn) => (input, opts, cb) => {
  const callback = typeof opts === 'function' ? opts : cb;
  const options = typeof opts === 'function' ? undefined : opts;
  if (typeof callback !== 'function') {
    const err = new TypeError(`node:zlib.${name} requires a callback`);
    err.code = 'ERR_INVALID_ARG_TYPE';
    throw err;
  }
  try {
    const result = fn(input, options);
    defer(() => callback(null, result));
  } catch (error) {
    defer(() => callback(error));
  }
};

const streamUnsupported = (name) => () => {
  throw unsupported(name, 'stream wrappers are not wired yet; only sync and callback APIs are available');
};

const classUnsupported = (name) => class {
  constructor() {
    throw unsupported(name, 'stream classes are not wired yet; use sync or callback APIs');
  }
};

export const Z_NO_FLUSH = 0;
export const Z_PARTIAL_FLUSH = 1;
export const Z_SYNC_FLUSH = 2;
export const Z_FULL_FLUSH = 3;
export const Z_FINISH = 4;
export const Z_BLOCK = 5;
export const Z_OK = 0;
export const Z_STREAM_END = 1;
export const Z_NEED_DICT = 2;
export const Z_ERRNO = -1;
export const Z_STREAM_ERROR = -2;
export const Z_DATA_ERROR = -3;
export const Z_MEM_ERROR = -4;
export const Z_BUF_ERROR = -5;
export const Z_VERSION_ERROR = -6;
export const Z_DEFAULT_COMPRESSION = -1;
export const Z_BEST_SPEED = 1;
export const Z_BEST_COMPRESSION = 9;
export const Z_DEFAULT_STRATEGY = 0;
export const Z_FILTERED = 1;
export const Z_HUFFMAN_ONLY = 2;
export const Z_RLE = 3;
export const Z_FIXED = 4;
export const Z_DEFAULT_WINDOWBITS = 15;
export const Z_DEFAULT_MEMLEVEL = 8;
export const Z_DEFAULT_CHUNK = 16384;

export const deflateSync = sync('deflateSync');
export const deflateRawSync = sync('deflateRawSync');
export const gzipSync = sync('gzipSync');
export const gunzipSync = sync('gunzipSync');
export const inflateSync = sync('inflateSync');
export const inflateRawSync = sync('inflateRawSync');
export const crc32 = sync('crc32');
export const adler32 = sync('adler32');
export const brotliCompressSync = () => {
  throw unsupported('brotliCompressSync', 'brotli is not exposed natively');
};
export const brotliDecompressSync = () => {
  throw unsupported('brotliDecompressSync', 'brotli is not exposed natively');
};

export const deflate = wrapAsync('deflate', deflateSync);
export const deflateRaw = wrapAsync('deflateRaw', deflateRawSync);
export const gzip = wrapAsync('gzip', gzipSync);
export const gunzip = wrapAsync('gunzip', gunzipSync);
export const inflate = wrapAsync('inflate', inflateSync);
export const inflateRaw = wrapAsync('inflateRaw', inflateRawSync);
export const brotliCompress = wrapAsync('brotliCompress', brotliCompressSync);
export const brotliDecompress = wrapAsync('brotliDecompress', brotliDecompressSync);

export const constants = {
  Z_NO_FLUSH,
  Z_PARTIAL_FLUSH,
  Z_SYNC_FLUSH,
  Z_FULL_FLUSH,
  Z_FINISH,
  Z_BLOCK,
  Z_OK,
  Z_STREAM_END,
  Z_NEED_DICT,
  Z_ERRNO,
  Z_STREAM_ERROR,
  Z_DATA_ERROR,
  Z_MEM_ERROR,
  Z_BUF_ERROR,
  Z_VERSION_ERROR,
  Z_DEFAULT_COMPRESSION,
  Z_BEST_SPEED,
  Z_BEST_COMPRESSION,
  Z_DEFAULT_STRATEGY,
  Z_FILTERED,
  Z_HUFFMAN_ONLY,
  Z_RLE,
  Z_FIXED,
  Z_DEFAULT_WINDOWBITS,
  Z_DEFAULT_MEMLEVEL,
  Z_DEFAULT_CHUNK,
};

export class Deflate extends classUnsupported('Deflate') {}
export class DeflateRaw extends classUnsupported('DeflateRaw') {}
export class Gzip extends classUnsupported('Gzip') {}
export class Gunzip extends classUnsupported('Gunzip') {}
export class Inflate extends classUnsupported('Inflate') {}
export class InflateRaw extends classUnsupported('InflateRaw') {}
export class BrotliCompress extends classUnsupported('BrotliCompress') {}
export class BrotliDecompress extends classUnsupported('BrotliDecompress') {}

export const createDeflate = streamUnsupported('createDeflate');
export const createDeflateRaw = streamUnsupported('createDeflateRaw');
export const createGzip = streamUnsupported('createGzip');
export const createGunzip = streamUnsupported('createGunzip');
export const createInflate = streamUnsupported('createInflate');
export const createInflateRaw = streamUnsupported('createInflateRaw');
export const createBrotliCompress = streamUnsupported('createBrotliCompress');
export const createBrotliDecompress = streamUnsupported('createBrotliDecompress');

const api = {
  Z_NO_FLUSH,
  Z_PARTIAL_FLUSH,
  Z_SYNC_FLUSH,
  Z_FULL_FLUSH,
  Z_FINISH,
  Z_BLOCK,
  Z_OK,
  Z_STREAM_END,
  Z_NEED_DICT,
  Z_ERRNO,
  Z_STREAM_ERROR,
  Z_DATA_ERROR,
  Z_MEM_ERROR,
  Z_BUF_ERROR,
  Z_VERSION_ERROR,
  Z_DEFAULT_COMPRESSION,
  Z_BEST_SPEED,
  Z_BEST_COMPRESSION,
  Z_DEFAULT_STRATEGY,
  Z_FILTERED,
  Z_HUFFMAN_ONLY,
  Z_RLE,
  Z_FIXED,
  Z_DEFAULT_WINDOWBITS,
  Z_DEFAULT_MEMLEVEL,
  Z_DEFAULT_CHUNK,
  constants,
  deflateSync,
  deflateRawSync,
  gzipSync,
  gunzipSync,
  inflateSync,
  inflateRawSync,
  crc32,
  adler32,
  brotliCompressSync,
  brotliDecompressSync,
  deflate,
  deflateRaw,
  gzip,
  gunzip,
  inflate,
  inflateRaw,
  brotliCompress,
  brotliDecompress,
  Deflate,
  DeflateRaw,
  Gzip,
  Gunzip,
  Inflate,
  InflateRaw,
  BrotliCompress,
  BrotliDecompress,
  createDeflate,
  createDeflateRaw,
  createGzip,
  createGunzip,
  createInflate,
  createInflateRaw,
  createBrotliCompress,
  createBrotliDecompress,
};

export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
