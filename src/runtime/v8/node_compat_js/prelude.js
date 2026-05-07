
(() => {
  const g = globalThis;
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
  const utf8Encode = (s) => {
    const out = [];
    for (let i = 0; i < s.length; ++i) {
      let c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
        const lo = s.charCodeAt(i + 1);
        if (lo >= 0xdc00 && lo <= 0xdfff) {
          c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
          ++i;
        }
      }
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
      else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
      else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 0x3f), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
    return new Uint8Array(out);
  };
  const utf8Decode = (bytes) => {
    let out = '';
    for (let i = 0; i < bytes.length;) {
      const b0 = bytes[i++];
      let cp = b0;
      if (b0 >= 0xc0 && b0 < 0xe0) cp = ((b0 & 0x1f) << 6) | (bytes[i++] & 0x3f);
      else if (b0 >= 0xe0 && b0 < 0xf0) cp = ((b0 & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
      else if (b0 >= 0xf0) cp = ((b0 & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
      if (cp <= 0xffff) out += String.fromCharCode(cp);
      else { cp -= 0x10000; out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff)); }
    }
    return out;
  };
  const bytesFromBase64 = (s) => {
    const clean = String(s).replace(/[\t\n\f\r ]+/g, '');
    const out = [];
    for (let i = 0; i < clean.length;) {
      const e1 = chars.indexOf(clean.charAt(i++));
      const e2 = chars.indexOf(clean.charAt(i++));
      const e3 = chars.indexOf(clean.charAt(i++));
      const e4 = chars.indexOf(clean.charAt(i++));
      const c1 = (e1 << 2) | (e2 >> 4);
      const c2 = ((e2 & 15) << 4) | (e3 >> 2);
      const c3 = ((e3 & 3) << 6) | e4;
      out.push(c1 & 255);
      if (e3 !== 64 && e3 !== -1) out.push(c2 & 255);
      if (e4 !== 64 && e4 !== -1) out.push(c3 & 255);
    }
    return new Uint8Array(out);
  };
  const base64FromBytes = (bytes) => {
    let out = '';
    for (let i = 0; i < bytes.length;) {
      const c1 = bytes[i++], c2 = i < bytes.length ? bytes[i++] : NaN, c3 = i < bytes.length ? bytes[i++] : NaN;
      out += chars[c1 >> 2];
      out += chars[((c1 & 3) << 4) | (Number.isNaN(c2) ? 0 : (c2 >> 4))];
      out += Number.isNaN(c2) ? '=' : chars[((c2 & 15) << 2) | (Number.isNaN(c3) ? 0 : (c3 >> 6))];
      out += Number.isNaN(c3) ? '=' : chars[c3 & 63];
    }
    return out;
  };
  if (typeof g.atob !== 'function') g.atob = (s) => String.fromCharCode(...bytesFromBase64(s));
  if (typeof g.btoa !== 'function') g.btoa = (s) => base64FromBytes(Uint8Array.from(String(s), (c) => c.charCodeAt(0) & 255));
  if (typeof g.TextEncoder !== 'function') g.TextEncoder = class TextEncoder { encode(s = '') { return utf8Encode(String(s)); } };
  if (typeof g.TextDecoder !== 'function') g.TextDecoder = class TextDecoder { constructor(label = 'utf-8') { this.encoding = String(label).toLowerCase(); } decode(input = new Uint8Array()) { return utf8Decode(input instanceof Uint8Array ? input : new Uint8Array(input)); } };
  if (typeof g.Blob !== 'function') {
    const normalizeBlobType = (type) => {
      const s = String(type ?? '');
      for (let i = 0; i < s.length; ++i) {
        const c = s.charCodeAt(i);
        if (c < 0x20 || c > 0x7e) return '';
      }
      return s.toLowerCase();
    };
    const copyBlobBytes = (part) => {
      if (part instanceof Blob) return part._parts.map((chunk) => new Uint8Array(chunk));
      if (typeof part === 'string') return [utf8Encode(part)];
      if (part instanceof ArrayBuffer) return [new Uint8Array(part.slice(0))];
      if (ArrayBuffer.isView(part)) return [new Uint8Array(new Uint8Array(part.buffer, part.byteOffset, part.byteLength))];
      return [utf8Encode(String(part))];
    };
    class Blob {
      constructor(parts = [], options = {}) {
        const chunks = [];
        let size = 0;
        for (const part of parts ?? []) {
          for (const chunk of copyBlobBytes(part)) {
            chunks.push(chunk);
            size += chunk.byteLength;
          }
        }
        this._parts = chunks;
        this.size = size;
        this.type = normalizeBlobType(options && options.type);
      }
      async arrayBuffer() {
        const out = new Uint8Array(this.size);
        let offset = 0;
        for (const chunk of this._parts) {
          out.set(chunk, offset);
          offset += chunk.byteLength;
        }
        return out.buffer;
      }
      async text() {
        return utf8Decode(new Uint8Array(await this.arrayBuffer()));
      }
      slice(start = 0, end = this.size, type = '') {
        const size = this.size;
        let relativeStart = Number(start);
        if (!Number.isFinite(relativeStart)) relativeStart = 0;
        let relativeEnd = end === undefined ? size : Number(end);
        if (!Number.isFinite(relativeEnd)) relativeEnd = 0;
        const from = relativeStart < 0 ? Math.max(size + relativeStart, 0) : Math.min(relativeStart, size);
        const to = relativeEnd < 0 ? Math.max(size + relativeEnd, 0) : Math.min(relativeEnd, size);
        const span = Math.max(to - from, 0);
        const out = new Uint8Array(span);
        let readOffset = 0;
        let writeOffset = 0;
        for (const chunk of this._parts) {
          const chunkEnd = readOffset + chunk.byteLength;
          if (chunkEnd > from && readOffset < to) {
            const sliceStart = Math.max(from - readOffset, 0);
            const sliceEnd = Math.min(to - readOffset, chunk.byteLength);
            out.set(chunk.subarray(sliceStart, sliceEnd), writeOffset);
            writeOffset += sliceEnd - sliceStart;
          }
          readOffset = chunkEnd;
          if (readOffset >= to) break;
        }
        return new Blob([out], { type });
      }
      stream() {
        throw new Error('Blob.stream() is not supported by this runtime');
      }
      get [Symbol.toStringTag]() { return 'Blob'; }
    }
    g.Blob = Blob;
  }
  if (typeof g.structuredClone !== 'function') g.structuredClone = (value) => value == null || typeof value !== 'object' ? value : JSON.parse(JSON.stringify(value));
  const fxeCloneMessage = (value) => {
    if (value === null || typeof value !== 'object') return value;
    return g.structuredClone(value);
  };
  const fxeScheduleMessage = (fn) => {
    Promise.resolve().then(fn);
  };
  const fxeCreateMessageEvent = (data, target) => ({ type: 'message', data, target, currentTarget: target });
  const fxeAddMessageListener = (target, type, listener) => {
    if (type !== 'message' || typeof listener !== 'function') return;
    target._listeners.add(listener);
  };
  const fxeRemoveMessageListener = (target, type, listener) => {
    if (type !== 'message' || typeof listener !== 'function') return;
    target._listeners.delete(listener);
  };
  const fxeDispatchMessage = (target, data) => {
    if (target._closed) return;
    const event = fxeCreateMessageEvent(data, target);
    if (typeof target.onmessage === 'function') {
      target.onmessage.call(target, event);
    }
    for (const listener of Array.from(target._listeners)) {
      if (!target._closed && typeof listener === 'function') {
        listener.call(target, event);
      }
    }
  };
  if (typeof g.BroadcastChannel !== 'function') {
    const channels = new Map();
    g.BroadcastChannel = class BroadcastChannel {
      constructor(name) {
        this.name = String(name);
        this.onmessage = null;
        this._closed = false;
        this._listeners = new Set();
        let peers = channels.get(this.name);
        if (peers === undefined) {
          peers = new Set();
          channels.set(this.name, peers);
        }
        peers.add(this);
      }
      postMessage(value) {
        if (this._closed) return;
        const peers = channels.get(this.name);
        if (peers === undefined) return;
        const deliveries = [];
        for (const peer of peers) {
          if (peer !== this && !peer._closed) {
            deliveries.push([peer, fxeCloneMessage(value)]);
          }
        }
        fxeScheduleMessage(() => {
          for (const [peer, data] of deliveries) {
            fxeDispatchMessage(peer, data);
          }
        });
      }
      addEventListener(type, listener) { fxeAddMessageListener(this, type, listener); }
      removeEventListener(type, listener) { fxeRemoveMessageListener(this, type, listener); }
      close() {
        if (this._closed) return;
        this._closed = true;
        this._listeners.clear();
        const peers = channels.get(this.name);
        if (peers !== undefined) {
          peers.delete(this);
          if (peers.size === 0) channels.delete(this.name);
        }
      }
      get [Symbol.toStringTag]() { return 'BroadcastChannel'; }
    };
  }
  if (typeof g.MessageChannel !== 'function' || typeof g.MessagePort !== 'function') {
    class MessagePort {
      constructor() {
        this.onmessage = null;
        this._closed = false;
        this._listeners = new Set();
        this._peer = null;
      }
      postMessage(value) {
        if (this._closed || this._peer === null || this._peer._closed) return;
        const peer = this._peer;
        const data = fxeCloneMessage(value);
        fxeScheduleMessage(() => {
          if (!this._closed && !peer._closed) {
            fxeDispatchMessage(peer, data);
          }
        });
      }
      start() {}
      close() {
        this._closed = true;
        this._listeners.clear();
      }
      addEventListener(type, listener) { fxeAddMessageListener(this, type, listener); }
      removeEventListener(type, listener) { fxeRemoveMessageListener(this, type, listener); }
      get [Symbol.toStringTag]() { return 'MessagePort'; }
    }
    class MessageChannel {
      constructor() {
        this.port1 = new MessagePort();
        this.port2 = new MessagePort();
        this.port1._peer = this.port2;
        this.port2._peer = this.port1;
      }
      get [Symbol.toStringTag]() { return 'MessageChannel'; }
    }
    if (typeof g.MessagePort !== 'function') g.MessagePort = MessagePort;
    if (typeof g.MessageChannel !== 'function') g.MessageChannel = MessageChannel;
  }
  if (typeof g.Buffer !== 'function') {
    class Buffer extends Uint8Array {
      static from(value, encoding = 'utf8') {
        if (typeof value === 'string') {
          const enc = String(encoding).toLowerCase();
          if (enc === 'hex') {
            const out = new Buffer(Math.floor(value.length / 2));
            for (let i = 0; i < out.length; ++i) out[i] = parseInt(value.substr(i * 2, 2), 16) || 0;
            return out;
          }
          if (enc === 'base64') return new Buffer(bytesFromBase64(value));
          return new Buffer(utf8Encode(value));
        }
        if (ArrayBuffer.isView(value)) return new Buffer(value.buffer, value.byteOffset, value.byteLength);
        if (value instanceof ArrayBuffer) return new Buffer(value);
        return new Buffer(value ?? 0);
      }
      static alloc(size, fill = 0) {
        const b = new Buffer(size);
        if (typeof fill === 'string' && fill.length > 0) {
          const bytes = utf8Encode(fill);
          if (bytes.length === 0) return b;
          for (let i = 0; i < b.length; ++i) b[i] = bytes[i % bytes.length];
        } else if (Buffer.isBuffer(fill) || ArrayBuffer.isView(fill)) {
          const bytes = Buffer.isBuffer(fill) ? fill : new Uint8Array(fill.buffer, fill.byteOffset, fill.byteLength);
          if (bytes.length === 0) return b;
          for (let i = 0; i < b.length; ++i) b[i] = bytes[i % bytes.length];
        } else {
          b.fill(Number(fill) || 0);
        }
        return b;
      }
      static allocUnsafe(size) { return new Buffer(Number(size) || 0); }
      static isBuffer(value) { return value instanceof Buffer; }
      static concat(list, totalLength = undefined) {
        if (!Array.isArray(list)) throw new TypeError('Buffer.concat list must be an array');
        let length = totalLength === undefined ? list.reduce((n, chunk) => n + (chunk ? chunk.length : 0), 0) : Number(totalLength);
        if (!Number.isFinite(length) || length < 0) length = 0;
        const out = Buffer.alloc(length);
        let offset = 0;
        for (const chunk of list) {
          const src = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk ?? []);
          if (offset >= length) break;
          out.set(src.subarray(0, length - offset), offset);
          offset += src.length;
        }
        return out;
      }
      static byteLength(value, encoding = 'utf8') { return Buffer.from(value, encoding).length; }
      toString(encoding = 'utf8') {
        const enc = String(encoding).toLowerCase();
        if (enc === 'hex') return Array.prototype.map.call(this, (b) => b.toString(16).padStart(2, '0')).join('');
        if (enc === 'base64') return base64FromBytes(this);
        return utf8Decode(this);
      }
    }
    g.Buffer = Buffer;
  } else {
    if (typeof g.Buffer.alloc !== 'function') g.Buffer.alloc = (size, fill = 0) => { const b = new g.Buffer(Number(size) || 0); b.fill(fill); return b; };
    if (typeof g.Buffer.allocUnsafe !== 'function') g.Buffer.allocUnsafe = (size) => new g.Buffer(Number(size) || 0);
    if (typeof g.Buffer.isBuffer !== 'function') g.Buffer.isBuffer = (value) => value instanceof g.Buffer;
    if (typeof g.Buffer.concat !== 'function') g.Buffer.concat = (list, totalLength = undefined) => {
      if (!Array.isArray(list)) throw new TypeError('Buffer.concat list must be an array');
      let length = totalLength === undefined ? list.reduce((n, chunk) => n + (chunk ? chunk.length : 0), 0) : Number(totalLength);
      if (!Number.isFinite(length) || length < 0) length = 0;
      const out = g.Buffer.alloc(length);
      let offset = 0;
      for (const chunk of list) {
        const src = g.Buffer.isBuffer(chunk) ? chunk : g.Buffer.from(chunk ?? []);
        if (offset >= length) break;
        out.set(src.subarray(0, length - offset), offset);
        offset += src.length;
      }
      return out;
    };
    if (typeof g.Buffer.byteLength !== 'function') g.Buffer.byteLength = (value, encoding = 'utf8') => g.Buffer.from(value, encoding).length;
  }
  if (g.__fxe_hmr == null) {
    const allKey = '*';
    const handlers = Object.create(null);
    const bucket = (path) => {
      const key = String(path);
      return handlers[key] || (handlers[key] = []);
    };
    const accept = (pathOrFn, fn) => {
      if (typeof pathOrFn === 'function' && fn === undefined) {
        bucket(allKey).push(pathOrFn);
        return;
      }
      if (typeof pathOrFn !== 'string' || typeof fn !== 'function') {
        throw new TypeError('__fxe_hmr.accept expects (path, handler) or (handler)');
      }
      bucket(pathOrFn).push(fn);
    };
    const nativeHmr = () => {
      const native = g.__fxe_native && g.__fxe_native.hmr;
      return native && typeof native === 'object' ? native : undefined;
    };
    const invalidate = (path) => {
      const native = nativeHmr();
      if (native && typeof native.invalidate === 'function') {
        const evicted = native.invalidate(String(path));
        return Array.isArray(evicted) ? evicted.map(String) : [];
      }
      return [];
    };
    const reimport = (path) => Promise.resolve().then(() => {
      const key = String(path);
      const native = nativeHmr();
      if (native && typeof native.reimport === 'function') native.reimport(key);
      else if (typeof g.__fxe_hmr_reload === 'function') g.__fxe_hmr_reload(key);
    });
    const fire = (path) => {
      const key = String(path);
      const native = nativeHmr();
      const evicted = invalidate(key);
      let moduleNamespace;
      if (evicted.length !== 0) {
        if (native && typeof native.reimport === 'function') moduleNamespace = native.reimport(key);
        else if (typeof g.__fxe_hmr_reload === 'function') g.__fxe_hmr_reload(key);
      }
      const calls = [...(handlers[key] || []), ...(handlers[allKey] || [])];
      let called = 0;
      for (const handler of calls) {
        ++called;
        try { handler(key, moduleNamespace, evicted); }
        catch (error) { console.error('fxe hmr:', error); }
      }
      if (typeof g.__fxeUiEnsureFrameLoop === 'function') {
        try { g.__fxeUiEnsureFrameLoop(); }
        catch (error) { console.error('fxe hmr:', error); }
      }
      return called;
    };
    const watch = (path) => {
      const key = String(path);
      if (!g.fs || typeof g.fs.watch !== 'function') {
        throw new TypeError('__fxe_hmr.watch requires fs.watch');
      }
      const watcher = g.fs.watch(key, { interval: 50 }, () => fire(key));
      return { close() { if (watcher && typeof watcher.close === 'function') watcher.close(); } };
    };
    Object.defineProperty(g, '__fxe_hmr', {
      configurable: true,
      enumerable: false,
      writable: true,
      value: { handlers, accept, fire, watch, invalidate, reimport },
    });
  }
})();
