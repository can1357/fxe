
const native = globalThis.__fxe_native?.os;
const call = (name) => {
  if (!native || typeof native[name] !== 'function') {
    throw new Error(`host-backed node:os function unavailable: ${name}`);
  }
  return native[name]();
};
export const platform = () => call('platform');
export const arch = () => call('arch');
export const release = () => call('release');
export const type = () => call('type');
export const endianness = () => call('endianness');
export const homedir = () => call('homedir');
export const tmpdir = () => call('tmpdir');
export const hostname = () => call('hostname');
export const uptime = () => call('uptime');
export const totalmem = () => call('totalmem');
export const freemem = () => call('freemem');
export const cpus = () => call('cpus');
export const networkInterfaces = () => call('networkInterfaces');
export const userInfo = () => call('userInfo');
export default {
  platform,
  arch,
  release,
  type,
  endianness,
  homedir,
  tmpdir,
  hostname,
  uptime,
  totalmem,
  freemem,
  cpus,
  networkInterfaces,
  userInfo,
};
