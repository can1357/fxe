
const native = globalThis.fs;
const fdNative = globalThis.__fxe_native?.fs_fd;
const call = (name, args) => {
  if (!native || typeof native[name] !== 'function') {
    throw new Error(`host-backed node:fs/promises function unavailable: ${name}`);
  }
  return native[name](...args);
};
const callFd = (name, args) => {
  if (!fdNative || typeof fdNative[name] !== 'function') {
    throw new Error(`host-backed node:fs/promises fd function unavailable: ${name}`);
  }
  return fdNative[name](...args);
};
export const readFile = (...args) => call('readFile', args);
export const writeFile = (...args) => call('writeFile', args);
export const appendFile = (...args) => call('appendFile', args);
export const stat = (...args) => call('stat', args);
export const readdir = (...args) => call('readdir', args);
export const mkdir = (...args) => call('mkdir', args);
export const rm = (...args) => call('rm', args);
export const rename = (...args) => call('rename', args);
export const realpath = (...args) => call('realpath', args);
export const exists = (...args) => call('exists', args);
export const copyFile = (...args) => call('copyFile', args);
export const cp = (...args) => call('cp', args);
export const symlink = (...args) => call('symlink', args);
export const readlink = (...args) => call('readlink', args);
export const link = (...args) => call('link', args);
export const lstat = (...args) => call('lstat', args);
export const access = (...args) => call('access', args);
export const chmod = (...args) => call('chmod', args);
export const chown = (...args) => call('chown', args);
export const lchmod = (...args) => call('lchmod', args);
export const utimes = (...args) => call('utimes', args);
export const lutimes = (...args) => call('lutimes', args);
export const glob = (...args) => call('glob', args);
export const writeFileAtomic = (...args) => call('writeFileAtomic', args);
export const lock = (...args) => call('lock', args);
export const unlock = (...args) => call('unlock', args);
export const open = (...args) => callFd('open', args).then((result) => result.fd);
export const read = (fd, buffer, offset = 0, length = (buffer?.byteLength ?? buffer?.length ?? 0) - offset, position = null) =>
  callFd('read', [fd, buffer, offset, length, position]).then((result) => ({ bytesRead: result.bytesRead, buffer }));
export const write = (fd, buffer, offset = 0, length = (buffer?.byteLength ?? buffer?.length ?? 0) - offset, position = null) =>
  callFd('write', [fd, buffer, offset, length, position]).then((result) => ({ bytesWritten: result.bytesWritten, buffer }));
export const close = (...args) => callFd('close', args);
export const fstat = (...args) => callFd('fstat', args);
export const ftruncate = (...args) => callFd('ftruncate', args);
export const fdatasync = (...args) => callFd('fdatasync', args);
export default {
  readFile,
  writeFile,
  appendFile,
  stat,
  readdir,
  mkdir,
  rm,
  rename,
  realpath,
  exists,
  copyFile,
  cp,
  symlink,
  readlink,
  link,
  lstat,
  access,
  chmod,
  chown,
  lchmod,
  utimes,
  lutimes,
  glob,
  writeFileAtomic,
  lock,
  unlock,
  open,
  read,
  write,
  close,
  fstat,
  ftruncate,
  fdatasync,
};
