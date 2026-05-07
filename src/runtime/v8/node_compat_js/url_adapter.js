import * as querystring from 'node:querystring';

const URLCtor = globalThis.URL;
const URLSearchParamsCtor = globalThis.URLSearchParams;

if (typeof URLCtor !== 'function' || typeof URLSearchParamsCtor !== 'function') {
  throw new Error('host-backed node:url unavailable: URL globals');
}

const decodePath = (value) => decodeURIComponent(String(value).replace(/\+/g, '%20'));
const encodePath = (value) => encodeURI(String(value)).replace(/[?#]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);

export const domainToASCII = (domain) => String(domain ?? '');
export const domainToUnicode = (domain) => String(domain ?? '');

export const fileURLToPath = (input) => {
  const url = input instanceof URLCtor ? input : new URLCtor(String(input));
  if (url.protocol !== 'file:') throw new TypeError('URL must use file: protocol');
  const platform = globalThis.process?.platform;
  if (platform === 'win32') {
    const host = url.hostname ? `//${url.hostname}` : '';
    let path = decodePath(url.pathname).replace(/\//g, '\\');
    if (/^\\[a-zA-Z]:/.test(path)) path = path.slice(1);
    return `${host}${path}`;
  }
  if (url.hostname && url.hostname !== 'localhost') throw new TypeError('file URL host must be empty or localhost');
  return decodePath(url.pathname);
};

export const pathToFileURL = (path) => {
  let value = String(path);
  if (globalThis.process?.platform === 'win32') value = value.replace(/\\/g, '/');
  if (!value.startsWith('/')) {
    const cwd = globalThis.process?.cwd?.() ?? '/';
    value = `${cwd.replace(/\\/g, '/')}/${value}`;
  }
  return new URLCtor(`file://${encodePath(value)}`);
};

export const parse = (urlString, parseQueryString = false, slashesDenoteHost = false) => {
  const input = String(urlString ?? '');
  let parsed;
  try {
    parsed = new URLCtor(input);
  } catch {
    const base = slashesDenoteHost || input.startsWith('//') ? 'http://relative.invalid' : 'http://relative.invalid/';
    parsed = new URLCtor(input, base);
  }
  const query = parsed.search.startsWith('?') ? parsed.search.slice(1) : parsed.search;
  const pathname = parsed.pathname || null;
  const auth = parsed.username || parsed.password ? `${decodeURIComponent(parsed.username)}${parsed.password ? `:${decodeURIComponent(parsed.password)}` : ''}` : null;
  const out = {
    protocol: parsed.protocol || null,
    slashes: input.includes('//'),
    auth,
    host: parsed.host || null,
    port: parsed.port || null,
    hostname: parsed.hostname || null,
    hash: parsed.hash || null,
    search: parsed.search || null,
    query: parseQueryString ? querystring.parse(query) : query,
    pathname,
    path: `${pathname ?? ''}${parsed.search || ''}` || null,
    href: parsed.href,
  };
  return out;
};

export const format = (value) => {
  if (typeof value === 'string') return value;
  if (value instanceof URLCtor) return value.href;
  const protocol = value.protocol ? String(value.protocol).replace(/:$/, '') + ':' : '';
  const slashes = value.slashes || value.host || value.hostname ? '//' : '';
  const auth = value.auth ? `${encodeURIComponent(String(value.auth)).replace(/%3A/i, ':')}@` : '';
  const host = value.host ?? `${value.hostname ?? ''}${value.port ? `:${value.port}` : ''}`;
  const pathname = value.pathname ?? '';
  let search = value.search;
  if (search == null && value.query != null) search = typeof value.query === 'string' ? value.query : querystring.stringify(value.query);
  if (search && !String(search).startsWith('?')) search = `?${search}`;
  const hash = value.hash ? (String(value.hash).startsWith('#') ? value.hash : `#${value.hash}`) : '';
  return `${protocol}${slashes}${auth}${host}${pathname}${search ?? ''}${hash}`;
};

export const resolve = (from, to) => new URLCtor(String(to), String(from)).href;
export const resolveObject = (from, to) => parse(resolve(from, to));
export { URLCtor as URL, URLSearchParamsCtor as URLSearchParams };
export default { URL: URLCtor, URLSearchParams: URLSearchParamsCtor, parse, format, resolve, resolveObject, fileURLToPath, pathToFileURL, domainToASCII, domainToUnicode };
