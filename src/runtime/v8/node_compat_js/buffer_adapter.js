const BufferCtor = globalThis.Buffer;

if (typeof BufferCtor !== 'function') {
  throw new Error('host-backed node:buffer unavailable: Buffer');
}

if (typeof BufferCtor.concat !== 'function') {
  BufferCtor.concat = (list, totalLength = undefined) => {
    if (!Array.isArray(list)) throw new TypeError('list must be an Array of Buffer or Uint8Array instances');
    let length = totalLength === undefined ? list.reduce((n, chunk) => n + (chunk?.length ?? 0), 0) : Number(totalLength);
    if (!Number.isFinite(length) || length < 0) length = 0;
    const out = BufferCtor.alloc(length);
    let offset = 0;
    for (const chunk of list) {
      const src = BufferCtor.isBuffer(chunk) ? chunk : BufferCtor.from(chunk ?? []);
      if (offset >= length) break;
      out.set(src.subarray(0, length - offset), offset);
      offset += src.length;
    }
    return out;
  };
}

if (typeof BufferCtor.byteLength !== 'function') {
  BufferCtor.byteLength = (value, encoding = 'utf8') => BufferCtor.from(value, encoding).length;
}
if (typeof BufferCtor.allocUnsafe !== 'function') BufferCtor.allocUnsafe = (size) => new BufferCtor(Number(size) || 0);
if (typeof BufferCtor.compare !== 'function') {
  BufferCtor.compare = (a, b) => {
    const aa = BufferCtor.from(a);
    const bb = BufferCtor.from(b);
    const n = Math.min(aa.length, bb.length);
    for (let i = 0; i < n; ++i) if (aa[i] !== bb[i]) return aa[i] < bb[i] ? -1 : 1;
    return aa.length === bb.length ? 0 : aa.length < bb.length ? -1 : 1;
  };
}

export const Buffer = BufferCtor;
export const SlowBuffer = (size) => BufferCtor.alloc(Number(size) || 0);
export const INSPECT_MAX_BYTES = 50;
export const kMaxLength = 0x7fffffff;
export const kStringMaxLength = 0x1fffffe8;
export const constants = { MAX_LENGTH: kMaxLength, MAX_STRING_LENGTH: kStringMaxLength };
export const atob = globalThis.atob?.bind(globalThis);
export const btoa = globalThis.btoa?.bind(globalThis);
export const Blob = globalThis.Blob;
export const File = globalThis.File;
export default { Buffer, SlowBuffer, INSPECT_MAX_BYTES, kMaxLength, kStringMaxLength, constants, atob, btoa, Blob, File };
