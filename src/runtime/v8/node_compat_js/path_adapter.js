const assertPath = (path) => {
  if (typeof path !== 'string') throw new TypeError('path must be a string');
};
const filter = (parts) => parts.filter((part) => {
  assertPath(part);
  return part.length !== 0;
});

const normalizeString = (path, sep, isAbs, allowAboveRoot) => {
  const segments = path.split(/[\\/]+/);
  const out = [];
  for (const segment of segments) {
    if (!segment || segment === '.') continue;
    if (segment === '..') {
      if (out.length !== 0 && out[out.length - 1] !== '..') out.pop();
      else if (allowAboveRoot && !isAbs) out.push('..');
    } else {
      out.push(segment);
    }
  }
  return out.join(sep);
};

const splitDevice = (path) => {
  const p = String(path).replace(/\//g, '\\');
  const m = /^[a-zA-Z]:/.exec(p);
  if (m) return { device: m[0], rest: p.slice(2), unc: false };
  if (p.startsWith('\\\\')) {
    const parts = p.slice(2).split('\\');
    if (parts[0] && parts[1]) {
      const device = `\\\\${parts[0]}\\${parts[1]}`;
      return { device, rest: p.slice(device.length), unc: true };
    }
  }
  return { device: '', rest: p, unc: false };
};

const makePosix = () => {
  const api = {
    sep: '/',
    delimiter: ':',
    isAbsolute(path) { assertPath(path); return path.startsWith('/'); },
    normalize(path) {
      assertPath(path);
      if (path.length === 0) return '.';
      const abs = api.isAbsolute(path);
      const trailing = path.endsWith('/');
      let out = normalizeString(path, '/', abs, true);
      if (!out && !abs) out = '.';
      if (out && trailing) out += '/';
      return `${abs ? '/' : ''}${out}`;
    },
    join(...paths) {
      const joined = filter(paths).join('/');
      return joined ? api.normalize(joined) : '.';
    },
    resolve(...paths) {
      let resolved = '';
      let abs = false;
      for (let i = paths.length - 1; i >= -1 && !abs; --i) {
        const part = i >= 0 ? paths[i] : globalThis.process?.cwd?.() ?? '/';
        assertPath(part);
        if (part.length === 0) continue;
        resolved = `${part}/${resolved}`;
        abs = part.startsWith('/');
      }
      const out = normalizeString(resolved, '/', abs, false);
      return `${abs ? '/' : ''}${out}` || '.';
    },
    relative(from, to) {
      assertPath(from); assertPath(to);
      const f = api.resolve(from).slice(1).split('/').filter(Boolean);
      const t = api.resolve(to).slice(1).split('/').filter(Boolean);
      let i = 0;
      while (i < f.length && i < t.length && f[i] === t[i]) ++i;
      return [...Array(f.length - i).fill('..'), ...t.slice(i)].join('/');
    },
    dirname(path) {
      assertPath(path);
      if (path.length === 0) return '.';
      const root = path.startsWith('/');
      const stripped = path.replace(/\/+$/, '') || (root ? '/' : '');
      const idx = stripped.lastIndexOf('/');
      if (idx < 0) return '.';
      if (idx === 0) return root ? '/' : '.';
      return stripped.slice(0, idx);
    },
    basename(path, suffix = '') {
      assertPath(path);
      let base = path.replace(/\/+$/, '').split('/').pop() ?? '';
      if (suffix && base.endsWith(String(suffix))) base = base.slice(0, -String(suffix).length);
      return base;
    },
    extname(path) {
      const base = api.basename(path);
      const idx = base.lastIndexOf('.');
      return idx > 0 ? base.slice(idx) : '';
    },
    parse(path) {
      assertPath(path);
      const root = api.isAbsolute(path) ? '/' : '';
      const dir = api.dirname(path);
      const base = api.basename(path);
      const ext = api.extname(path);
      return { root, dir: dir === '.' ? '' : dir, base, ext, name: ext ? base.slice(0, -ext.length) : base };
    },
    format(obj) {
      const dir = obj.dir || obj.root || '';
      const base = obj.base || `${obj.name || ''}${obj.ext || ''}`;
      return dir ? `${dir}${dir.endsWith('/') ? '' : '/'}${base}` : base;
    },
    toNamespacedPath(path) { return path; },
  };
  return api;
};

const makeWin32 = () => {
  const api = {
    sep: '\\',
    delimiter: ';',
    isAbsolute(path) {
      assertPath(path);
      const p = path.replace(/\//g, '\\');
      return /^([a-zA-Z]:)?\\/.test(p) || p.startsWith('\\\\');
    },
    normalize(path) {
      assertPath(path);
      if (path.length === 0) return '.';
      const { device, rest } = splitDevice(path);
      const abs = api.isAbsolute(path);
      const trailing = /[\\/]$/.test(path);
      let out = normalizeString(rest, '\\', abs, true);
      if (!out && !abs) out = '.';
      if (out && trailing) out += '\\';
      return `${device}${abs && !out.startsWith('\\') ? '\\' : ''}${out}`;
    },
    join(...paths) {
      const joined = filter(paths).join('\\');
      return joined ? api.normalize(joined) : '.';
    },
    resolve(...paths) {
      let resolved = '';
      let device = '';
      let abs = false;
      for (let i = paths.length - 1; i >= -1 && !abs; --i) {
        const part = i >= 0 ? paths[i] : globalThis.process?.cwd?.() ?? '\\';
        assertPath(part);
        if (!part) continue;
        const split = splitDevice(part);
        if (!device) device = split.device;
        resolved = `${split.rest}\\${resolved}`;
        abs = api.isAbsolute(part);
      }
      const out = normalizeString(resolved, '\\', abs, false);
      return `${device}${abs ? '\\' : ''}${out}` || '.';
    },
    relative(from, to) {
      const f = api.resolve(from).toLowerCase().split('\\').filter(Boolean);
      const tResolved = api.resolve(to);
      const t = tResolved.toLowerCase().split('\\').filter(Boolean);
      let i = 0;
      while (i < f.length && i < t.length && f[i] === t[i]) ++i;
      return [...Array(f.length - i).fill('..'), ...tResolved.split('\\').filter(Boolean).slice(i)].join('\\');
    },
    dirname(path) {
      assertPath(path);
      const p = path.replace(/\//g, '\\').replace(/\\+$/, '');
      const { device, rest } = splitDevice(p);
      const idx = rest.lastIndexOf('\\');
      if (idx < 0) return device || '.';
      if (idx === 0) return `${device}\\`;
      return `${device}${rest.slice(0, idx)}`;
    },
    basename(path, suffix = '') {
      assertPath(path);
      let base = path.replace(/\//g, '\\').replace(/\\+$/, '').split('\\').pop() ?? '';
      if (suffix && base.endsWith(String(suffix))) base = base.slice(0, -String(suffix).length);
      return base;
    },
    extname(path) {
      const base = api.basename(path);
      const idx = base.lastIndexOf('.');
      return idx > 0 ? base.slice(idx) : '';
    },
    parse(path) {
      assertPath(path);
      const { device } = splitDevice(path);
      const root = api.isAbsolute(path) ? `${device}${device && !device.endsWith('\\') ? '\\' : ''}` || '\\' : '';
      const dir = api.dirname(path);
      const base = api.basename(path);
      const ext = api.extname(path);
      return { root, dir: dir === '.' ? '' : dir, base, ext, name: ext ? base.slice(0, -ext.length) : base };
    },
    format(obj) {
      const dir = obj.dir || obj.root || '';
      const base = obj.base || `${obj.name || ''}${obj.ext || ''}`;
      return dir ? `${dir}${/[\\/]$/.test(dir) ? '' : '\\'}${base}` : base;
    },
    toNamespacedPath(path) {
      assertPath(path);
      const p = api.resolve(path);
      return p.startsWith('\\\\?\\') ? p : `\\\\?\\${p}`;
    },
  };
  return api;
};

export const posix = makePosix();
export const win32 = makeWin32();
const hostIsWin = globalThis.process?.platform === 'win32' || globalThis.path?.sep === '\\';
const host = hostIsWin ? win32 : posix;
export const sep = host.sep;
export const delimiter = host.delimiter;
export const resolve = host.resolve;
export const normalize = host.normalize;
export const isAbsolute = host.isAbsolute;
export const join = host.join;
export const relative = host.relative;
export const dirname = host.dirname;
export const basename = host.basename;
export const extname = host.extname;
export const parse = host.parse;
export const format = host.format;
export const toNamespacedPath = host.toNamespacedPath;
export default host;
