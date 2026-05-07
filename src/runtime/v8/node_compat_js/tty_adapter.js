
const native = globalThis.__fxe_native?.tty;
export const isatty = (fd) => {
  if (!native || typeof native.isatty !== 'function') {
    throw new Error('host-backed node:tty function unavailable: isatty');
  }
  return native.isatty(fd);
};
export const getWindowSize = (fd) => {
  if (!native || typeof native.getWindowSize !== 'function') {
    throw new Error('host-backed node:tty function unavailable: getWindowSize');
  }
  return native.getWindowSize(fd);
};
export default { isatty, getWindowSize };
