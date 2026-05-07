const proc = globalThis.process;
if (!proc || typeof proc !== 'object') {
  throw new Error('host-backed node:process unavailable: process global');
}

if (typeof proc.hrtime === 'function' && typeof proc.hrtime.bigint !== 'function') {
  proc.hrtime.bigint = () => {
    const parts = proc.hrtime();
    return BigInt(parts[0]) * 1000000000n + BigInt(parts[1]);
  };
}
if (!proc.release) {
  proc.release = { name: 'fxe' };
}

export const argv = proc.argv;
export const env = proc.env;
export const stdin = proc.stdin;
export const stdout = proc.stdout;
export const stderr = proc.stderr;
export const versions = proc.versions;
export const release = proc.release;
export const pid = proc.pid;
export const platform = proc.platform;
export const arch = proc.arch;
export const cwd = proc.cwd.bind(proc);
export const chdir = proc.chdir.bind(proc);
export const exit = proc.exit.bind(proc);
export const kill = proc.kill.bind(proc);
export const umask = proc.umask.bind(proc);
export const hrtime = proc.hrtime.bind(proc);
hrtime.bigint = proc.hrtime.bigint.bind(proc.hrtime);
export const nextTick = proc.nextTick.bind(proc);
export const on = proc.on.bind(proc);
export const off = proc.off.bind(proc);
export const addListener = proc.on.bind(proc);
export const removeListener = proc.off.bind(proc);
export const browser = false;
export const title = 'fxe';
export const version = versions?.fxe ?? '0.0.0';
export default proc;
