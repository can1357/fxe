import { format } from 'node:util';

const globalConsole = globalThis.console || {};
const bindConsole = (name, fallback = 'log') => (typeof globalConsole[name] === 'function'
  ? globalConsole[name].bind(globalConsole)
  : typeof globalConsole[fallback] === 'function'
    ? globalConsole[fallback].bind(globalConsole)
    : () => {});

export class Console {
  constructor(stdout = undefined, stderr = stdout) {
    this._stdout = stdout;
    this._stderr = stderr ?? stdout;
  }
  _write(stream, args) {
    const line = `${format(...args)}\n`;
    if (stream && typeof stream.write === 'function') stream.write(line);
    else bindConsole('log')(...args);
  }
  log(...args) { this._write(this._stdout, args); }
  info(...args) { this.log(...args); }
  debug(...args) { this.log(...args); }
  warn(...args) { this._write(this._stderr, args); }
  error(...args) { this._write(this._stderr, args); }
  dir(value, options = undefined) { this.log(value, options); }
  time() {}
  timeEnd() {}
  trace(...args) { this.error(...args); }
  assert(condition, ...args) { if (!condition) this.error('Assertion failed:', ...args); }
}

export const log = bindConsole('log');
export const info = bindConsole('info');
export const debug = bindConsole('debug');
export const warn = bindConsole('warn');
export const error = bindConsole('error');
export const dir = bindConsole('dir');
export const trace = bindConsole('trace', 'error');
export const assert = (condition, ...args) => { if (!condition) (globalConsole.assert ? globalConsole.assert(condition, ...args) : error('Assertion failed:', ...args)); };
export const time = bindConsole('time');
export const timeEnd = bindConsole('timeEnd');
export const table = bindConsole('table');
export const group = bindConsole('group');
export const groupEnd = bindConsole('groupEnd');
export const clear = bindConsole('clear');
export const count = bindConsole('count');
export const countReset = bindConsole('countReset');

export default { Console, log, info, debug, warn, error, dir, trace, assert, time, timeEnd, table, group, groupEnd, clear, count, countReset };
