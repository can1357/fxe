import { setTimeout as callbackSetTimeout, setImmediate as callbackSetImmediate } from 'node:timers';

const abortError = (signal) => signal?.reason ?? new Error('The operation was aborted');
const checkSignal = (signal, reject) => {
  if (!signal) return undefined;
  if (signal.aborted) {
    reject(abortError(signal));
    return undefined;
  }
  const onAbort = () => reject(abortError(signal));
  signal.addEventListener('abort', onAbort, { once: true });
  return () => signal.removeEventListener('abort', onAbort);
};

export const setTimeout = (delay = 1, value = undefined, options = undefined) => new Promise((resolve, reject) => {
  let cleanup;
  const id = callbackSetTimeout(() => { cleanup?.(); resolve(value); }, Number(delay) || 0);
  cleanup = checkSignal(options?.signal, (error) => { clearTimeout(id); reject(error); });
});

export const setImmediate = (value = undefined, options = undefined) => new Promise((resolve, reject) => {
  let cleanup;
  const id = callbackSetImmediate(() => { cleanup?.(); resolve(value); });
  cleanup = checkSignal(options?.signal, (error) => { clearTimeout(id); reject(error); });
});

export async function* setInterval(delay = 1, value = undefined, options = undefined) {
  const signal = options?.signal;
  while (!signal?.aborted) {
    await setTimeout(delay, undefined, options);
    if (!signal?.aborted) yield value;
  }
}

export const scheduler = {
  wait: setTimeout,
  yield: () => setImmediate(),
};

export default { setTimeout, setImmediate, setInterval, scheduler };
