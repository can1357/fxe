const bind = (name) => {
  const fn = globalThis[name];
  if (typeof fn !== 'function') throw new Error(`host-backed node:timers function unavailable: ${name}`);
  return fn.bind(globalThis);
};

export const setTimeout = bind('setTimeout');
export const clearTimeout = bind('clearTimeout');
export const setInterval = bind('setInterval');
export const clearInterval = bind('clearInterval');
export const setImmediate = typeof globalThis.setImmediate === 'function' ? globalThis.setImmediate.bind(globalThis) : (fn, ...args) => setTimeout(fn, 0, ...args);
export const clearImmediate = typeof globalThis.clearImmediate === 'function' ? globalThis.clearImmediate.bind(globalThis) : clearTimeout;
export const queueMicrotask = bind('queueMicrotask');
export default { setTimeout, clearTimeout, setInterval, clearInterval, setImmediate, clearImmediate, queueMicrotask };
