const native = (globalThis.__fxe_native && globalThis.__fxe_native.inspector) || null;
const EventEmitter = (await import('node:events')).EventEmitter;

class Session extends EventEmitter {
  constructor() { super(); this._connected = false; }
  connect() { this._connected = true; }
  connectToMainThread() { this._connected = true; }
  disconnect() { this._connected = false; }
  post(method, params, callback) {
    if (typeof params === 'function') { callback = params; params = undefined; }
    if (!this._connected) {
      const e = new Error('Session is not connected'); e.code = 'ERR_INSPECTOR_NOT_CONNECTED';
      if (callback) return queueMicrotask(() => callback(e)); throw e;
    }
    if (!native) {
      const e = new Error('fxe: __fxe_native.inspector not installed'); e.code = 'ERR_FXE_INSPECTOR_UNAVAILABLE';
      if (callback) return queueMicrotask(() => callback(e)); throw e;
    }
    const paramsJson = params ? JSON.stringify(params) : '{}';
    native.dispatch(method, paramsJson).then(resultJson => {
      try { const r = JSON.parse(resultJson || 'null'); if (callback) callback(null, r); }
      catch (e) { if (callback) callback(e); }
    }, err => { if (callback) callback(err); else queueMicrotask(() => { throw err; }); });
  }
}

function open() { /* server already running; return immediately */ }
function close() { /* keep running */ }
function url() { return native ? native.serverUrl() : undefined; }
function waitForDebugger() { return native ? native.serverWaitForConnect() : Promise.resolve(); }
const console_ = { error: (...a) => globalThis.console?.error?.(...a), log: (...a) => globalThis.console?.log?.(...a), warn: (...a) => globalThis.console?.warn?.(...a) };

module.exports = { Session, open, close, url, waitForDebugger, console: console_ };
module.exports.default = module.exports;
