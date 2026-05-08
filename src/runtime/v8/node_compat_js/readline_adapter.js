// Minimal node:readline adapter. Real Node has many more knobs; this covers the
// 80% case used by simple CLIs over process.stdin/stdout.
//
// Supports: createInterface({ input, output, terminal?, prompt? }), Interface
// with question/prompt/close/setPrompt/getPrompt/write/pause/resume,
// 'line', 'close', 'pause', 'resume' events, async iterator.
// Does NOT support: history persistence, completer, raw-keypress, cursor mgmt.

import { EventEmitter } from 'node:events';

const process = globalThis.process || {};

export class Interface extends EventEmitter {
  constructor(opts = {}) {
    super();
    this._input = opts.input || process.stdin;
    this._output = opts.output || process.stdout;
    this._terminal = Boolean(opts.terminal);
    this._prompt = opts.prompt ?? '> ';
    this._buffer = '';
    this._closed = false;
    this._questionResolve = null;
    this._lineQueue = [];
    this._readers = [];
    this._wireInput();
  }

  _wireInput() {
    if (!this._input || typeof this._input.on !== 'function') return;
    this._input.on('data', (chunk) => {
      const text = typeof chunk === 'string' ? chunk : (chunk?.toString?.('utf8') ?? '');
      this._buffer += text;
      let idx;
      while ((idx = this._buffer.indexOf('\n')) !== -1) {
        const line = this._buffer.slice(0, idx).replace(/\r$/, '');
        this._buffer = this._buffer.slice(idx + 1);
        this._emitLine(line);
      }
    });
    this._input.on('end', () => this.close());
    this._input.on('close', () => this.close());
  }

  _emitLine(line) {
    if (this._questionResolve) {
      const resolve = this._questionResolve;
      this._questionResolve = null;
      resolve(line);
      return;
    }
    if (this._readers.length > 0) {
      const next = this._readers.shift();
      next({ value: line, done: false });
      return;
    }
    this._lineQueue.push(line);
    this.emit('line', line);
  }

  prompt(_preserveCursor) {
    if (this._closed) return;
    if (this._output && typeof this._output.write === 'function') this._output.write(this._prompt);
  }

  setPrompt(prompt) { this._prompt = prompt; }

  getPrompt() { return this._prompt; }

  question(query, cb) {
    if (this._output && typeof this._output.write === 'function') this._output.write(query);
    if (typeof cb === 'function') {
      this._questionResolve = cb;
      return undefined;
    }
    return new Promise((resolve) => {
      this._questionResolve = resolve;
    });
  }

  write(data) {
    if (this._output && typeof this._output.write === 'function') this._output.write(data);
  }

  pause() {
    this._input?.pause?.();
    this.emit('pause');
  }

  resume() {
    this._input?.resume?.();
    this.emit('resume');
  }

  close() {
    if (this._closed) return;
    this._closed = true;
    while (this._readers.length > 0) this._readers.shift()({ value: undefined, done: true });
    this.emit('close');
  }

  [Symbol.asyncIterator]() {
    const self = this;
    return {
      next() {
        if (self._lineQueue.length > 0) return Promise.resolve({ value: self._lineQueue.shift(), done: false });
        if (self._closed) return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve) => {
          self._readers.push(resolve);
        });
      },
    };
  }
}

export function createInterface(opts) {
  return new Interface(opts);
}

const api = { Interface, createInterface };
export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
