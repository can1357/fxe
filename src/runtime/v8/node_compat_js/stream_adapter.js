import { EventEmitter } from 'node:events';
import { Buffer } from 'node:buffer';

const asBuffer = (chunk) => typeof chunk === 'string' ? Buffer.from(chunk) : Buffer.from(chunk ?? []);

export class Writable extends EventEmitter {
  constructor(options = {}) {
    super();
    this.writableEnded = false;
    this.chunks = [];
    this._writeImpl = typeof options.write === 'function' ? options.write : undefined;
    this._finalImpl = typeof options.final === 'function' ? options.final : undefined;
  }
  write(chunk, encoding = undefined, callback = undefined) {
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    if (this.writableEnded) throw new Error('write after end');
    const cb = typeof callback === 'function' ? callback : () => {};
    if (this._writeImpl) this._writeImpl(chunk, encoding, cb);
    else { this.chunks.push(chunk); cb(); }
    this.emit('drain');
    return true;
  }
  end(chunk = undefined, encoding = undefined, callback = undefined) {
    if (typeof chunk === 'function') { callback = chunk; chunk = undefined; encoding = undefined; }
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    if (chunk !== undefined) this.write(chunk, encoding);
    this.writableEnded = true;
    const done = () => { this.emit('finish'); this.emit('close'); callback?.(); };
    if (this._finalImpl) this._finalImpl(done);
    else done();
    return this;
  }
}

export class Readable extends EventEmitter {
  constructor(options = {}) {
    super();
    this.readableEnded = false;
    this._queue = [];
    this._encoding = null;
    this._readImpl = typeof options.read === 'function' ? options.read : undefined;
  }
  static from(iterable) {
    const readable = new Readable();
    queueMicrotask(async () => {
      try {
        for await (const chunk of iterable) readable.push(chunk);
        readable.push(null);
      } catch (error) {
        readable.destroy(error);
      }
    });
    return readable;
  }
  setEncoding(encoding) { this._encoding = String(encoding ?? 'utf8').toLowerCase(); return this; }
  push(chunk) {
    if (chunk === null) {
      this.readableEnded = true;
      this.emit('end');
      this.emit('close');
      return false;
    }
    const value = this._encoding && chunk && typeof chunk !== 'string' ? Buffer.from(chunk).toString(this._encoding) : chunk;
    this._queue.push(value);
    this.emit('data', value);
    return true;
  }
  read() { return this._queue.shift() ?? null; }
  resume() { if (this._readImpl) this._readImpl(); return this; }
  pause() { return this; }
  pipe(dest) {
    this.on('data', (chunk) => dest.write(chunk));
    this.on('end', () => dest.end?.());
    this.on('error', (error) => dest.emit?.('error', error));
    return dest;
  }
  destroy(error = undefined) {
    if (error) this.emit('error', error);
    this.readableEnded = true;
    this.emit('close');
    return this;
  }
  async *[Symbol.asyncIterator]() {
    while (!this.readableEnded || this._queue.length !== 0) {
      const chunk = this.read();
      if (chunk !== null) yield chunk;
      else await new Promise((resolve) => {
        const done = () => { cleanup(); resolve(); };
        const cleanup = () => { this.off('data', done); this.off('end', done); this.off('error', done); };
        this.once('data', done); this.once('end', done); this.once('error', done);
      });
    }
  }
}

export class Transform extends Readable {
  constructor(options = {}) {
    super(options);
    this.writableEnded = false;
    this._transformImpl = typeof options.transform === 'function' ? options.transform : undefined;
  }
  write(chunk, encoding = undefined, callback = undefined) {
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    const cb = typeof callback === 'function' ? callback : () => {};
    if (this._transformImpl) this._transformImpl(chunk, encoding, (error, data) => {
      if (error) this.destroy(error);
      else if (data !== undefined) this.push(data);
      cb(error);
    });
    else { this.push(chunk); cb(); }
    return true;
  }
  end(chunk = undefined, encoding = undefined, callback = undefined) {
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    if (chunk !== undefined) this.write(chunk, encoding);
    this.writableEnded = true;
    this.push(null);
    callback?.();
    return this;
  }
}

export class Duplex extends Transform {}
export class PassThrough extends Transform {}

export const finished = (stream, options = undefined, callback = undefined) => {
  if (typeof options === 'function') { callback = options; options = undefined; }
  const promise = new Promise((resolve, reject) => {
    const done = (error) => {
      cleanup();
      if (callback) callback(error);
      error ? reject(error) : resolve();
    };
    const cleanup = () => {
      stream.off?.('end', onDone); stream.off?.('finish', onDone); stream.off?.('close', onDone); stream.off?.('error', onError);
    };
    const onDone = () => done();
    const onError = (error) => done(error);
    stream.once?.('end', onDone); stream.once?.('finish', onDone); stream.once?.('close', onDone); stream.once?.('error', onError);
    if (stream.readableEnded || stream.writableEnded) queueMicrotask(onDone);
  });
  return callback ? stream : promise;
};

export const pipeline = (...streams) => {
  const callback = typeof streams[streams.length - 1] === 'function' ? streams.pop() : undefined;
  if (streams.length < 2) throw new TypeError('pipeline requires at least two streams');
  for (let i = 0; i < streams.length - 1; ++i) streams[i].pipe(streams[i + 1]);
  const last = streams[streams.length - 1];
  const promise = finished(last).then(() => last);
  if (callback) promise.then((value) => callback(null, value), callback);
  return callback ? last : promise;
};

export const promises = { finished, pipeline };
export default { Readable, Writable, Transform, Duplex, PassThrough, finished, pipeline, promises };
