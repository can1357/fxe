const native = globalThis.__fxe_native?.vm;

if (!native || typeof native !== 'object') {
  throw new Error('fxe: __fxe_native.vm is not installed');
}

function unsupported(method) {
  return Promise.reject(new Error(`fxe: node:vm.${method} is not supported`));
}

export class Script {
  constructor(code, options) {
    this._compiled = native.compile(String(code), options);
  }

  runInThisContext(options) {
    return this._compiled.runInThisContext(options);
  }

  runInContext(contextifiedObject, options) {
    return this._compiled.runInContext(contextifiedObject, options);
  }

  runInNewContext(contextObject, options) {
    return this._compiled.runInNewContext(contextObject, options);
  }
}

export function createContext(sandbox = {}) {
  return native.createContext(sandbox);
}

export function isContext(obj) {
  return native.isContext(obj);
}

export function runInThisContext(code, options) {
  return native.runInThisContext(String(code), options);
}

export function runInContext(code, contextifiedObject, options) {
  return native.runInContext(contextifiedObject, String(code), options);
}

export function runInNewContext(code, contextObject = {}, options) {
  return native.runInNewContext(String(code), contextObject, options);
}

export function compileFunction(code, params = [], options) {
  return native.compileFunction(String(code), Array.isArray(params) ? params.map(String) : [], options);
}

export function measureMemory() {
  return unsupported('measureMemory');
}

const api = {
  Script,
  createContext,
  isContext,
  runInThisContext,
  runInContext,
  runInNewContext,
  compileFunction,
  measureMemory,
};

export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
